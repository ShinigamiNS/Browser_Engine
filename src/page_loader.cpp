#include "page_loader.h"
#include "browser_ui.h"
#include "css_parser.h"
#include "dom_builder.h"
#include "image_cache.h"
#include "layout.h"
#include "lexbor_adapter.h"
#include "net.h"
#include "paint.h"
#include "quickjs_adapter.h"
#include "style.h"
#include "svg_rasterizer.h"
#include "utils.h"
#include <algorithm>
#include <atomic>
#include <functional>
#include <iostream>
#include <string>
#include <vector>
#include <windows.h>

// ── Globals defined elsewhere (main.cpp) ─────────────────────────────────────
extern bool app_initialized;
extern HWND g_hwnd;
extern int buffer_width;
extern BrowserUI browser_ui;

// ── Loading guard — prevents concurrent/recursive page loads ─────────────────
static std::atomic<bool> g_load_in_progress{false};

// ── Page globals defined here ─────────────────────────────────────────────────
std::shared_ptr<LayoutBox> global_layout_root = nullptr;
DisplayList master_display_list;
std::string g_current_page_url;
int g_viewport_height = 600;
int g_viewport_width  = 1440;

// CSS stylesheets for the current page
static std::shared_ptr<Node> g_dom_root;
static Stylesheet g_main_stylesheet;
static Stylesheet g_hover_stylesheet;
static Stylesheet g_focus_stylesheet;
static QJSEngine* g_qjs_engine = nullptr;

std::vector<ScrollContainer> g_scroll_containers;

// ── URL helpers ───────────────────────────────────────────────────────────────

static std::string resolve_url(const std::string &src,
                               const std::string &page_url) {
  if (src.empty())
    return "";
  if (src.substr(0, 8) == "https://" || src.substr(0, 7) == "http://")
    return src;
  if (src.substr(0, 2) == "//")
    return "https:" + src;
  if (src[0] == '/') {
    size_t scheme_end = page_url.find("://");
    if (scheme_end == std::string::npos)
      return "";
    size_t host_end = page_url.find('/', scheme_end + 3);
    std::string origin = (host_end == std::string::npos)
                             ? page_url
                             : page_url.substr(0, host_end);
    return "https://" + origin.substr(origin.find("://") + 3) + src;
  }
  size_t last_slash = page_url.rfind('/');
  if (last_slash == std::string::npos)
    return src;
  return page_url.substr(0, last_slash + 1) + src;
}

static bool is_undecoded_format(const std::string &url) {
  std::string path = url;
  size_t q = path.find('?');
  if (q != std::string::npos) path = path.substr(0, q);
  if (path.size() >= 4) {
    std::string ext = path.substr(path.size() - 4);
    for (char &c : ext) c = (char)::tolower((unsigned char)c);
    if (ext == ".svg" || ext == ".svgz") return true;
    if (ext == ".webp") return true;
    if (ext == ".avif") return true;
  }
  if (path.size() >= 5) {
    std::string ext5 = path.substr(path.size() - 5);
    for (char &c : ext5) c = (char)::tolower((unsigned char)c);
    if (ext5 == ".svgz") return true;
  }
  return false;
}

static std::string pick_srcset_url(const std::string &srcset) {
  if (srcset.empty()) return "";
  std::string best_url;
  float best_val = -1.f;
  size_t pos = 0;
  while (pos < srcset.size()) {
    size_t comma = srcset.find(',', pos);
    std::string entry = (comma == std::string::npos)
                          ? srcset.substr(pos)
                          : srcset.substr(pos, comma - pos);
    pos = (comma == std::string::npos) ? srcset.size() : comma + 1;
    size_t s = entry.find_first_not_of(" \t");
    if (s == std::string::npos) continue;
    entry = entry.substr(s);
    size_t last_sp = entry.rfind(' ');
    std::string cand_url, desc;
    if (last_sp != std::string::npos) {
      cand_url = entry.substr(0, last_sp);
      desc     = entry.substr(last_sp + 1);
    } else {
      cand_url = entry;
    }
    float val = 1.f;
    if (!desc.empty()) {
      try { val = std::stof(desc); } catch (...) { val = 1.f; }
    }
    if (val > best_val && !cand_url.empty()) {
      best_val = val;
      best_url = cand_url;
    }
  }
  return best_url;
}

