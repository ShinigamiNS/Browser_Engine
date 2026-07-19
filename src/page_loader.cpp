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
#include <fstream>
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

// When true, prefers-color-scheme:dark media queries match (dark theme).
bool g_dark_mode = false;

// ── URL helpers ───────────────────────────────────────────────────────────────

// Percent-decode a URL path component (%20 -> space, etc.). '+' is left alone
// because it is only a space in query strings, not in paths.
std::string percent_decode(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size() &&
        isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
      auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return c - 'A' + 10;
      };
      out += (char)((hex(s[i + 1]) << 4) | hex(s[i + 2]));
      i += 2;
    } else {
      out += s[i];
    }
  }
  return out;
}

// Percent-encode a string for use in a query string value.
std::string percent_encode_query(const std::string &s) {
  static const char *hex_digits = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    unsigned char c = (unsigned char)s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += (char)c;
    } else if (c == ' ') {
      out += '+';
    } else {
      out += '%';
      out += hex_digits[c >> 4];
      out += hex_digits[c & 0xF];
    }
  }
  return out;
}

// Convert a file:// URL to a local filesystem path.
// Handles file:///C:/..., file://C:/..., percent-encoding, and backslashes.
std::string file_url_to_path(const std::string &url) {
  std::string path = url;
  if (path.substr(0, 8) == "file:///")
    path = path.substr(8);
  else if (path.substr(0, 7) == "file://")
    path = path.substr(7);
  // Drop any #fragment / ?query — meaningless for local files
  size_t hash = path.find('#');
  if (hash != std::string::npos) path = path.substr(0, hash);
  size_t q = path.find('?');
  if (q != std::string::npos) path = path.substr(0, q);
  path = percent_decode(path);
  // A leading slash before a drive letter ("/C:/...") is a URL artifact
  if (path.size() >= 3 && path[0] == '/' && isalpha((unsigned char)path[1]) &&
      path[2] == ':')
    path = path.substr(1);
  return path;
}

// Clean up raw user/command-line input and turn it into a loadable URL.
// - trims whitespace, control chars, and surrounding quotes
// - Windows paths (C:\..., C:/...) become file:// URLs
// - things that can't be a hostname become a Google search
// - bare domains get https:// prepended
std::string normalize_url_input(const std::string &raw) {
  std::string s = raw;
  // Trim whitespace/control characters from both ends
  size_t b = 0, e = s.size();
  while (b < e && (unsigned char)s[b] <= ' ') b++;
  while (e > b && (unsigned char)s[e - 1] <= ' ') e--;
  s = s.substr(b, e - b);
  // Strip one pair of surrounding quotes, then trim again
  if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') &&
      s.back() == s.front()) {
    s = s.substr(1, s.size() - 2);
    b = 0; e = s.size();
    while (b < e && (unsigned char)s[b] <= ' ') b++;
    while (e > b && (unsigned char)s[e - 1] <= ' ') e--;
    s = s.substr(b, e - b);
  }
  if (s.empty()) return "";

  if (s.substr(0, 7) == "file://") return s;
  if (s.substr(0, 8) == "https://" || s.substr(0, 7) == "http://") {
    // Strip embedded whitespace (broken copy/paste artifacts)
    std::string cleaned;
    cleaned.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i)
      if ((unsigned char)s[i] > ' ') cleaned += s[i];
    // Google search results are a JavaScript app shell this engine cannot
    // execute (Google stopped serving basic HTML results in 2025, even to
    // text browsers). Rewrite to DuckDuckGo's HTML endpoint so search works;
    // the address bar shows the rewritten URL.
    size_t gpos = cleaned.find("google.com/search?");
    if (gpos != std::string::npos) {
      size_t qpos = cleaned.find('?', gpos);
      std::string query = cleaned.substr(qpos + 1);
      std::string q;
      size_t p = 0;
      while (p < query.size()) {
        size_t amp = query.find('&', p);
        std::string kv = (amp == std::string::npos) ? query.substr(p)
                                                    : query.substr(p, amp - p);
        if (kv.substr(0, 2) == "q=") { q = kv.substr(2); break; }
        p = (amp == std::string::npos) ? query.size() : amp + 1;
      }
      if (!q.empty())
        return "https://html.duckduckgo.com/html/?q=" + q;
    }
    return cleaned;
  }
  if (s.substr(0, 6) == "about:") return s;

  // Windows filesystem paths -> file URLs
  bool win_path = (s.size() >= 3 && isalpha((unsigned char)s[0]) &&
                   s[1] == ':' && (s[2] == '\\' || s[2] == '/'));
  if (win_path || s.substr(0, 2) == "\\\\") {
    std::string p = s;
    for (size_t i = 0; i < p.size(); ++i)
      if (p[i] == '\\') p[i] = '/';
    return "file:///" + p;
  }

  // No scheme: decide between URL and search query.
  // Searches: anything with spaces, or without a dot (not a hostname).
  size_t host_end = s.find_first_of("/?#");
  std::string host = (host_end == std::string::npos) ? s : s.substr(0, host_end);
  bool has_space = (s.find(' ') != std::string::npos);
  bool has_dot = (host.find('.') != std::string::npos);
  bool is_localhost = (host == "localhost" ||
                       host.substr(0, 10) == "localhost:");
  if (has_space || (!has_dot && !is_localhost))
    return "https://html.duckduckgo.com/html/?q=" + percent_encode_query(s);

  return (is_localhost ? "http://" : "https://") + s;
}

