// main.cpp — Win32 entry point + WndProc
// All heavy subsystems live in image_cache.cpp, page_loader.cpp, renderer.cpp

#include "browser_ui.h"
#include "css_parser.h"
#include "image_cache.h"
#include "layout.h"
#include "lexbor_adapter.h"
#include "page_loader.h"
#include "paint.h"
#include "quickjs_adapter.h"
#include "renderer.h"
#include "style.h"
#include <algorithm>
#include <functional>
#include <iostream>
#include <numeric>
#include <string>
#include <windows.h>


// ── Window-level globals ───────────────────────────────────────────────────────
bool running         = true;
bool app_initialized = false; // guard against early WM_PAINT/WM_SIZE
HWND g_hwnd          = NULL;

BrowserUI browser_ui;

// Scroll state
int  scroll_y           = 0;
static const int SCROLL_STEP     = 40;
static const int SCROLLBAR_WIDTH = 12;
bool is_scrolling       = false;
int  scroll_drag_start_y = 0;
int  scroll_start_val   = 0;
float scroll_max        = 0;

// Focused layout box (for keyboard input into <input> fields)
std::shared_ptr<LayoutBox> focused_box = nullptr;

// ── Text Selection state ──────────────────────────────────────────────────────
bool is_selecting = false;        // mouse drag active
bool has_selection = false;       // selection exists
float sel_start_x = 0, sel_start_y = 0; // document coords
float sel_end_x = 0, sel_end_y = 0;     // document coords
// Selection is stored as normalized (top-left to bottom-right) for rendering
// These are exposed to renderer.cpp via extern
float g_sel_min_x = 0, g_sel_min_y = 0, g_sel_max_x = 0, g_sel_max_y = 0;
bool g_has_selection = false;

static void normalize_selection() {
  if (sel_start_y < sel_end_y || (sel_start_y == sel_end_y && sel_start_x <= sel_end_x)) {
    g_sel_min_x = sel_start_x; g_sel_min_y = sel_start_y;
    g_sel_max_x = sel_end_x;   g_sel_max_y = sel_end_y;
  } else {
    g_sel_min_x = sel_end_x;   g_sel_min_y = sel_end_y;
    g_sel_max_x = sel_start_x; g_sel_max_y = sel_start_y;
  }
  g_has_selection = has_selection;
}

static void copy_selection_to_clipboard(HWND hwnd) {
  if (!has_selection) return;
  normalize_selection();
  // Walk display list, collect text within selection range
  std::string result;
  float last_y = -99999;
  for (auto &cmd : master_display_list) {
    if (cmd.type != DisplayCommandType::Text) continue;
    float ty = cmd.rect.y;
    float tx = cmd.rect.x;
    float tw = cmd.rect.width;
    float th = cmd.font_size > 0 ? (float)cmd.font_size * 1.4f : 20.f;
    float tb = ty + th;
    // Check if text command is within selection y-range
    if (tb < g_sel_min_y || ty > g_sel_max_y) continue;
    // For single-line selection, check x-range too
    bool single_line = (g_sel_max_y - g_sel_min_y) < th;
    if (single_line) {
      if (tx + tw < g_sel_min_x || tx > g_sel_max_x) continue;
    }
    // Add newline when y changes significantly
    if (ty > last_y + th * 0.5f && !result.empty()) result += "\r\n";
    else if (ty <= last_y + th * 0.5f && !result.empty() && result.back() != ' ') result += " ";

    // Trim text to selection boundaries using proportional character position
    std::string txt = cmd.text;
    int len = (int)txt.size();
    if (len > 0 && tw > 0.1f) {
      bool on_first_line = (ty >= g_sel_min_y - 1 && ty <= g_sel_min_y + th);
      bool on_last_line  = (tb >= g_sel_max_y - 1 && tb <= g_sel_max_y + th + 1);
      int start_char = 0;
      int end_char = len;
      // Trim start: if this text is on the first selected line
      if (single_line || on_first_line) {
        if (g_sel_min_x > tx) {
          float frac = (g_sel_min_x - tx) / tw;
          start_char = (int)(frac * len);
          if (start_char < 0) start_char = 0;
          if (start_char > len) start_char = len;
        }
      }
      // Trim end: if this text is on the last selected line
      if (single_line || on_last_line) {
        if (g_sel_max_x < tx + tw) {
          float frac = (g_sel_max_x - tx) / tw;
          end_char = (int)(frac * len + 0.5f);
          if (end_char < 0) end_char = 0;
          if (end_char > len) end_char = len;
        }
      }
      if (start_char < end_char) {
        txt = txt.substr(start_char, end_char - start_char);
      } else {
        continue; // selection doesn't cover any characters
      }
    }
    result += txt;
    last_y = ty;
  }
  if (result.empty()) return;
  // Convert to wide string for clipboard
  int wlen = MultiByteToWideChar(CP_UTF8, 0, result.c_str(), -1, NULL, 0);
  if (wlen <= 0) return;
  HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, wlen * sizeof(wchar_t));
  if (!hMem) return;
  wchar_t *pMem = (wchar_t *)GlobalLock(hMem);
  MultiByteToWideChar(CP_UTF8, 0, result.c_str(), -1, pMem, wlen);
  GlobalUnlock(hMem);
  if (OpenClipboard(hwnd)) {
    EmptyClipboard();
    SetClipboardData(CF_UNICODETEXT, hMem);
    CloseClipboard();
  } else {
    GlobalFree(hMem);
  }
}

// ── Find-in-Page state ────────────────────────────────────────────────────────
bool g_find_bar_open = false;
std::string g_find_query;
int g_find_current_match = 0;
int g_find_total_matches = 0;
std::vector<FindMatch> g_find_matches;

static void update_find_matches() {
  g_find_matches.clear();
  if (g_find_query.empty()) { g_find_total_matches = 0; return; }
  // Case-insensitive search in display list text commands
  std::string query_lower = g_find_query;
  for (auto &c : query_lower) c = (char)tolower((unsigned char)c);
  for (auto &cmd : master_display_list) {
    if (cmd.type != DisplayCommandType::Text) continue;
    std::string text_lower = cmd.text;
    for (auto &c : text_lower) c = (char)tolower((unsigned char)c);
    size_t pos = 0;
    while ((pos = text_lower.find(query_lower, pos)) != std::string::npos) {
      // Approximate x position within the text command
      float char_w = cmd.rect.width / (float)(cmd.text.size() > 0 ? cmd.text.size() : 1);
      float match_x = cmd.rect.x + pos * char_w;
      float match_w = g_find_query.size() * char_w;
      float match_h = cmd.font_size > 0 ? (float)cmd.font_size * 1.4f : 20.f;
      g_find_matches.push_back({match_x, cmd.rect.y, match_w, match_h});
      pos += g_find_query.size();
    }
  }
  g_find_total_matches = (int)g_find_matches.size();
  if (g_find_current_match >= g_find_total_matches) g_find_current_match = 0;
}