void collect_img_urls(const std::shared_ptr<LayoutBox> &box,
                      const std::string &page_url,
                      std::vector<std::string> &out) {
  if (!box)
    return;
  if (box->style_node && box->style_node->node &&
      box->style_node->node->type == NodeType::Element &&
      box->style_node->node->data == "img") {
    auto &attrs = box->style_node->node->attributes;
    std::string chosen_src;
    auto ss_it = attrs.find("srcset");
    if (ss_it != attrs.end() && !ss_it->second.empty())
      chosen_src = pick_srcset_url(ss_it->second);
    if (chosen_src.empty()) {
      auto src_it = attrs.find("src");
      if (src_it != attrs.end()) chosen_src = src_it->second;
    }
    if (!chosen_src.empty()) {
      if (chosen_src.size() >= 6 && chosen_src.substr(0, 6) == "__svg_") {
        // Already rasterized into the image cache — nothing to fetch.
      } else if (chosen_src.size() > 5 && chosen_src.substr(0, 5) == "data:") {
        // Queue ALL data: URIs (including image/svg+xml). The fetch thread
        // routes SVG payloads through rasterize_svg() and bitmap payloads
        // through decode_image_bytes() — both end up in g_image_cache under
        // the original data: URI key.
        out.push_back(chosen_src);
      } else {
        std::string url = resolve_url(chosen_src, page_url);
        bool is_http = (!url.empty() &&
                        (url.substr(0, 8) == "https://" ||
                         url.substr(0, 7) == "http://"));
        if (is_http && !is_undecoded_format(url))
          out.push_back(url);
      }
    }
  }
  // Also collect CSS background-image URLs
  if (box->style_node) {
    std::string bgi = box->style_node->value("background-image");
    if (!bgi.empty() && bgi.find("url(") != std::string::npos) {
      size_t s = bgi.find("url(") + 4;
      size_t e = bgi.find(')', s);
      if (e != std::string::npos) {
        std::string bg_url = bgi.substr(s, e - s);
        if (!bg_url.empty() && (bg_url.front() == '"' || bg_url.front() == '\''))
          bg_url = bg_url.substr(1, bg_url.size() - 2);
        if (!bg_url.empty()) {
          if (bg_url.size() > 5 && bg_url.substr(0, 5) == "data:") {
            out.push_back(bg_url);
          } else {
            std::string resolved = resolve_url(bg_url, page_url);
            bool is_http = (!resolved.empty() &&
                            (resolved.substr(0, 8) == "https://" ||
                             resolved.substr(0, 7) == "http://"));
            if (is_http)
              out.push_back(resolved);
          }
        }
      }
    }
  }
  for (auto &child : box->children)
    collect_img_urls(child, page_url, out);
}

// ── Scroll containers ─────────────────────────────────────────────────────────

std::vector<ScrollContainer> compute_scroll_containers(
    const DisplayList &dl, const std::vector<ScrollContainer> &prev) {
  std::vector<ScrollContainer> result;
  for (const auto &cmd : dl) {
    if (cmd.type == DisplayCommandType::ClipPush && cmd.clip_scrollable) {
      ScrollContainer sc;
      sc.bounds = cmd.rect;
      sc.content_height = cmd.clip_content_height;
      sc.scroll_y = 0.f;
      for (const auto &p : prev) {
        if (std::abs(p.bounds.x - sc.bounds.x) < 1.f &&
            std::abs(p.bounds.y - sc.bounds.y) < 1.f &&
            std::abs(p.bounds.width - sc.bounds.width) < 1.f) {
          sc.scroll_y = p.scroll_y;
          break;
        }
      }
      result.push_back(sc);
    }
  }
  return result;
}

void rebuild_scroll_containers() {
  g_scroll_containers = compute_scroll_containers(master_display_list, g_scroll_containers);
}

// ── Image fetch thread ────────────────────────────────────────────────────────

DWORD WINAPI fetch_images_thread(LPVOID param) {
  ImgFetchParams *p = (ImgFetchParams *)param;
  std::vector<std::string> urls = p->urls;
  HWND hwnd = p->hwnd;
  delete p;

  for (size_t i = 0; i < urls.size(); ++i) {
    const std::string &url = urls[i];
    {
      EnterCriticalSection(&g_image_cache_cs);
      bool cached = (g_image_cache.count(url) > 0);
      LeaveCriticalSection(&g_image_cache_cs);
      if (cached)
        continue;
    }

    std::string bytes;
    if (url.size() > 5 && url.substr(0, 5) == "data:") {
      // Check for SVG data URI — needs rasterization, not bitmap decode
      if (url.find("image/svg") != std::string::npos) {
        std::string payload = decode_data_uri(url);
        // URL-decode percent-encoded characters
        std::string svg_xml;
        svg_xml.reserve(payload.size());
        for (size_t pi = 0; pi < payload.size(); ++pi) {
          if (payload[pi] == '%' && pi + 2 < payload.size()) {
            int hi = 0, lo = 0;
            char c1 = payload[pi+1], c2 = payload[pi+2];
            if (c1 >= '0' && c1 <= '9') hi = c1 - '0';
            else if (c1 >= 'A' && c1 <= 'F') hi = c1 - 'A' + 10;
            else if (c1 >= 'a' && c1 <= 'f') hi = c1 - 'a' + 10;
            if (c2 >= '0' && c2 <= '9') lo = c2 - '0';
            else if (c2 >= 'A' && c2 <= 'F') lo = c2 - 'A' + 10;
            else if (c2 >= 'a' && c2 <= 'f') lo = c2 - 'a' + 10;
            svg_xml += (char)((hi << 4) | lo);
            pi += 2;
          } else {
            svg_xml += payload[pi];
          }
        }
        std::string key = rasterize_svg(svg_xml, 0, 0);
        if (!key.empty()) {
          // Map the data URI to the rasterized cache entry
          EnterCriticalSection(&g_image_cache_cs);
          if (g_image_cache.count(key)) {
            g_image_cache[url] = g_image_cache[key];
          }
          LeaveCriticalSection(&g_image_cache_cs);
          PostMessage(hwnd, WM_USER + 1, 0, 0);
        }
        continue;
      }
      bytes = decode_data_uri(url);
    } else {
      bytes = fetch_https(url);
    }

    CachedImage img;
    if (decode_image_bytes(bytes, img)) {
      EnterCriticalSection(&g_image_cache_cs);
      g_image_cache[url] = img;
      LeaveCriticalSection(&g_image_cache_cs);
      PostMessage(hwnd, WM_USER + 1, 0, 0);
    } else {
      if (!bytes.empty()) {
        EnterCriticalSection(&g_image_cache_cs);
        g_image_cache[url] = CachedImage();
        LeaveCriticalSection(&g_image_cache_cs);
      }
    }
  }
  return 0;
}