// Security gate for content-initiated navigation (link clicks, form actions,
// meta-refresh). `target` is an already-resolved absolute URL; `page_url` is
// the page the navigation originates from.
//
// The rule that matters: a page served over http(s) must never be able to
// navigate the browser to a local file:// URL — that is a local-file
// disclosure / SSRF vector. file:// pages (local test files opened by the
// user) may link among themselves. The address bar and command line are
// trusted entry points and bypass this check entirely.
bool is_content_navigation_allowed(const std::string &target,
                                   const std::string &page_url) {
  auto scheme_of = [](const std::string &u) -> std::string {
    size_t c = u.find(':');
    if (c == std::string::npos) return "";
    std::string s = u.substr(0, c);
    for (auto &ch : s) ch = (char)tolower((unsigned char)ch);
    return s;
  };
  std::string ts = scheme_of(target);
  std::string ps = scheme_of(page_url);

  // Never let content drive these schemes.
  if (ts == "javascript" || ts == "vbscript" || ts == "chrome" ||
      (ts == "about" && target != "about:blank"))
    return false;

  bool page_is_web = (ps == "http" || ps == "https");
  bool target_is_local = (ts == "file");
  if (page_is_web && target_is_local) return false; // block web -> file://

  // http(s), data:, mailto:, tel:, relative (empty scheme), and file->file
  // are allowed.
  return true;
}