static void scroll_to_find_match(int match_idx) {
  if (match_idx < 0 || match_idx >= (int)g_find_matches.size()) return;
  auto &m = g_find_matches[match_idx];
  int content_h = browser_ui.content_height();
  // Scroll so match is visible (centered vertically)
  int target = (int)(m.y - content_h / 2.f);
  if (target < 0) target = 0;
  float total_h = global_layout_root ? global_layout_root->dimensions.content.height : (float)content_h;
  if (target > (int)(total_h - content_h)) target = (int)std::max(0.f, total_h - content_h);
  scroll_y = target;
}

// ── Zoom state ────────────────────────────────────────────────────────────────
float g_zoom_level = 1.0f;
static const float g_zoom_levels[] = {0.25f, 0.33f, 0.5f, 0.67f, 0.75f, 0.8f, 0.9f,
                                       1.0f, 1.1f, 1.25f, 1.5f, 1.75f, 2.0f, 2.5f, 3.0f};
static const int g_num_zoom_levels = sizeof(g_zoom_levels) / sizeof(g_zoom_levels[0]);

static void apply_zoom() {
  if (!global_layout_root || buffer_width <= 0) return;
  Dimensions viewport;
  viewport.content.width = (float)buffer_width / g_zoom_level;
  viewport.content.height = 0.0f;
  global_layout_root->layout(viewport);
  master_display_list = build_display_list(global_layout_root);
  rebuild_scroll_containers();
}

// ── Context Menu IDs ──────────────────────────────────────────────────────────
#define IDM_COPY          40001
#define IDM_SELECT_ALL    40002
#define IDM_OPEN_LINK     40003
#define IDM_COPY_LINK     40004
#define IDM_RELOAD        40005
#define IDM_VIEW_SOURCE   40006
#define IDM_COPY_IMAGE    40007

// ── View Source ───────────────────────────────────────────────────────────────
std::string g_raw_html_source;

static void show_view_source(HWND hwnd) {
  if (g_raw_html_source.empty()) return;
  // HTML-escape the source, converting newlines to <br> and spaces/tabs to &nbsp;
  std::string escaped;
  escaped.reserve(g_raw_html_source.size() * 3);
  for (size_t i = 0; i < g_raw_html_source.size(); i++) {
    char c = g_raw_html_source[i];
    switch (c) {
      case '&':  escaped += "&amp;"; break;
      case '<':  escaped += "&lt;"; break;
      case '>':  escaped += "&gt;"; break;
      case '"':  escaped += "&quot;"; break;
      case '\n': escaped += "<br>"; break;
      case '\r': break; // skip CR
      case '\t': escaped += "&nbsp;&nbsp;&nbsp;&nbsp;"; break;
      case ' ':  escaped += "&nbsp;"; break;
      default:   escaped += c; break;
    }
  }
  std::string source_html =
    "<html><head><title>Source: " + g_current_page_url + "</title>"
    "<style>"
    "body { background: #1e1e2e; margin: 0; padding: 16px; }"
    "p { color: #cdd6f4; font-family: Consolas; font-size: 13px; margin: 0; padding: 0; }"
    "</style></head><body><p>" + escaped + "</p></body></html>";
  std::string tab_title = "Source: " + g_current_page_url;
  browser_ui.add_tab("view-source:" + g_current_page_url, tab_title);
  g_raw_html_source = source_html;
  auto root = lexbor_parse_to_dom(source_html);
  if (root) {
    std::string ua_css =
      "head,script,style,meta,link,title{display:none}\n"
      "body{margin:0}\npre{display:block}\n";
    CSSParser css_p(ua_css);
    Stylesheet ss = css_p.parse_stylesheet();
    auto st = build_style_tree(root, ss, nullptr, nullptr);
    auto lr = build_layout_tree(st);
    if (lr) {
      Dimensions vp;
      extern int buffer_width;
      extern float g_zoom_level;
      vp.content.width = (float)buffer_width / g_zoom_level;
      vp.content.height = 0;
      lr->layout(vp);
      get_g_dom_root() = root;
      get_g_main_stylesheet() = ss;
      global_layout_root = lr;
      master_display_list = build_display_list(lr);
      rebuild_scroll_containers();
      scroll_y = 0;
    }
  }
  InvalidateRect(hwnd, NULL, FALSE);
}

// ── Context menu state ───────────────────────────────────────────────────────
std::string s_ctx_link_url;
std::string s_ctx_img_src;