// ── SVG helpers ───────────────────────────────────────────────────────────────

// Extract top-level SVG strings from raw HTML text (before html.clear())
static std::vector<std::string> extract_raw_svgs(const std::string& html) {
    std::vector<std::string> result;
    size_t pos = 0;
    while (pos < html.size()) {
        size_t s = html.find("<svg", pos);
        if (s == std::string::npos) break;
        // Must be <svg> or <svg ... (not <svgfoo>)
        size_t a = s + 4;
        if (a < html.size()) {
            char c = html[a];
            if (c != '>' && c != ' ' && c != '\n' && c != '\r' && c != '\t' && c != '/') {
                pos = a; continue;
            }
        }
        // Track nesting depth to find matching </svg>
        int depth = 0;
        size_t cur = s;
        size_t end = std::string::npos;
        while (cur < html.size()) {
            size_t open  = html.find("<svg", cur);
            size_t close = html.find("</svg>", cur);
            if (close == std::string::npos) break;
            bool open_valid = false;
            if (open != std::string::npos && open < close) {
                size_t oa = open + 4;
                if (oa >= html.size()) open_valid = true;
                else { char oc = html[oa]; open_valid = (oc=='>'||oc==' '||oc=='\n'||oc=='\r'||oc=='\t'||oc=='/'); }
            }
            if (open_valid && open < close) {
                depth++; cur = open + 4;
            } else {
                depth--; cur = close + 6;
                if (depth == 0) { end = cur; break; }
            }
        }
        if (end != std::string::npos) {
            result.push_back(html.substr(s, end - s));
            pos = end;
        } else {
            break;
        }
    }
    return result;
}

// Collect top-level <svg> DOM nodes in document order
static void collect_svg_nodes(const std::shared_ptr<Node>& node,
                               std::vector<std::shared_ptr<Node>>& out) {
    if (!node) return;
    if (node->type == NodeType::Element && node->data == "svg") {
        out.push_back(node);
        return; // don't recurse into nested SVGs
    }
    for (auto& child : node->children)
        collect_svg_nodes(child, out);
}

// Replace old_node with new_node in its parent's children list
static void replace_dom_node(const std::shared_ptr<Node>& old_node,
                              const std::shared_ptr<Node>& new_node) {
    auto parent = old_node->parent.lock();
    if (!parent) return;
    auto& ch = parent->children;
    auto it = std::find(ch.begin(), ch.end(), old_node);
    if (it == ch.end()) return;
    new_node->parent = parent;
    new_node->prev_sibling = old_node->prev_sibling;
    new_node->next_sibling = old_node->next_sibling;
    if (auto prev = old_node->prev_sibling.lock()) prev->next_sibling = new_node;
    if (old_node->next_sibling) old_node->next_sibling->prev_sibling = new_node;
    *it = new_node;
    old_node->parent.reset();
    old_node->next_sibling.reset();
    old_node->prev_sibling.reset();
}

// ── Page loading pipeline ─────────────────────────────────────────────────────