std::string resolve_url(const std::string &src,
                        const std::string &page_url) {
  if (src.empty())
    return "";
  if (src.substr(0, 8) == "https://" || src.substr(0, 7) == "http://" ||
      src.substr(0, 7) == "file://" || src.substr(0, 5) == "data:")
    return src;
  // Scheme of the current page (https by default)
  size_t scheme_end = page_url.find("://");
  std::string scheme = (scheme_end == std::string::npos)
                           ? "https"
                           : page_url.substr(0, scheme_end);
  if (src.substr(0, 2) == "//")
    return (scheme == "file" ? "https:" : scheme + ":") + src;
  if (src[0] == '/') {
    if (scheme_end == std::string::npos)
      return "";
    if (scheme == "file") {
      // Root-relative on a local page: resolve against the drive root
      std::string path = page_url.substr(scheme_end + 3);
      while (!path.empty() && path[0] == '/') path = path.substr(1);
      size_t drive_end = path.find('/');
      std::string drive =
          (drive_end == std::string::npos) ? path : path.substr(0, drive_end);
      return "file:///" + drive + src;
    }
    size_t host_end = page_url.find('/', scheme_end + 3);
    std::string origin = (host_end == std::string::npos)
                             ? page_url
                             : page_url.substr(0, host_end);
    return origin + src;
  }
  // Path-relative: replace everything after the last slash
  std::string base = page_url;
  size_t hash = base.find('#');
  if (hash != std::string::npos) base = base.substr(0, hash);
  size_t q = base.find('?');
  if (q != std::string::npos) base = base.substr(0, q);
  size_t last_slash = base.rfind('/');
  if (last_slash == std::string::npos ||
      (scheme_end != std::string::npos && last_slash < scheme_end + 3))
    return base + "/" + src;
  return base.substr(0, last_slash + 1) + src;
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

// ── History & bookmarks storage ───────────────────────────────────────────────
// Simple tab-separated files next to the executable. One entry per line:
//   url \t title
// Kept small and human-readable; good enough for a scratch browser.

static const char *HISTORY_FILE   = "browser_history.txt";
static const char *BOOKMARKS_FILE = "browser_bookmarks.txt";

static std::string sanitize_line(const std::string &s) {
  std::string o;
  for (char c : s)
    if (c != '\n' && c != '\r' && c != '\t') o += c;
  return o;
}

// True for URLs that should never be recorded (internal pages, blanks).
static bool is_recordable_url(const std::string &url) {
  if (url.empty()) return false;
  if (url.substr(0, 6) == "about:") return false;
  if (url.substr(0, 12) == "view-source:") return false;
  return true;
}

void record_history(const std::string &url, const std::string &title) {
  // Never record while incognito.
  if (g_private_mode || !is_recordable_url(url)) return;
  std::ofstream f(HISTORY_FILE, std::ios::app);
  if (!f) return;
  f << sanitize_line(url) << "\t" << sanitize_line(title) << "\n";
}

bool add_bookmark(const std::string &url, const std::string &title) {
  if (!is_recordable_url(url)) return false;
  // De-dupe: skip if this URL is already bookmarked.
  {
    std::ifstream in(BOOKMARKS_FILE);
    std::string line;
    while (std::getline(in, line)) {
      size_t tab = line.find('\t');
      std::string u = (tab == std::string::npos) ? line : line.substr(0, tab);
      if (u == url) return false;
    }
  }
  std::ofstream f(BOOKMARKS_FILE, std::ios::app);
  if (!f) return false;
  f << sanitize_line(url) << "\t" << sanitize_line(title) << "\n";
  return true;
}

// Build an about: listing page (history or bookmarks) as HTML.
static std::string build_listing_page(const std::string &which) {
  bool is_hist = (which == "history");
  const char *file = is_hist ? HISTORY_FILE : BOOKMARKS_FILE;
  const char *heading = is_hist ? "History" : "Bookmarks";

  // Read entries; for history, show newest first and de-dupe.
  std::vector<std::pair<std::string, std::string>> entries;
  {
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty()) continue;
      size_t tab = line.find('\t');
      std::string u = (tab == std::string::npos) ? line : line.substr(0, tab);
      std::string t = (tab == std::string::npos) ? "" : line.substr(tab + 1);
      if (!u.empty()) entries.push_back({u, t});
    }
  }
  if (is_hist) {
    std::reverse(entries.begin(), entries.end());
    std::vector<std::pair<std::string, std::string>> dedup;
    for (auto &e : entries) {
      bool seen = false;
      for (auto &d : dedup) if (d.first == e.first) { seen = true; break; }
      if (!seen) dedup.push_back(e);
      if (dedup.size() >= 300) break;
    }
    entries.swap(dedup);
  }

  auto esc = [](const std::string &s) {
    std::string o;
    for (char c : s) {
      if (c == '<') o += "&lt;";
      else if (c == '>') o += "&gt;";
      else if (c == '&') o += "&amp;";
      else if (c == '"') o += "&quot;";
      else o += c;
    }
    return o;
  };

  std::string body;
  body += "<html><head><title>" + std::string(heading) + "</title><style>";
  body += "body{font-family:sans-serif;background:#1e2026;color:#e0e4f0;"
          "margin:0;padding:40px;}";
  body += "h1{color:#5082ff;font-size:28px;}";
  body += ".row{padding:8px 0;border-bottom:1px solid #2c2f38;}";
  body += "a{color:#7aa2ff;text-decoration:none;font-size:15px;}";
  body += ".t{color:#c0c6d8;font-size:14px;}";
  body += ".u{color:#6a7080;font-size:12px;}";
  body += ".empty{color:#6a7080;font-size:15px;}";
  body += "</style></head><body>";
  body += "<h1>" + std::string(heading) + "</h1>";
  if (entries.empty()) {
    body += "<p class=\"empty\">Nothing here yet.</p>";
  } else {
    for (auto &e : entries) {
      std::string title = e.second.empty() ? e.first : e.second;
      body += "<div class=\"row\"><a href=\"" + esc(e.first) + "\">";
      body += "<span class=\"t\">" + esc(title) + "</span></a><br>";
      body += "<span class=\"u\">" + esc(e.first) + "</span></div>";
    }
  }
  body += "</body></html>";
  return body;
}