// ── WndProc ────────────────────────────────────────────────────────────────────

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam,
                            LPARAM lParam) {
  // Convenience accessors for page globals (live in page_loader.cpp)
  auto &g_dom_root        = get_g_dom_root();
  auto &g_main_stylesheet = get_g_main_stylesheet();
  auto &g_hover_stylesheet= get_g_hover_stylesheet();
  auto &g_focus_stylesheet= get_g_focus_stylesheet();
  auto &g_qjs_engine      = get_g_qjs_engine();

  switch (uMsg) {
  case WM_CLOSE:
    running = false;
    break;

  case WM_DESTROY:
    PostQuitMessage(0);
    running = false;
    break;

  case WM_MOUSEMOVE: {
    if (!app_initialized) break;
    int mx = (short)LOWORD(lParam);
    int my = (short)HIWORD(lParam);
    // Text selection drag
    if (is_selecting && my > browser_ui.content_y()) {
      sel_end_x = (float)mx / g_zoom_level;
      sel_end_y = (float)(my - browser_ui.content_y() + scroll_y) / g_zoom_level;
      has_selection = true;
      normalize_selection();
      InvalidateRect(hwnd, NULL, FALSE);
    }
    if (is_scrolling) {
      int dy = my - scroll_drag_start_y;
      int content_h = browser_ui.content_height();
      float total_h = global_layout_root
                          ? global_layout_root->dimensions.content.height
                          : (float)content_h;
      if (total_h > content_h) {
        float thumb_h = (std::max)(20.0f, (float)content_h * (float)content_h / total_h);
        float ratio   = (total_h - (float)content_h) / (float)(content_h - thumb_h);
        scroll_y = scroll_start_val + (int)(dy * ratio);
        if (scroll_y < 0) scroll_y = 0;
        if (scroll_y > total_h - content_h) scroll_y = (int)(total_h - content_h);
        InvalidateRect(hwnd, NULL, FALSE);
      }
    }
    browser_ui.on_mouse_move(mx, my);

    // Hover detection for :hover CSS
    if (global_layout_root && !is_scrolling) {
      int hx = mx;
      int hy = my - browser_ui.content_y() + scroll_y;
      std::function<std::shared_ptr<LayoutBox>(std::shared_ptr<LayoutBox>,int,int)> htest;
      htest=[&](std::shared_ptr<LayoutBox> b,int rx,int ry)->std::shared_ptr<LayoutBox>{
        if(!b)return nullptr;
        if(b->style_node && b->style_node->value("pointer-events") == "none") return nullptr;
        auto mb=b->dimensions.margin_box();
        if(rx<mb.x||rx>mb.x+mb.width||ry<mb.y||ry>mb.y+mb.height)return nullptr;
        for(auto&c:b->children){auto h=htest(c,rx,ry);if(h)return h;}
        return b;
      };
      auto hov=htest(global_layout_root,hx,hy);
      Node* nh=(hov&&hov->style_node)?hov->style_node->node.get():nullptr;
      if (hov && hov->style_node) {
        std::string css_cursor = hov->style_node->value("cursor");
        bool is_link = hov->style_node->node &&
                       hov->style_node->node->type == NodeType::Element &&
                       hov->style_node->node->data == "a";
        bool pointer_cursor = (css_cursor == "pointer") || is_link;
        SetCursor(LoadCursor(NULL, pointer_cursor ? IDC_HAND : IDC_ARROW));
      } else {
        SetCursor(LoadCursor(NULL, IDC_ARROW));
      }

      static Node* last_hov = nullptr;
      if(nh != last_hov) {
        if(last_hov) last_hov->attributes.erase("__hover");
        last_hov = nh;
        if(nh) nh->attributes["__hover"] = "1";
        if(g_dom_root && master_display_list.size() <= 300){
          auto hs=build_style_tree(g_dom_root,g_main_stylesheet,
            g_hover_stylesheet.rules.empty()?nullptr:&g_hover_stylesheet,
            g_focus_stylesheet.rules.empty()?nullptr:&g_focus_stylesheet);
          auto lr=build_layout_tree(hs);
          if(lr){
            Dimensions vp;
            vp.content.width  = (float)buffer_width;
            vp.content.height = 0.0f;
            lr->layout(vp);
            global_layout_root=lr;
            master_display_list=build_display_list(lr);
            rebuild_scroll_containers();
          }
        }
        InvalidateRect(hwnd,NULL,FALSE);
      }
    }

    TRACKMOUSEEVENT tme = {};
    tme.cbSize = sizeof(tme);
    tme.dwFlags = TME_LEAVE;
    tme.hwndTrack = hwnd;
    TrackMouseEvent(&tme);
  } break;

  case WM_MOUSELEAVE:
    if (!app_initialized) break;
    browser_ui.on_mouse_move(-1, -1);
    break;

  case WM_LBUTTONDOWN: {
    if (!app_initialized) break;
    int mx = (short)LOWORD(lParam);
    int my = (short)HIWORD(lParam);

    // Check if clicking on find bar close button
    if (g_find_bar_open) {
      int content_y = browser_ui.content_y();
      int win_w_val = buffer_width;
      int bar_w = 400;
      int bar_x = win_w_val - bar_w - 20;
      int bar_h = 30;
      // Close "X" button area
      if (mx >= bar_x + bar_w - 25 && mx <= bar_x + bar_w - 5 &&
          my >= content_y + 5 && my <= content_y + bar_h - 5) {
        g_find_bar_open = false;
        g_find_query.clear();
        g_find_matches.clear();
        g_find_total_matches = 0;
        InvalidateRect(hwnd, NULL, FALSE);
        break;
      }
    }

    UIHitResult hr = browser_ui.hit_test(mx, my);
    if (hr == UIHitResult::ContentArea) {
      // Start text selection (convert screen coords to document coords)
      sel_start_x = (float)mx / g_zoom_level;
      sel_start_y = (float)(my - browser_ui.content_y() + scroll_y) / g_zoom_level;
      sel_end_x = sel_start_x;
      sel_end_y = sel_start_y;
      is_selecting = true;
      has_selection = false;
      g_has_selection = false;
      SetCapture(hwnd);

      int content_x = mx;
      int content_y_val = my - browser_ui.content_y() + scroll_y;

      std::function<std::shared_ptr<LayoutBox>(std::shared_ptr<LayoutBox>, int, int)>
          hit_test_content;
      hit_test_content = [&](std::shared_ptr<LayoutBox> box, int rx, int ry)
          -> std::shared_ptr<LayoutBox> {
        if (!box) return nullptr;
        if (box->style_node && box->style_node->value("pointer-events") == "none")
          return nullptr;
        std::shared_ptr<LayoutBox> hit = nullptr;
        if (rx >= box->dimensions.content.x &&
            rx <= box->dimensions.content.x + box->dimensions.content.width &&
            ry >= box->dimensions.content.y &&
            ry <= box->dimensions.content.y + box->dimensions.content.height) {
          hit = box;
        }
        for (auto &c : box->children) {
          auto h = hit_test_content(c, rx, ry);
          if (h) hit = h;
        }
        return hit;
      };

      focused_box = hit_test_content(global_layout_root, content_x, content_y_val);

      // JavaScript onclick
      if (focused_box && focused_box->style_node && focused_box->style_node->node) {
        auto node = focused_box->style_node->node;
        std::shared_ptr<Node> onclick_node = nullptr;
        auto cur = node;
        while (cur) {
          if (cur->type == NodeType::Element && cur->attributes.count("onclick")) {
            onclick_node = cur;
            break;
          }
          cur = cur->parent.lock();
        }
        if (onclick_node) {
          std::string code = onclick_node->attributes["onclick"];
          if (g_qjs_engine) {
            std::string err = qjs_eval(g_qjs_engine, code, "<onclick>");
            if (!err.empty()) std::cerr << "[JS onclick error] " << err << "\n";
          }
        }
      }

      // addEventListener "click"
      if (g_qjs_engine && focused_box && focused_box->style_node &&
          focused_box->style_node->node) {
        auto cur = focused_box->style_node->node;
        while (cur) {
          qjs_fire_event(g_qjs_engine, cur.get(), "click", content_x,
                         content_y_val - scroll_y);
          cur = cur->parent.lock();
        }
      }

      // JS DOM dirty rebuild
      if (g_qjs_engine && qjs_dom_dirty(g_qjs_engine) && g_dom_root) {
        qjs_clear_dirty(g_qjs_engine);
        auto st = build_style_tree(g_dom_root, g_main_stylesheet,
            g_hover_stylesheet.rules.empty() ? nullptr : &g_hover_stylesheet,
            g_focus_stylesheet.rules.empty() ? nullptr : &g_focus_stylesheet);
        global_layout_root = build_layout_tree(st);
        if (global_layout_root) {
          Dimensions vp;
          vp.content.width  = (float)buffer_width;
          vp.content.height = 0.0f;
          global_layout_root->layout(vp);
          master_display_list = build_display_list(global_layout_root);
          rebuild_scroll_containers();
        }
        InvalidateRect(hwnd, NULL, FALSE);
      }

      // Submit button click
      {
        std::shared_ptr<Node> btn_node = nullptr;
        if (focused_box && focused_box->style_node && focused_box->style_node->node) {
          auto cur = focused_box->style_node->node;
          while (cur) {
            if (cur->type == NodeType::Element) {
              std::string tag = cur->data;
              std::string type;
              if (cur->attributes.count("type")) type = cur->attributes["type"];
              if (tag == "button" || type == "submit" || type == "button") {
                btn_node = cur;
                break;
              }
            }
            cur = cur->parent.lock();
          }
        }
        if (btn_node) {
          std::function<std::string(std::shared_ptr<LayoutBox>)> find_input;
          find_input = [&](std::shared_ptr<LayoutBox> box) -> std::string {
            if (!box) return "";
            if (box->style_node && box->style_node->node &&
                box->style_node->node->data == "input") {
              std::string itype = box->style_node->node->attributes["type"];
              if (itype != "hidden" && itype != "submit" && itype != "button")
                return box->style_node->node->attributes["value"];
            }
            for (auto &c : box->children) {
              std::string res = find_input(c);
              if (!res.empty()) return res;
            }
            return "";
          };
          bool has_onclick = btn_node->attributes.count("onclick") > 0;
          if (!has_onclick) {
            std::string val = find_input(global_layout_root);
            if (!val.empty()) {
              std::string nav_url = "https://google.com/search?q=" + val;
              for (char &c : nav_url) if (c == ' ') c = '+';
              Tab *tab = browser_ui.active_tab();
              if (tab) {
                tab->push_url(nav_url);
                browser_ui.set_address_text(nav_url);
              }
              navigate_to(nav_url);
              focused_box = nullptr;
            }
          }
        }
      }

      // Link (<a href>) click navigation
      if (focused_box && focused_box->style_node && focused_box->style_node->node) {
        std::shared_ptr<Node> anchor_node = nullptr;
        auto cur = focused_box->style_node->node;
        while (cur) {
          if (cur->type == NodeType::Element && cur->data == "a" &&
              cur->attributes.count("href")) {
            anchor_node = cur;
            break;
          }
          cur = cur->parent.lock();
        }
        if (anchor_node) {
          std::string href = anchor_node->attributes["href"];
          // Skip javascript: URLs and anchor-only links
          bool skip = false;
          if (href.size() >= 11 && href.substr(0, 11) == "javascript:")
            skip = true;
          if (!href.empty() && href[0] == '#')
            skip = true;
          if (!skip && !href.empty()) {
            // Resolve relative URLs against current page URL
            std::string resolved = href;
            if (href.substr(0, 8) != "https://" &&
                href.substr(0, 7) != "http://") {
              if (href.substr(0, 2) == "//") {
                resolved = "https:" + href;
              } else if (href[0] == '/') {
                size_t scheme_end = g_current_page_url.find("://");
                if (scheme_end != std::string::npos) {
                  size_t host_end =
                      g_current_page_url.find('/', scheme_end + 3);
                  std::string origin =
                      (host_end == std::string::npos)
                          ? g_current_page_url
                          : g_current_page_url.substr(0, host_end);
                  resolved = origin + href;
                }
              } else {
                size_t last_slash = g_current_page_url.rfind('/');
                if (last_slash != std::string::npos)
                  resolved = g_current_page_url.substr(0, last_slash + 1) + href;
              }
            }
            Tab *tab = browser_ui.active_tab();
            if (tab) {
              tab->push_url(resolved);
              browser_ui.set_address_text(resolved);
            }
            navigate_to(resolved);
            focused_box = nullptr;
          }
        }
      }
    } else {
      if (mx >= buffer_width - SCROLLBAR_WIDTH && mx <= buffer_width) {
        is_scrolling = true;
        scroll_drag_start_y = my;
        scroll_start_val = scroll_y;
      } else {
        browser_ui.on_mouse_down(mx, my);
      }
    }
    InvalidateRect(hwnd, NULL, FALSE);
  } break;

  case WM_LBUTTONUP: {
    if (!app_initialized) break;
    int mx = (short)LOWORD(lParam);
    int my = (short)HIWORD(lParam);
    is_scrolling = false;
    if (is_selecting) {
      is_selecting = false;
      ReleaseCapture();
      // If drag distance is too small, treat as click (no selection)
      float dx = sel_end_x - sel_start_x;
      float dy = sel_end_y - sel_start_y;
      if (dx * dx + dy * dy < 9.f) {
        has_selection = false;
        g_has_selection = false;
      } else {
        normalize_selection();
      }
      InvalidateRect(hwnd, NULL, FALSE);
    }
    browser_ui.on_mouse_up(mx, my);
  } break;

  case WM_MOUSEWHEEL: {
    if (!app_initialized) break;
    int delta = GET_WHEEL_DELTA_WPARAM(wParam);
    // Ctrl+mousewheel = zoom
    if (LOWORD(wParam) & MK_CONTROL) {
      if (delta > 0) {
        for (int i = 0; i < g_num_zoom_levels - 1; i++) {
          if (g_zoom_levels[i] >= g_zoom_level - 0.01f) {
            g_zoom_level = g_zoom_levels[i + 1]; break;
          }
        }
      } else {
        for (int i = g_num_zoom_levels - 1; i > 0; i--) {
          if (g_zoom_levels[i] <= g_zoom_level + 0.01f) {
            g_zoom_level = g_zoom_levels[i - 1]; break;
          }
        }
      }
      apply_zoom();
      InvalidateRect(hwnd, NULL, FALSE);
      break;
    }
    POINT mpt;
    mpt.x = (short)LOWORD(lParam);
    mpt.y = (short)HIWORD(lParam);
    ScreenToClient(hwnd, &mpt);
    int wmx = mpt.x;
    int wmy = mpt.y;
    bool handled = false;
    if (wmy > browser_ui.content_y()) {
      float doc_x = (float)wmx;
      float doc_y = (float)(wmy - browser_ui.content_y() + scroll_y);
      for (auto &sc : g_scroll_containers) {
        if (doc_x >= sc.bounds.x && doc_x <= sc.bounds.x + sc.bounds.width &&
            doc_y >= sc.bounds.y && doc_y <= sc.bounds.y + sc.bounds.height) {
          sc.scroll_y -= (float)(delta / 120) * SCROLL_STEP;
          float max_scroll = sc.content_height - sc.bounds.height;
          if (sc.scroll_y < 0.f) sc.scroll_y = 0.f;
          if (max_scroll > 0.f && sc.scroll_y > max_scroll) sc.scroll_y = max_scroll;
          handled = true;
          break;
        }
      }
    }
    if (!handled) {
      scroll_y -= (delta / 120) * SCROLL_STEP;
      int content_h = browser_ui.content_height();
      float total_h = global_layout_root
                          ? global_layout_root->dimensions.content.height
                          : (float)content_h;
      if (scroll_y < 0) scroll_y = 0;
      if (scroll_y > total_h - content_h)
        scroll_y = (int)std::max(0.0f, total_h - content_h);
    }
    InvalidateRect(hwnd, NULL, FALSE);
  } break;

  case WM_KEYDOWN: {
    if (!app_initialized) break;
    browser_ui.on_key_down((int)wParam);

    if (GetKeyState(VK_CONTROL) & 0x8000) {
      if (wParam == 'T') {
        browser_ui.add_tab("", "New Tab");
        browser_ui.focus_address_bar();
        navigate_to("");
        InvalidateRect(hwnd, NULL, FALSE);
      } else if (wParam == 'W') {
        browser_ui.close_tab(browser_ui.active_tab_index());
        Tab *t = browser_ui.active_tab();
        if (t) navigate_to(t->url);
        InvalidateRect(hwnd, NULL, FALSE);
      } else if (wParam == 'L') {
        browser_ui.focus_address_bar();
        InvalidateRect(hwnd, NULL, FALSE);
      } else if (wParam == 'R') {
        Tab *t = browser_ui.active_tab();
        if (t && !t->url.empty()) navigate_to(t->url);
      } else if (wParam == 'C') {
        // Copy selection to clipboard
        copy_selection_to_clipboard(hwnd);
      } else if (wParam == 'A') {
        // Select all — select entire page content
        if (global_layout_root) {
          sel_start_x = 0; sel_start_y = 0;
          sel_end_x = (float)buffer_width / g_zoom_level;
          sel_end_y = global_layout_root->dimensions.content.height;
          has_selection = true;
          normalize_selection();
          InvalidateRect(hwnd, NULL, FALSE);
        }
      } else if (wParam == 'V') {
        // Paste into find bar if open
        if (g_find_bar_open) {
          if (OpenClipboard(hwnd)) {
            HANDLE hData = GetClipboardData(CF_UNICODETEXT);
            if (hData) {
              wchar_t *pData = (wchar_t *)GlobalLock(hData);
              if (pData) {
                int needed = WideCharToMultiByte(CP_UTF8, 0, pData, -1, NULL, 0, NULL, NULL);
                if (needed > 0) {
                  std::string utf8(needed, 0);
                  WideCharToMultiByte(CP_UTF8, 0, pData, -1, &utf8[0], needed, NULL, NULL);
                  // Remove null terminator and any newlines
                  while (!utf8.empty() && (utf8.back() == '\0' || utf8.back() == '\r' || utf8.back() == '\n'))
                    utf8.pop_back();
                  g_find_query += utf8;
                  update_find_matches();
                  if (g_find_total_matches > 0)
                    scroll_to_find_match(g_find_current_match);
                  InvalidateRect(hwnd, NULL, FALSE);
                }
                GlobalUnlock(hData);
              }
            }
            CloseClipboard();
          }
        }
      } else if (wParam == 'F') {
        // Toggle find bar
        g_find_bar_open = !g_find_bar_open;
        if (!g_find_bar_open) {
          g_find_query.clear();
          g_find_matches.clear();
          g_find_total_matches = 0;
        }
        InvalidateRect(hwnd, NULL, FALSE);
      } else if (wParam == VK_OEM_PLUS || wParam == VK_ADD) {
        // Zoom in
        for (int i = 0; i < g_num_zoom_levels - 1; i++) {
          if (g_zoom_levels[i] >= g_zoom_level - 0.01f) {
            g_zoom_level = g_zoom_levels[i + 1];
            break;
          }
        }
        apply_zoom();
        InvalidateRect(hwnd, NULL, FALSE);
      } else if (wParam == VK_OEM_MINUS || wParam == VK_SUBTRACT) {
        // Zoom out
        for (int i = g_num_zoom_levels - 1; i > 0; i--) {
          if (g_zoom_levels[i] <= g_zoom_level + 0.01f) {
            g_zoom_level = g_zoom_levels[i - 1];
            break;
          }
        }
        apply_zoom();
        InvalidateRect(hwnd, NULL, FALSE);
      } else if (wParam == '0') {
        // Reset zoom
        g_zoom_level = 1.0f;
        apply_zoom();
        InvalidateRect(hwnd, NULL, FALSE);
      } else if (wParam == 'U') {
        show_view_source(hwnd);
      }
    }

    if (wParam == VK_F5) {
      Tab *t = browser_ui.active_tab();
      if (t && !t->url.empty()) navigate_to(t->url);
    }
    if (wParam == VK_ESCAPE) {
      if (g_find_bar_open) {
        g_find_bar_open = false;
        g_find_query.clear();
        g_find_matches.clear();
        g_find_total_matches = 0;
        InvalidateRect(hwnd, NULL, FALSE);
      } else if (has_selection) {
        has_selection = false;
        g_has_selection = false;
        InvalidateRect(hwnd, NULL, FALSE);
      }
    }

    if (GetKeyState(VK_MENU) & 0x8000) {
      if (wParam == VK_LEFT) {
        Tab *t = browser_ui.active_tab();
        if (t && t->can_go_back()) {
          std::string url = t->go_back();
          browser_ui.set_address_text(url);
          navigate_to(url);
        }
      } else if (wParam == VK_RIGHT) {
        Tab *t = browser_ui.active_tab();
        if (t && t->can_go_forward()) {
          std::string url = t->go_forward();
          browser_ui.set_address_text(url);
          navigate_to(url);
        }
      }
    }
  } break;

  case WM_CHAR: {
    if (!app_initialized) break;
    char typed = (char)wParam;

    if (browser_ui.is_address_focused()) {
      browser_ui.on_char(typed);
      break;
    }
    // Find bar input
    if (g_find_bar_open) {
      if (typed == '\b') {
        if (!g_find_query.empty()) g_find_query.pop_back();
      } else if (typed == '\r') {
        // Enter = next match, Shift+Enter = previous
        if (GetKeyState(VK_SHIFT) & 0x8000) {
          g_find_current_match--;
          if (g_find_current_match < 0) g_find_current_match = g_find_total_matches - 1;
        } else {
          g_find_current_match++;
          if (g_find_current_match >= g_find_total_matches) g_find_current_match = 0;
        }
        scroll_to_find_match(g_find_current_match);
      } else if (typed >= 32 && typed <= 126) {
        g_find_query += typed;
      } else {
        break;
      }
      update_find_matches();
      if (g_find_total_matches > 0 && typed != '\r')
        scroll_to_find_match(g_find_current_match);
      InvalidateRect(hwnd, NULL, FALSE);
      break;
    }

    if (focused_box && focused_box->style_node &&
        focused_box->style_node->node &&
        focused_box->style_node->node->data == "input") {
      auto &val = focused_box->style_node->node->attributes["value"];
      if (typed == '\b') {
        if (!val.empty()) val.pop_back();
      } else if (typed >= 32 && typed <= 126) {
        val += typed;
      } else if (typed == '\r') {
        std::string nav_url = "https://google.com/search?q=" + val;
        for (char &c : nav_url) if (c == ' ') c = '+';
        Tab *tab = browser_ui.active_tab();
        if (tab) {
          tab->push_url(nav_url);
          browser_ui.set_address_text(nav_url);
        }
        navigate_to(nav_url);
      }
      if (global_layout_root) {
        master_display_list = build_display_list(global_layout_root);
        rebuild_scroll_containers();
        InvalidateRect(hwnd, NULL, FALSE);
      }
    }
  } break;

  case WM_SIZE: {
    if (!app_initialized) break;

    RECT rect;
    GetClientRect(hwnd, &rect);
    int win_w = rect.right - rect.left;
    int win_h = rect.bottom - rect.top;

    if (win_w < 1 || win_h < 1) break;

    browser_ui.resize(win_w, win_h);

    buffer_width = browser_ui.content_width();
    buffer_height = browser_ui.content_height();
    if (buffer_width < 1)  buffer_width  = 1;
    if (buffer_height < 1) buffer_height = 1;
    g_viewport_height = buffer_height;
    g_viewport_width  = buffer_width;

    if (g_qjs_engine)
      qjs_set_viewport(g_qjs_engine, buffer_width, buffer_height);

    if (buffer_memory) {
      VirtualFree(buffer_memory, 0, MEM_RELEASE);
      buffer_memory = nullptr;
    }

    bitmap_info.bmiHeader.biSize        = sizeof(bitmap_info.bmiHeader);
    bitmap_info.bmiHeader.biWidth       = buffer_width;
    bitmap_info.bmiHeader.biHeight      = -buffer_height;
    bitmap_info.bmiHeader.biPlanes      = 1;
    bitmap_info.bmiHeader.biBitCount    = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;

    int buffer_size = buffer_width * buffer_height * 4;
    if (buffer_size > 0)
      buffer_memory = VirtualAlloc(0, buffer_size, MEM_COMMIT, PAGE_READWRITE);

    if (global_layout_root) {
      Dimensions viewport;
      viewport.content.width  = (float)buffer_width / g_zoom_level;
      viewport.content.height = 0.0f;
      global_layout_root->layout(viewport);
      master_display_list = build_display_list(global_layout_root);
      rebuild_scroll_containers();
    }
    InvalidateRect(hwnd, NULL, FALSE);
  } break;

  case WM_PAINT: {
    PAINTSTRUCT ps;
    HDC window_dc = BeginPaint(hwnd, &ps);

    RECT client;
    GetClientRect(hwnd, &client);
    int win_w = client.right - client.left;
    int win_h = client.bottom - client.top;

    if (win_w < 1 || win_h < 1) { EndPaint(hwnd, &ps); break; }

    HDC mem_dc = CreateCompatibleDC(window_dc);
    if (!mem_dc) { EndPaint(hwnd, &ps); break; }

    HBITMAP mem_bmp = CreateCompatibleBitmap(window_dc, win_w, win_h);
    if (!mem_bmp) { DeleteDC(mem_dc); EndPaint(hwnd, &ps); break; }

    HGDIOBJ old_bmp = SelectObject(mem_dc, mem_bmp);

    render_frame(hwnd, mem_dc, win_w, win_h, scroll_y, is_scrolling, g_current_page_url);

    BitBlt(window_dc, 0, 0, win_w, win_h, mem_dc, 0, 0, SRCCOPY);

    SelectObject(mem_dc, old_bmp);
    DeleteObject(mem_bmp);
    DeleteDC(mem_dc);
    EndPaint(hwnd, &ps);
  } break;

  case WM_USER + 1:
    // Image fetch thread finished a batch
    InvalidateRect(hwnd, NULL, FALSE);
    break;

  case WM_TIMER:
    // Run deferred setTimeout callbacks and refresh display
    if (get_g_qjs_engine() && qjs_run_pending_timers(get_g_qjs_engine())) {
      // JS fired and may have dirtied the DOM — rebuild layout
      if (qjs_dom_dirty(get_g_qjs_engine())) {
        qjs_clear_dirty(get_g_qjs_engine());
        auto &g_dom_root2        = get_g_dom_root();
        auto &g_main_stylesheet2 = get_g_main_stylesheet();
        auto &g_hover_stylesheet2= get_g_hover_stylesheet();
        auto &g_focus_stylesheet2= get_g_focus_stylesheet();
        if (g_dom_root2) {
          auto st = build_style_tree(g_dom_root2, g_main_stylesheet2,
              g_hover_stylesheet2.rules.empty() ? nullptr : &g_hover_stylesheet2,
              g_focus_stylesheet2.rules.empty() ? nullptr : &g_focus_stylesheet2);
          auto lr = build_layout_tree(st);
          if (lr) {
            Dimensions vp;
            vp.content.width  = (float)buffer_width;
            vp.content.height = 0.0f;
            lr->layout(vp);
            global_layout_root  = lr;
            master_display_list = build_display_list(lr);
            rebuild_scroll_containers();
          }
        }
      }
    }
    InvalidateRect(hwnd, NULL, FALSE);
    break;

  case WM_USER + 3: {
    // Page load complete — swap in new globals
    PageResult *pr = (PageResult *)wParam;
    if (!pr) {
      master_display_list.clear();
      global_layout_root = nullptr;
      g_scroll_containers.clear();
      browser_ui.set_loading(false);
      browser_ui.set_status("Error loading page");
      InvalidateRect(hwnd, NULL, FALSE);
      break;
    }

    g_dom_root          = pr->dom_root;
    g_main_stylesheet   = std::move(pr->main_stylesheet);
    g_hover_stylesheet  = std::move(pr->hover_stylesheet);
    g_focus_stylesheet  = std::move(pr->focus_stylesheet);
    global_layout_root  = pr->layout_root;
    master_display_list = std::move(pr->display_list);
    g_scroll_containers = std::move(pr->scroll_containers);
    g_current_page_url  = pr->page_url;
    g_raw_html_source   = std::move(pr->raw_html);
    scroll_y            = 0;
    focused_box         = nullptr;
    has_selection        = false;
    g_has_selection      = false;

    Tab *tab = browser_ui.active_tab();
    if (tab && !pr->page_title.empty())
      tab->title = pr->page_title;
    else if (tab && !pr->page_url.empty())
      tab->title = pr->page_url;

    if (!pr->img_urls.empty()) {
      ImgFetchParams *p = new ImgFetchParams();
      p->urls = pr->img_urls;
      p->hwnd = g_hwnd;
      HANDLE ht = CreateThread(NULL, 0, fetch_images_thread, p, 0, NULL);
      if (ht) CloseHandle(ht);
    }

    std::string status = pr->page_url.empty() ? "Ready" : pr->page_url;
    delete pr;

    // Relayout with current viewport dimensions (page was laid out in background
    // thread which may have used a different viewport width)
    if (global_layout_root && buffer_width > 0) {
      Dimensions viewport;
      viewport.content.width  = (float)buffer_width / g_zoom_level;
      viewport.content.height = 0.0f;
      global_layout_root->layout(viewport);
      master_display_list = build_display_list(global_layout_root);
      rebuild_scroll_containers();
    }

    browser_ui.set_loading(false);
    browser_ui.set_status(status);
    InvalidateRect(hwnd, NULL, FALSE);
    break;
  }

  case WM_RBUTTONDOWN: {
    if (!app_initialized) break;
    int mx = (short)LOWORD(lParam);
    int my = (short)HIWORD(lParam);
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) break;

    UIHitResult hr = browser_ui.hit_test(mx, my);
    if (hr == UIHitResult::ContentArea) {
      int doc_x = mx;
      int doc_y = my - browser_ui.content_y() + scroll_y;
      // Hit-test for link or image
      std::shared_ptr<LayoutBox> hit = nullptr;
      std::function<std::shared_ptr<LayoutBox>(std::shared_ptr<LayoutBox>, int, int)> ht;
      ht = [&](std::shared_ptr<LayoutBox> b, int rx, int ry) -> std::shared_ptr<LayoutBox> {
        if (!b) return nullptr;
        auto mb = b->dimensions.margin_box();
        if (rx < mb.x || rx > mb.x + mb.width || ry < mb.y || ry > mb.y + mb.height) return nullptr;
        for (auto &c : b->children) { auto h = ht(c, rx, ry); if (h) return h; }
        return b;
      };
      hit = ht(global_layout_root, doc_x, doc_y);

      bool is_link = false, is_image = false;
      std::string link_url, img_src;
      if (hit && hit->style_node && hit->style_node->node) {
        auto cur = hit->style_node->node;
        while (cur) {
          if (cur->data == "a" && cur->attributes.count("href")) {
            is_link = true; link_url = cur->attributes["href"]; break;
          }
          if (cur->data == "img" && cur->attributes.count("src")) {
            is_image = true; img_src = cur->attributes["src"];
          }
          cur = cur->parent.lock();
        }
      }

      if (has_selection) AppendMenuA(hMenu, MF_STRING, IDM_COPY, "Copy\tCtrl+C");
      if (is_link) {
        AppendMenuA(hMenu, MF_STRING, IDM_OPEN_LINK, "Open Link");
        AppendMenuA(hMenu, MF_STRING, IDM_COPY_LINK, "Copy Link Address");
      }
      if (is_image)
        AppendMenuA(hMenu, MF_STRING, IDM_COPY_IMAGE, "Copy Image Address");
      AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);
      AppendMenuA(hMenu, MF_STRING, IDM_SELECT_ALL, "Select All\tCtrl+A");
      AppendMenuA(hMenu, MF_STRING, IDM_RELOAD, "Reload\tF5");
      AppendMenuA(hMenu, MF_STRING, IDM_VIEW_SOURCE, "View Page Source\tCtrl+U");

      // Store context info for WM_COMMAND
      s_ctx_link_url = link_url; s_ctx_img_src = img_src;
    } else {
      AppendMenuA(hMenu, MF_STRING, IDM_RELOAD, "Reload\tF5");
    }

    POINT pt = {mx, my};
    ClientToScreen(hwnd, &pt);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(hMenu);
  } break;

  case WM_COMMAND: {
    int cmd_id = LOWORD(wParam);
    switch (cmd_id) {
    case IDM_COPY:
      copy_selection_to_clipboard(hwnd);
      break;
    case IDM_SELECT_ALL:
      if (global_layout_root) {
        sel_start_x = 0; sel_start_y = 0;
        sel_end_x = (float)buffer_width / g_zoom_level;
        sel_end_y = global_layout_root->dimensions.content.height;
        has_selection = true;
        normalize_selection();
        InvalidateRect(hwnd, NULL, FALSE);
      }
      break;
    case IDM_OPEN_LINK: {
      if (!s_ctx_link_url.empty()) {
        Tab *t = browser_ui.active_tab();
        if (t) { t->push_url(s_ctx_link_url); browser_ui.set_address_text(s_ctx_link_url); }
        navigate_to(s_ctx_link_url);
      }
      break;
    }
    case IDM_COPY_LINK: {
      if (!s_ctx_link_url.empty()) {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, s_ctx_link_url.c_str(), -1, NULL, 0);
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, wlen * sizeof(wchar_t));
        if (hMem) {
          wchar_t *p = (wchar_t *)GlobalLock(hMem);
          MultiByteToWideChar(CP_UTF8, 0, s_ctx_link_url.c_str(), -1, p, wlen);
          GlobalUnlock(hMem);
          if (OpenClipboard(hwnd)) { EmptyClipboard(); SetClipboardData(CF_UNICODETEXT, hMem); CloseClipboard(); }
          else GlobalFree(hMem);
        }
      }
      break;
    }
    case IDM_COPY_IMAGE: {
      if (!s_ctx_img_src.empty()) {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, s_ctx_img_src.c_str(), -1, NULL, 0);
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, wlen * sizeof(wchar_t));
        if (hMem) {
          wchar_t *p = (wchar_t *)GlobalLock(hMem);
          MultiByteToWideChar(CP_UTF8, 0, s_ctx_img_src.c_str(), -1, p, wlen);
          GlobalUnlock(hMem);
          if (OpenClipboard(hwnd)) { EmptyClipboard(); SetClipboardData(CF_UNICODETEXT, hMem); CloseClipboard(); }
          else GlobalFree(hMem);
        }
      }
      break;
    }
    case IDM_RELOAD: {
      Tab *t = browser_ui.active_tab();
      if (t && !t->url.empty()) navigate_to(t->url);
      break;
    }
    case IDM_VIEW_SOURCE: {
      show_view_source(hwnd);
      break;
    }
    }
  } break;

  case WM_SETCURSOR:
    if (LOWORD(lParam) == HTCLIENT) return TRUE;
    break;
  }
  return DefWindowProcA(hwnd, uMsg, wParam, lParam);
}