void load_page(const std::string &raw_url) {
  // Atomically claim the loading slot — only one load at a time.
  bool expected = false;
  if (!g_load_in_progress.compare_exchange_strong(expected, true)) {
    std::cerr << "[load_page] blocked concurrent load for: " << raw_url << "\n";
    browser_ui.set_loading(false);
    PostMessage(g_hwnd, WM_USER + 3, 0, 0);
    return;
  }
  // RAII guard to clear flag even if we throw
  struct LoadGuard {
    ~LoadGuard() { g_load_in_progress.store(false); }
  } _guard;

  try {
    std::string html;
    std::string url = raw_url;

    if (url.empty()) {
      html = "<html><head><title>Scratch Browser</title>"
             "<style>"
             "body { background-color: #1e2026; color: #e0e4f0; }"
             ".hero { padding: 60px; }"
             ".title { color: #5082ff; font-size: 36px; }"
             ".subtitle { font-size: 18px; color: #8890a0; }"
             ".footer { font-size: 14px; color: #5a5e6e; }"
             "</style></head>"
             "<body><div class=\"hero\">"
             "<h1 class=\"title\">Scratch Browser</h1>"
             "<p class=\"subtitle\">Type a URL in the address bar above "
             "and press Enter or click Go to navigate.</p>"
             "<br/>"
             "<p class=\"footer\">Built from scratch with custom HTML parser, "
             "CSS engine, layout engine, and paint system.</p>"
             "</div></body></html>";
    } else {
      std::string file_path = url;
      if (file_path.substr(0, 8) == "file:///") {
        file_path = file_path.substr(8);
      } else if (file_path.substr(0, 7) == "file://") {
        file_path = file_path.substr(7);
      }
      html = read_file(file_path);
      if (html.empty()) {
        if (url.length() < 4 ||
            (url.substr(0, 8) != "https://" && url.substr(0, 7) != "http://")) {
          url = "https://" + url;
        }
        html = fetch_https(url);
      }
      if (html.empty()) {
        html = "<html><head><title>Error</title>"
               "<style>"
               "body { background-color: #1e2026; color: #e0e4f0; }"
               ".err { padding: 60px; }"
               "h1 { color: #e84848; }"
               "p { font-size: 16px; color: #8890a0; }"
               "</style></head>"
               "<body><div class=\"err\">"
               "<h1>Network Error</h1>"
               "<p>Could not load the page.</p>"
               "</div></body></html>";
      }
    }

    std::string css =
        "head,script,style,meta,link,title{display:none}\n"
        "body{margin:8px}\n"
        "a,span,b,strong,em,i,u,small,abbr,cite,code,sub,sup,label,"
        "button,input,select,textarea,img{"
        "display:inline}\n"
        "div,p,h1,h2,h3,h4,h5,h6,ul,ol,li,section,article,header,"
        "footer,nav,main,aside,form,table,tr,thead,tbody,tfoot,caption,"
        "blockquote,pre,figure,figcaption,address,hr,br{"
        "display:block}\n"
        "h1{font-size:32px;font-weight:bold}\n"
        "h2{font-size:24px;font-weight:bold}\n"
        "h3{font-size:20px;font-weight:bold}\n"
        "h4{font-size:16px;font-weight:bold}\n"
        "a{color:#0000EE}\n"
        "center{display:block;text-align:center}\n";

    std::cerr << "Parsing HTML (" << html.size() / 1024 << " KB)...\n";
    auto root = lexbor_parse_to_dom(html);

    {
      std::string page_css = lexbor_extract_css(root);
      static const size_t MAX_CSS_SIZE = 8 * 1024 * 1024; // 8MB inline CSS
      if (page_css.size() > MAX_CSS_SIZE) page_css.resize(MAX_CSS_SIZE);
      css += page_css;
    }

    if (!root) root = ElementNode("html");

    // Extract raw SVG strings before clearing HTML (Lexbor strips SVG attrs)
    std::vector<std::string> raw_svgs = extract_raw_svgs(html);
    std::cerr << "Found " << raw_svgs.size() << " raw SVGs in HTML\n";

    // Save raw HTML for View Source before clearing
    std::string saved_raw_html = html;

    html.clear();
    html.shrink_to_fit();

    // Fetch external stylesheets
    {
      static const int MAX_SHEETS = 8;
      int sheets_fetched = 0;
      std::function<void(std::shared_ptr<Node>)> collect_links;
      collect_links = [&](std::shared_ptr<Node> node) {
        if (!node || sheets_fetched >= MAX_SHEETS) return;
        if (node->type == NodeType::Element && node->data == "link") {
          auto& attrs = node->attributes;
          auto rel_it = attrs.find("rel");
          if (rel_it == attrs.end()) { for (auto& c : node->children) collect_links(c); return; }
          std::string rel = rel_it->second;
          std::transform(rel.begin(), rel.end(), rel.begin(), ::tolower);
          if (rel.find("stylesheet") == std::string::npos) return;
          auto href_it = attrs.find("href");
          if (href_it == attrs.end() || href_it->second.empty()) return;
          if (href_it->second.substr(0,5) == "data:") return;
          auto media_it = attrs.find("media");
          if (media_it != attrs.end()) {
            std::string media = media_it->second;
            std::transform(media.begin(), media.end(), media.begin(), ::tolower);
            if (media == "print") return;
          }
          std::string sheet_url = resolve_url(href_it->second, url);
          if (sheet_url.empty()) return;
          std::cerr << "Fetching CSS: " << sheet_url << "\n";
          std::string sheet = fetch_https(sheet_url);
          if (!sheet.empty()) {
            if (sheet.size() > 4*1024*1024) sheet.resize(4*1024*1024);
            css += "\n" + sheet + "\n";
            sheets_fetched++;
          }
        }
        for (auto& c : node->children) collect_links(c);
      };
      collect_links(root);
    }

    if (!root) {
      root = ElementNode("html");
    }

    std::function<int(std::shared_ptr<Node>)> count_nodes;
    count_nodes = [&](std::shared_ptr<Node> node) -> int {
      if (!node) return 0;
      int c = 1;
      for (auto &child : node->children) {
        c += count_nodes(child);
        if (c > 50000) break;
      }
      return c;
    };
    int total_nodes = count_nodes(root);
    std::cerr << "DOM nodes: " << total_nodes << "\n";

    std::cerr << "Total CSS: " << css.size() / 1024 << " KB\n";

    // Pre-pass: unwrap @media/@supports/@layer blocks (extract inner CSS rules).
    // Google wraps many display:none rules inside @media blocks we used to skip.
    // Run twice to handle one level of nesting.
    // We now evaluate simple max-width/min-width conditions against the viewport
    // to avoid unwrapping rules that shouldn't apply (e.g. mobile overrides).
    auto media_query_matches = [&](const std::string &condition) -> bool {
      // Check for "print" — never matches on screen
      if (condition.find("print") != std::string::npos &&
          condition.find("not") == std::string::npos &&
          condition.find("screen") == std::string::npos)
        return false;
      // forced-colors:active means high-contrast mode is enabled — skip.
      // forced-colors:none is the normal state — keep.
      if (condition.find("forced-colors:active") != std::string::npos ||
          condition.find("forced-colors: active") != std::string::npos)
        return false;
      // prefers-contrast:more means user wants more contrast — skip in normal mode
      if (condition.find("prefers-contrast:more") != std::string::npos ||
          condition.find("prefers-contrast: more") != std::string::npos)
        return false;
      // hover:hover matches on desktop
      if (condition.find("hover:hover") != std::string::npos ||
          condition.find("hover: hover") != std::string::npos)
        return true;
      int vw = g_viewport_width > 0 ? g_viewport_width : 1440;
      // Check max-width
      { size_t p = condition.find("max-width");
        if (p != std::string::npos) {
          p = condition.find(':', p);
          if (p != std::string::npos) {
            p++;
            while (p < condition.size() && (condition[p]==' '||condition[p]=='\t')) p++;
            int val = 0;
            while (p < condition.size() && condition[p] >= '0' && condition[p] <= '9') {
              val = val * 10 + (condition[p] - '0'); p++;
            }
            if (val > 0 && vw > val) return false; // viewport wider than max-width
          }
        }
      }
      // Check min-width
      { size_t p = condition.find("min-width");
        if (p != std::string::npos) {
          p = condition.find(':', p);
          if (p != std::string::npos) {
            p++;
            while (p < condition.size() && (condition[p]==' '||condition[p]=='\t')) p++;
            int val = 0;
            while (p < condition.size() && condition[p] >= '0' && condition[p] <= '9') {
              val = val * 10 + (condition[p] - '0'); p++;
            }
            if (val > 0 && vw < val) return false; // viewport narrower than min-width
          }
        }
      }
      return true; // default: include
    };
    for (int unwrap_pass = 0; unwrap_pass < 2; unwrap_pass++) {
      std::string flat;
      flat.reserve(css.size());
      size_t i = 0;
      while (i < css.size()) {
        if (css[i] != '@') {
          size_t next = css.find('@', i + 1);
          size_t end  = (next == std::string::npos) ? css.size() : next;
          flat.append(css, i, end - i);
          i = end;
          continue;
        }
        // '@' found — read rule name
        size_t at = i++;
        while (i < css.size() && (isalnum((unsigned char)css[i]) || css[i] == '-')) i++;
        std::string aname = css.substr(at + 1, i - at - 1);
        for (char &c : aname) c = (char)tolower((unsigned char)c);
        // Capture the condition text between rule name and '{'
        size_t cond_start = i;
        // Advance to '{' or ';'
        while (i < css.size() && css[i] != '{' && css[i] != ';') i++;
        if (i >= css.size()) break;
        if (css[i] == ';') { i++; continue; }
        std::string condition = css.substr(cond_start, i - cond_start);
        // css[i] == '{'
        int depth = 1; i++;
        size_t inner_start = i;
        while (i < css.size() && depth > 0) {
          if (css[i] == '{') depth++;
          else if (css[i] == '}') depth--;
          i++;
        }
        size_t inner_end = i - 1;
        // Keep content of @media, @supports, @layer; discard @keyframes, @font-face etc.
        if (aname == "media" || aname == "supports" || aname == "layer" || aname == "document") {
          // Only unwrap @media if the query matches the current viewport
          if (aname != "media" || media_query_matches(condition)) {
            flat.append(css, inner_start, inner_end - inner_start);
            flat += '\n';
          }
        }
      }
      css = std::move(flat);
    }
    std::cerr << "CSS after @media unwrap: " << css.size() / 1024 << " KB\n";

    // Extract :hover/:focus rules
    std::string hover_css, focus_css;
    {
      size_t i = 0;
      while (i < css.size()) {
        if (css[i]=='/' && i+1<css.size() && css[i+1]=='*') {
          i+=2; while(i+1<css.size()&&!(css[i]=='*'&&css[i+1]=='/'))i++; if(i+1<css.size())i+=2; continue;
        }
        if (css[i]=='@') {
          while(i<css.size()&&css[i]!='{' &&css[i]!=';')i++;
          if(i<css.size()&&css[i]==';'){i++;continue;}
          if(i<css.size()&&css[i]=='{'){int d=1;i++;while(i<css.size()&&d>0){if(css[i]=='{')d++;else if(css[i]=='}')d--;i++;}continue;}
          continue;
        }
        if (css[i]=='{') {
          size_t sel_end = i;
          size_t sel_start = css.rfind('}', sel_end > 0 ? sel_end-1 : 0);
          sel_start = (sel_start==std::string::npos) ? 0 : sel_start+1;
          std::string selector = css.substr(sel_start, sel_end-sel_start);
          int d=1; size_t j=i+1;
          while(j<css.size()&&d>0){if(css[j]=='{')d++;else if(css[j]=='}')d--;j++;}
          std::string body = css.substr(i, j-i);
          auto strip_ps=[](const std::string& s,const std::string& ps){
            std::string o=s; size_t p;
            while((p=o.find(ps))!=std::string::npos){size_t e=p+ps.size();o.erase(p,e-p);}
            return o;
          };
          if (selector.find(":hover")!=std::string::npos) {
            std::string cs=strip_ps(selector,":hover");
            if(!cs.empty()&&cs.find_first_not_of(" \t\n\r,")!=std::string::npos)
              hover_css+=cs+body+"\n";
          }
          if (selector.find(":focus")!=std::string::npos) {
            std::string cs=strip_ps(selector,":focus");
            if(!cs.empty()&&cs.find_first_not_of(" \t\n\r,")!=std::string::npos)
              focus_css+=cs+body+"\n";
          }
          i=j; continue;
        }
        i++;
      }
    }

    // Preprocess CSS
    {
      std::string cleaned;
      cleaned.reserve(css.size());
      size_t i = 0;
      int brace_depth = 0;

      while (i < css.size()) {
        char c = css[i];

        if (c == '/' && i + 1 < css.size() && css[i + 1] == '*') {
          i += 2;
          while (i + 1 < css.size() && !(css[i] == '*' && css[i + 1] == '/'))
            i++;
          if (i + 1 < css.size())
            i += 2;
          continue;
        }

        if (brace_depth > 0) {
          if (c == '"' || c == '\'') {
            char q = c;
            cleaned += c;
            i++;
            while (i < css.size() && css[i] != q) {
              if (css[i] == '\\') { cleaned += css[i++]; }
              if (i < css.size()) { cleaned += css[i++]; }
            }
            if (i < css.size()) { cleaned += css[i++]; }
            continue;
          }
          if (c == '{') { brace_depth++; cleaned += c; i++; continue; }
          if (c == '}') { brace_depth--; cleaned += c; i++; continue; }
          cleaned += c; i++; continue;
        }

        if (c == '{') { brace_depth++; cleaned += c; i++; continue; }
        if (c == '}') { i++; continue; }

        if (c == '@') {
          size_t at_start = i;
          i++;
          std::string at_name;
          while (i < css.size() && isalpha((unsigned char)css[i]))
            at_name += css[i++];

          if (at_name == "media") {
            while (i < css.size() && (css[i]==' '||css[i]=='\t'||css[i]=='\n'||css[i]=='\r')) i++;
            std::string condition;
            while (i < css.size() && css[i] != '{' && css[i] != ';')
              condition += css[i++];
            while (!condition.empty() && (condition.back()==' '||condition.back()=='\t')) condition.pop_back();

            bool matches = false;
            std::string cond_lower = condition;
            for (char &ch : cond_lower) ch = tolower((unsigned char)ch);

            if (cond_lower.find("print") != std::string::npos) {
              matches = false;
            } else if (cond_lower.find("speech") != std::string::npos) {
              matches = false;
            } else if (cond_lower.find("forced-colors:active") != std::string::npos ||
                       cond_lower.find("forced-colors: active") != std::string::npos) {
              matches = false;
            } else if (cond_lower.find("prefers-contrast:more") != std::string::npos ||
                       cond_lower.find("prefers-contrast: more") != std::string::npos) {
              matches = false;
            } else if (cond_lower.find("prefers-color-scheme") != std::string::npos) {
              matches = (cond_lower.find("light") != std::string::npos);
            } else if (cond_lower.find("prefers-reduced-motion") != std::string::npos) {
              matches = false;
            } else {
              matches = true;
              auto extract_px = [](const std::string& s, const std::string& key) -> int {
                size_t p = s.find(key);
                if (p == std::string::npos) return -1;
                size_t vs = p + key.size();
                while (vs < s.size() && (s[vs]==' '||s[vs]==':')) vs++;
                int val = 0;
                while (vs < s.size() && isdigit((unsigned char)s[vs])) val = val*10+(s[vs++]-'0');
                return val;
              };
              int min_w = extract_px(cond_lower, "min-width:");
              int max_w = extract_px(cond_lower, "max-width:");
              int min_h = extract_px(cond_lower, "min-height:");
              int max_h = extract_px(cond_lower, "max-height:");
              int vp_w = g_viewport_width > 0 ? g_viewport_width : 1440;
              int vp_h = g_viewport_height > 0 ? g_viewport_height : 600;
              if (min_w > 0 && vp_w < min_w) matches = false;
              if (max_w > 0 && vp_w > max_w) matches = false;
              if (min_h > 0 && vp_h < min_h) matches = false;
              if (max_h > 0 && vp_h > max_h) matches = false;
            }

            if (i < css.size() && css[i] == '{') {
              if (matches) {
                i++;
                int depth = 1;
                size_t inner_start = i;
                while (i < css.size() && depth > 0) {
                  if (css[i] == '{') depth++;
                  else if (css[i] == '}') depth--;
                  if (depth > 0) i++;
                  else break;
                }
                cleaned += css.substr(inner_start, i - inner_start);
                if (i < css.size()) i++;
              } else {
                int depth = 1; i++;
                while (i < css.size() && depth > 0) {
                  if (css[i] == '{') depth++;
                  else if (css[i] == '}') depth--;
                  i++;
                }
              }
            } else if (i < css.size() && css[i] == ';') {
              i++;
            }
          } else {
            while (i < css.size() && css[i] != '{' && css[i] != ';')
              i++;
            if (i < css.size() && css[i] == ';') {
              i++;
            } else if (i < css.size() && css[i] == '{') {
              int depth = 1; i++;
              while (i < css.size() && depth > 0) {
                if (css[i] == '{') depth++;
                else if (css[i] == '}') depth--;
                i++;
              }
            }
          }
          continue;
        }

        if (c == ':') {
          size_t j = i + 1;
          bool is_double = (j < css.size() && css[j] == ':');
          if (is_double) j++;
          size_t name_start = j;
          while (j < css.size() && (isalnum((unsigned char)css[j]) || css[j]=='-' || css[j]=='_')) j++;
          std::string pname = css.substr(name_start, j - name_start);
          // Strip state pseudo-classes (we handle hover/focus separately).
          // Preserve ::before/::after so pseudo-element rules parse correctly.
          // Strip all other ::pseudo-elements (::placeholder, ::selection, etc.)
          bool should_strip = (is_double && pname != "before" && pname != "after") ||
              pname == "hover" || pname == "focus" || pname == "active" ||
              pname == "visited" || pname == "focus-within" || pname == "focus-visible";
          if (should_strip) {
            i = j;
            if (i < css.size() && css[i] == '(') {
              int depth = 1; i++;
              while (i < css.size() && depth > 0) {
                if (css[i]=='(') depth++;
                else if (css[i]==')') depth--;
                i++;
              }
            }
            continue;
          }
          cleaned += c; i++; continue;
        }

        cleaned += c;
        i++;
      }
      css = std::move(cleaned);
    }

    static const size_t CSS_PARSE_LIMIT = 32 * 1024 * 1024; // 32MB
    if (css.size() > CSS_PARSE_LIMIT) {
      size_t cut = CSS_PARSE_LIMIT;
      while (cut > 0 && css[cut] != '}')
        cut--;
      if (cut > 0)
        css.resize(cut + 1);
    }
    std::cerr << "CSS after preprocessing: " << css.size() / 1024 << " KB\n";

    std::cerr << "Parsing CSS...\n";
    CSSParser css_parser(css);
    auto stylesheet = css_parser.parse_stylesheet();
    css.clear();
    css.shrink_to_fit();

    std::cerr << "CSS rules: " << stylesheet.rules.size() << "\n";

    if (!hover_css.empty()) {
      CSSParser hp(hover_css);
      g_hover_stylesheet = hp.parse_stylesheet();
      std::cerr << "Hover rules: " << g_hover_stylesheet.rules.size() << "\n";
    }
    if (!focus_css.empty()) {
      CSSParser fp(focus_css);
      g_focus_stylesheet = fp.parse_stylesheet();
    }

    // ── Step 6: JavaScript Execution (after DOM + CSSOM are ready) ────────────
    // Per the browser rendering pipeline: JS runs after HTML parsing and CSS
    // parsing are complete, so scripts have access to the full DOM and CSSOM.
    {
      bool run_js = (total_nodes <= 5000);
      std::cerr << "JS engine: " << (run_js ? "enabled" : "skipped (too many DOM nodes)") << "\n";
      std::cerr.flush();
      if (g_qjs_engine) { qjs_destroy(g_qjs_engine); g_qjs_engine = nullptr; }
      if (run_js) {
        g_qjs_engine = qjs_create(root);
        qjs_set_page_url(g_qjs_engine, url);
        qjs_set_viewport(g_qjs_engine, buffer_width > 0 ? buffer_width : 800,
                                       g_viewport_height > 0 ? g_viewport_height : 600);
        qjs_set_layout_cb(g_qjs_engine, [](Node *n, DOMRect &r) -> bool {
            if (!global_layout_root || !n) return false;
            std::function<bool(std::shared_ptr<LayoutBox>, Node *, DOMRect &)> walk;
            walk = [&](std::shared_ptr<LayoutBox> box, Node *tgt, DOMRect &out) -> bool {
                if (!box) return false;
                if (box->style_node && box->style_node->node.get() == tgt) {
                    out.x      = box->dimensions.content.x;
                    out.y      = box->dimensions.content.y;
                    out.width  = box->dimensions.content.width;
                    out.height = box->dimensions.content.height;
                    return true;
                }
                for (auto &c : box->children)
                    if (walk(c, tgt, out)) return true;
                return false;
            };
            return walk(global_layout_root, n, r);
        });

        qjs_run_scripts(g_qjs_engine, root);
        qjs_call_global(g_qjs_engine, "onload");
      }
    }

    // ── Rasterize SVG elements ────────────────────────────────────────────────
    if (!raw_svgs.empty()) {
      std::vector<std::shared_ptr<Node>> svg_nodes;
      collect_svg_nodes(root, svg_nodes);
      size_t n = std::min(raw_svgs.size(), svg_nodes.size());
      std::cerr << "SVG: " << n << " to rasterize (dom=" << svg_nodes.size()
                << " raw=" << raw_svgs.size() << ")\n";
      for (size_t i = 0; i < n; i++) {
        // Extract hint dimensions from the DOM node's width/height/viewBox attrs
        int hint_w = 0, hint_h = 0;
        {
          auto &attrs = svg_nodes[i]->attributes;
          auto pw = attrs.find("width");
          auto ph = attrs.find("height");
          if (pw != attrs.end()) try { hint_w = std::stoi(pw->second); } catch (...) {}
          if (ph != attrs.end()) try { hint_h = std::stoi(ph->second); } catch (...) {}
          // Fallback: parse viewBox="min-x min-y width height"
          if ((hint_w <= 0 || hint_h <= 0)) {
            auto vb = attrs.find("viewBox");
            if (vb == attrs.end()) vb = attrs.find("viewbox");
            if (vb != attrs.end()) {
              float vx = 0, vy = 0, vw = 0, vh = 0;
              if (sscanf(vb->second.c_str(), "%f %f %f %f", &vx, &vy, &vw, &vh) == 4) {
                if (hint_w <= 0 && vw > 0) hint_w = (int)vw;
                if (hint_h <= 0 && vh > 0) hint_h = (int)vh;
              }
            }
          }
          // Cap SVG sizes — prevent giant icons (e.g. viewBox 960x960)
          // while allowing logos (e.g. 272x92) to render at full size.
          if (hint_w > 512) hint_w = 512;
          if (hint_h > 512) hint_h = 512;
        }
        std::string key = rasterize_svg(raw_svgs[i], hint_w, hint_h);
        if (key.empty()) continue;
        int w = 0, h = 0;
        {
          EnterCriticalSection(&g_image_cache_cs);
          auto it = g_image_cache.find(key);
          if (it != g_image_cache.end()) { w = it->second.width; h = it->second.height; }
          LeaveCriticalSection(&g_image_cache_cs);
        }
        auto img_node = ElementNode("img");
        img_node->attributes["src"] = key;
        if (w > 0) img_node->attributes["width"]  = std::to_string(w);
        if (h > 0) img_node->attributes["height"] = std::to_string(h);
        // Copy id/class/style from the original SVG so CSS selectors still match
        for (const char* attr : {"id", "class", "style", "aria-label", "data-hveid"}) {
            if (svg_nodes[i]->attributes.count(attr))
                img_node->attributes[attr] = svg_nodes[i]->attributes.at(attr);
        }
        replace_dom_node(svg_nodes[i], img_node);
      }
    }

    std::cerr << "DOM ready, building style tree...\n";

    PageResult *pr = new PageResult();
    pr->page_url        = url;
    pr->raw_html        = std::move(saved_raw_html);
    pr->main_stylesheet  = stylesheet;
    pr->hover_stylesheet = g_hover_stylesheet;
    pr->focus_stylesheet = g_focus_stylesheet;
    pr->dom_root        = root;

    auto styleTreeRoot = build_style_tree(root, pr->main_stylesheet,
        pr->hover_stylesheet.rules.empty() ? nullptr : &pr->hover_stylesheet,
        pr->focus_stylesheet.rules.empty() ? nullptr : &pr->focus_stylesheet);

    pr->layout_root = build_layout_tree(styleTreeRoot);
    if (pr->layout_root) {
      Dimensions viewport;
      int cw = browser_ui.content_width();
      int ch = browser_ui.content_height();
      if (cw < 1) cw = 800;
      if (ch < 1) ch = 600;
      viewport.content.width  = (float)cw;
      viewport.content.height = 0.0f;
      pr->layout_root->layout(viewport);
      pr->display_list       = build_display_list(pr->layout_root);
      pr->scroll_containers  = compute_scroll_containers(pr->display_list, g_scroll_containers);
      collect_img_urls(pr->layout_root, url, pr->img_urls);
      std::cerr << "Display list: " << pr->display_list.size() << " commands\n";
    }

    if (root) {
      auto title_node = root->query_selector("title");
      if (title_node) {
        for (auto &child : title_node->children) {
          if (child->type == NodeType::Text) {
            pr->page_title = child->data;
            break;
          }
        }
      }
    }

    PostMessage(g_hwnd, WM_USER + 3, (WPARAM)pr, 0);

  } catch (const std::exception &e) {
    std::cerr << "load_page error: " << e.what() << "\n";
    browser_ui.set_loading(false);
    browser_ui.set_status("Error loading page");
    PostMessage(g_hwnd, WM_USER + 3, 0, 0);
  } catch (...) {
    std::cerr << "load_page error: unknown exception\n";
    browser_ui.set_loading(false);
    browser_ui.set_status("Error loading page");
    PostMessage(g_hwnd, WM_USER + 3, 0, 0);
  }
}

struct LoadArg { std::string url; };
static DWORD WINAPI load_page_thread(LPVOID p) {
  LoadArg *arg = (LoadArg *)p;
  load_page(arg->url);
  delete arg;
  return 0;
}

void navigate_to(const std::string &url) {
  browser_ui.set_loading(true);
  browser_ui.set_status("Loading...");
  LoadArg *arg = new LoadArg{url};
  HANDLE t = CreateThread(NULL, 0, load_page_thread, arg, 0, NULL);
  if (t) CloseHandle(t);
}

// ── DOM rebuild helpers for hover/focus/JS ────────────────────────────────────

// These globals are accessed by main.cpp's WndProc for hover/focus/JS rebuild.
// They live here because load_page sets them.
std::shared_ptr<Node>& get_g_dom_root()             { return g_dom_root; }
Stylesheet&            get_g_main_stylesheet()       { return g_main_stylesheet; }
Stylesheet&            get_g_hover_stylesheet()      { return g_hover_stylesheet; }
Stylesheet&            get_g_focus_stylesheet()      { return g_focus_stylesheet; }
QJSEngine*&            get_g_qjs_engine()            { return g_qjs_engine; }