// ── Page loading pipeline ─────────────────────────────────────────────────────

void load_page(const std::string &raw_url, int tab_id,
               const std::string &post_body) {
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

  bool page_load_error = false;
  bool page_cert_error = false;
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
    } else if (url == "about:history" || url == "about:bookmarks") {
      html = build_listing_page(url == "about:history" ? "history"
                                                       : "bookmarks");
    } else {
      bool is_file = (url.substr(0, 7) == "file://");
      std::string err_title, err_summary, err_detail, err_accent = "#e84848";
      if (is_file) {
        std::string file_path = file_url_to_path(url);
        html = read_file(file_path);
        if (html.empty()) {
          err_title = "File Not Found";
          err_summary = "The file could not be opened.";
          err_detail = file_path;
        }
      } else {
        HttpResponse resp =
            http_fetch(url, post_body.empty() ? "GET" : "POST", post_body,
                       g_private_mode);
        html = resp.body;
        // Follow redirects in the address bar/history.
        if (!resp.final_url.empty()) url = resp.final_url;

        if (resp.error == "cert") {
          page_cert_error = true;
          // Distinct TLS interstitial — the certificate failed strict
          // validation (expired, self-signed, wrong host, or untrusted CA).
          err_title = "Your connection is not private";
          err_summary =
              "This site's security certificate is not trusted, so the "
              "connection may be intercepted. The page was blocked.";
          err_detail = "NET::ERR_CERT_INVALID  \xE2\x80\x94  " + url;
          err_accent = "#e8a848";
          html.clear();
        } else if (!resp.ok || html.empty()) {
          err_title = "Network Error";
          if (resp.error == "dns")
            err_summary = "The server's address could not be found (DNS).";
          else if (resp.error == "connect")
            err_summary = "The connection to the server was refused or reset.";
          else if (resp.error == "timeout")
            err_summary = "The server took too long to respond.";
          else
            err_summary = "The page could not be loaded.";
          err_detail = url;
          html.clear();
        }
      }
      if (!err_title.empty()) page_load_error = true;
      if (html.empty() && !err_title.empty()) {
        auto esc_html = [](const std::string &s) {
          std::string o;
          for (char c : s) {
            if (c == '<') o += "&lt;";
            else if (c == '>') o += "&gt;";
            else if (c == '&') o += "&amp;";
            else o += c;
          }
          return o;
        };
        html = "<html><head><title>" + esc_html(err_title) + "</title>"
               "<style>"
               "html { background-color: #1e2026; }"
               "body { background-color: #1e2026; color: #e0e4f0;"
               "  font-family: sans-serif; }"
               ".err { padding: 60px; max-width: 700px; }"
               "h1 { color: " + err_accent + "; font-size: 28px; }"
               "p { font-size: 16px; color: #a0a6b8; line-height: 1.5; }"
               ".detail { font-size: 13px; color: #6a7080;"
               "  font-family: monospace; margin-top: 24px; }"
               "</style></head>"
               "<body><div class=\"err\">"
               "<h1>" + esc_html(err_title) + "</h1>"
               "<p>" + esc_html(err_summary) + "</p>"
               "<p class=\"detail\">" + esc_html(err_detail) + "</p>"
               "</div></body></html>";
      }
    }

    std::string css =
        "head,script,style,meta,link,title{display:none}\n"
        "body{margin:8px}\n"
        "a,span,b,strong,em,i,u,small,abbr,cite,code,sub,sup,label,"
        "button,input,select,textarea,img,svg{"
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
          // Security: only fetch http(s) stylesheets. A remote page must not
          // be able to pull a local file:// resource (or any other scheme)
          // as a subresource — that would disclose local files to the site.
          bool sheet_http = (sheet_url.substr(0, 8) == "https://" ||
                             sheet_url.substr(0, 7) == "http://");
          if (!sheet_http) {
            std::cerr << "Blocked non-http stylesheet: " << sheet_url << "\n";
            return;
          }
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
      // prefers-color-scheme — match dark blocks only when dark mode is on,
      // and light blocks only when it's off (like a real browser reporting
      // the OS/browser theme preference).
      if (condition.find("prefers-color-scheme") != std::string::npos) {
        bool wants_dark = condition.find("dark") != std::string::npos;
        return g_dark_mode ? wants_dark : !wants_dark;
      }
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
              bool wants_dark = cond_lower.find("dark") != std::string::npos;
              matches = g_dark_mode ? wants_dark : !wants_dark;
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
    // The engine is created locally and handed to the main thread inside
    // PageResult — the previous page's engine is destroyed on the main
    // thread when the result is installed (never here, where the main
    // thread could still be using it).
    QJSEngine *page_engine = nullptr;
    {
      bool run_js = (total_nodes <= 5000);
      std::cerr << "JS engine: " << (run_js ? "enabled" : "skipped (too many DOM nodes)") << "\n";
      std::cerr.flush();
      if (run_js) {
        page_engine = qjs_create(root);
        qjs_set_page_url(page_engine, url);
        qjs_set_viewport(page_engine, buffer_width > 0 ? buffer_width : 800,
                                      g_viewport_height > 0 ? g_viewport_height : 600);
        qjs_set_layout_cb(page_engine, [](Node *n, DOMRect &r) -> bool {
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

        qjs_run_scripts(page_engine, root);
        qjs_call_global(page_engine, "onload");
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
        // Turn the <svg> into a replaced image in place: keep the tag (and
        // all its attributes) so CSS selectors like `.x svg{}` still match,
        // point src at the rasterized cache entry, and drop the children so
        // no phantom boxes are laid out. Only set width/height attrs when
        // the original element had them — otherwise CSS controls the size.
        auto &svg = svg_nodes[i];
        svg->attributes["src"] = key;
        bool had_w = svg->attributes.count("width") > 0;
        bool had_h = svg->attributes.count("height") > 0;
        if (had_w || had_h) {
          if (w > 0) svg->attributes["width"]  = std::to_string(w);
          if (h > 0) svg->attributes["height"] = std::to_string(h);
        }
        for (auto &c : svg->children) {
          if (!c) continue;
          c->parent.reset();
          c->next_sibling.reset();
          c->prev_sibling.reset();
        }
        svg->children.clear();
      }
    }

    std::cerr << "DOM ready, building style tree...\n";

    PageResult *pr = new PageResult();
    pr->tab_id          = tab_id;
    pr->qjs_engine      = page_engine;
    pr->load_error      = page_load_error;
    pr->cert_error      = page_cert_error;
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

struct LoadArg { std::string url; int tab_id; std::string post_body; };
static DWORD WINAPI load_page_thread(LPVOID p) {
  LoadArg *arg = (LoadArg *)p;
  load_page(arg->url, arg->tab_id, arg->post_body);
  delete arg;
  return 0;
}

static void navigate_impl(const std::string &url,
                          const std::string &post_body) {
  // Normalize whatever we were handed (address bar text, command line,
  // history entries, hrefs). Call sites may have already pushed the raw
  // string into tab history/address bar — sync those to the cleaned URL.
  std::string clean = normalize_url_input(url);
  if (clean != url) {
    Tab *t = browser_ui.active_tab();
    if (t) {
      if (t->url == url) t->url = clean;
      if (t->history_index >= 0 && t->history_index < (int)t->history.size() &&
          t->history[t->history_index] == url)
        t->history[t->history_index] = clean;
    }
    if (browser_ui.get_address_text() == url)
      browser_ui.set_address_text(clean);
  }
  browser_ui.set_loading(true);
  browser_ui.set_status("Loading...");
  Tab *active = browser_ui.active_tab();
  LoadArg *arg = new LoadArg{clean, active ? active->id : -1, post_body};
  HANDLE t = CreateThread(NULL, 0, load_page_thread, arg, 0, NULL);
  if (t) CloseHandle(t);
}

void navigate_to(const std::string &url) { navigate_impl(url, ""); }

void navigate_to_post(const std::string &url, const std::string &post_body) {
  navigate_impl(url, post_body);
}

// ── DOM rebuild helpers for hover/focus/JS ────────────────────────────────────

// These globals are accessed by main.cpp's WndProc for hover/focus/JS rebuild.
// They live here because load_page sets them.
std::shared_ptr<Node>& get_g_dom_root()             { return g_dom_root; }
Stylesheet&            get_g_main_stylesheet()       { return g_main_stylesheet; }
Stylesheet&            get_g_hover_stylesheet()      { return g_hover_stylesheet; }
Stylesheet&            get_g_focus_stylesheet()      { return g_focus_stylesheet; }
QJSEngine*&            get_g_qjs_engine()            { return g_qjs_engine; }