// ── WinMain ────────────────────────────────────────────────────────────────────

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/,
                   LPSTR pCmdLine, int nCmdShow) {
  AllocConsole();
  freopen("out.txt", "w", stdout);
  freopen("out.txt", "w", stderr);

  std::cout << "--- Scratch Browser Engine v47 initializing ---\n";

  const char CLASS_NAME[] = "ScratchBrowserWindowClass";
  WNDCLASSA wc = {};
  wc.lpfnWndProc   = WindowProc;
  wc.hInstance     = hInstance;
  wc.lpszClassName = CLASS_NAME;
  wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
  wc.style         = CS_OWNDC;
  RegisterClassA(&wc);

  int initial_w = 1024, initial_h = 720;
  HWND hwnd = CreateWindowExA(0, CLASS_NAME, "Scratch Browser", WS_OVERLAPPEDWINDOW,
      CW_USEDEFAULT, CW_USEDEFAULT, initial_w, initial_h, NULL, NULL, hInstance, NULL);

  if (!hwnd) { std::cerr << "Failed to create window!\n"; return 1; }

  // Initialize GDI+
  InitializeCriticalSection(&g_image_cache_cs);
  Gdiplus::GdiplusStartupInput gdip_input;
  Gdiplus::GdiplusStartup(&g_gdip_token, &gdip_input, NULL);
  g_hwnd = hwnd;

  RECT rect;
  GetClientRect(hwnd, &rect);
  int win_w = rect.right - rect.left;
  int win_h = rect.bottom - rect.top;
  g_viewport_height = win_h;
  g_viewport_width  = win_w;

  browser_ui.init(hwnd, win_w, win_h);

  browser_ui.set_navigate_callback([](const std::string &url) {
    navigate_to(url);
  });

  buffer_width  = browser_ui.content_width();
  buffer_height = browser_ui.content_height();
  if (buffer_width < 1)  buffer_width  = 800;
  if (buffer_height < 1) buffer_height = 600;

  bitmap_info.bmiHeader.biSize        = sizeof(bitmap_info.bmiHeader);
  bitmap_info.bmiHeader.biWidth       = buffer_width;
  bitmap_info.bmiHeader.biHeight      = -buffer_height;
  bitmap_info.bmiHeader.biPlanes      = 1;
  bitmap_info.bmiHeader.biBitCount    = 32;
  bitmap_info.bmiHeader.biCompression = BI_RGB;

  int buffer_size = buffer_width * buffer_height * 4;
  if (buffer_size > 0)
    buffer_memory = VirtualAlloc(0, buffer_size, MEM_COMMIT, PAGE_READWRITE);

  app_initialized = true;
  SetTimer(hwnd, 1, 1500, NULL);  // 1.5s periodic repaint for async images

  // Pre-set viewport to maximized size so CSS media queries are evaluated correctly
  {
    RECT work_area;
    SystemParametersInfo(SPI_GETWORKAREA, 0, &work_area, 0);
    int max_w = work_area.right - work_area.left;
    int max_h = work_area.bottom - work_area.top;
    // Subtract window chrome (borders, title bar) to approximate client area
    RECT wr = {0, 0, max_w, max_h};
    AdjustWindowRectEx(&wr, WS_OVERLAPPEDWINDOW, FALSE, 0);
    int chrome_w = (wr.right - wr.left) - max_w;
    int chrome_h = (wr.bottom - wr.top) - max_h;
    g_viewport_width  = max_w - chrome_w;
    g_viewport_height = max_h - chrome_h;
    if (g_viewport_width < 800) g_viewport_width = 800;
    if (g_viewport_height < 600) g_viewport_height = 600;
  }

  std::string cmdLine(pCmdLine);
  if (!cmdLine.empty()) {
    Tab *tab = browser_ui.active_tab();
    if (tab) tab->push_url(cmdLine);
    browser_ui.set_address_text(cmdLine);
    navigate_to(cmdLine);
  } else {
    navigate_to("");
  }

  std::cout << "--- Initialization complete, showing window ---\n";
  ShowWindow(hwnd, SW_SHOWMAXIMIZED);
  // Requery client rect after maximize so the WM_SIZE uses the full viewport
  GetClientRect(hwnd, &rect);
  SendMessageA(hwnd, WM_SIZE, 0, MAKELPARAM(rect.right - rect.left, rect.bottom - rect.top));

  MSG msg = {};
  while (GetMessageA(&msg, NULL, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageA(&msg);
  }

  if (buffer_memory) {
    VirtualFree(buffer_memory, 0, MEM_RELEASE);
    buffer_memory = nullptr;
  }
  if (g_gdip_token) Gdiplus::GdiplusShutdown(g_gdip_token);
  DeleteCriticalSection(&g_image_cache_cs);
  return 0;
}
