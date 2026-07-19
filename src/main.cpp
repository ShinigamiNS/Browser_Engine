// main.cpp — Win32 entry point + WndProc
// All heavy subsystems live in image_cache.cpp, page_loader.cpp, renderer.cpp

#include "browser_ui.h"
#include "css_parser.h"
#include "image_cache.h"
#include "layout.h"
#include "lexbor_adapter.h"
#include "net.h"
#include "page_loader.h"
#include "paint.h"
#include "quickjs_adapter.h"
#include "renderer.h"
#include "style.h"
#include <algorithm>
#include <functional>
#include <iostream>
#include <map>
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

static void clamp_main_scroll(); // defined below (needs zoom state)
extern float g_zoom_level;

static void scroll_to_find_match(int match_idx) {
  if (match_idx < 0 || match_idx >= (int)g_find_matches.size()) return;
  auto &m = g_find_matches[match_idx];
  int content_h = browser_ui.content_height();
  // Scroll so match is visible (centered vertically); match coords are in
  // layout space, scroll_y is in zoomed pixel space
  scroll_y = (int)(m.y * g_zoom_level - content_h / 2.f);
  clamp_main_scroll();
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

// Total document height in screen pixels (layout height scaled by zoom)
static float document_pixel_height() {
  int content_h = browser_ui.content_height();
  float total_h = global_layout_root
                      ? global_layout_root->dimensions.content.height
                      : (float)content_h;
  return total_h * g_zoom_level;
}

// Keep scroll_y within [0, doc_height - viewport]
static void clamp_main_scroll() {
  int content_h = browser_ui.content_height();
  float max_scroll = document_pixel_height() - (float)content_h;
  if (max_scroll < 0.f) max_scroll = 0.f;
  if (scroll_y < 0) scroll_y = 0;
  if (scroll_y > (int)max_scroll) scroll_y = (int)max_scroll;
}

// ── Context Menu IDs ──────────────────────────────────────────────────────────
#define IDM_COPY          40001
#define IDM_SELECT_ALL    40002
#define IDM_OPEN_LINK     40003
#define IDM_COPY_LINK     40004
#define IDM_RELOAD        40005
#define IDM_VIEW_SOURCE   40006
#define IDM_COPY_IMAGE    40007
#define IDM_SAVE_LINK     40008
#define IDM_SAVE_IMAGE    40009

// ── Downloads ─────────────────────────────────────────────────────────────────
// User-initiated only (context-menu "Save link/image as"). Fetches the bytes on
// a worker thread and writes them to the user's Downloads folder.

struct DownloadArg { std::string url; };

static std::string downloads_dir() {
  char profile[MAX_PATH] = {0};
  DWORD n = GetEnvironmentVariableA("USERPROFILE", profile, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return "";
  return std::string(profile) + "\\Downloads\\";
}

// Derive a safe local filename from a URL's last path segment.
static std::string filename_from_url(const std::string &url) {
  std::string u = url;
  size_t q = u.find_first_of("?#");
  if (q != std::string::npos) u = u.substr(0, q);
  size_t slash = u.find_last_of('/');
  std::string name = (slash == std::string::npos) ? u : u.substr(slash + 1);
  // Strip characters illegal in Windows filenames.
  std::string safe;
  for (char c : name) {
    if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' ||
        c == '"' || c == '<' || c == '>' || c == '|' || (unsigned char)c < 32)
      continue;
    safe += c;
  }
  if (safe.empty()) safe = "download";
  return safe;
}

static DWORD WINAPI download_thread(LPVOID param) {
  DownloadArg *arg = (DownloadArg *)param;
  std::string url = arg->url;
  delete arg;

  std::string bytes;
  if (url.substr(0, 5) == "data:") {
    bytes = decode_data_uri(url);
  } else if (url.substr(0, 4) == "http") {
    bytes = http_fetch(url, "GET", "", g_private_mode).body;
  } else {
    browser_ui.set_status("Cannot download: " + url);
    PostMessage(g_hwnd, WM_USER + 5, 0, 0);
    return 0;
  }
  if (bytes.empty()) {
    browser_ui.set_status("Download failed: " + url);
    PostMessage(g_hwnd, WM_USER + 5, 0, 0);
    return 0;
  }

  std::string dir = downloads_dir();
  std::string base = filename_from_url(url);
  // Avoid clobbering an existing file: append (1), (2), ...
  std::string path = dir + base;
  for (int i = 1; GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
       ++i) {
    size_t dot = base.find_last_of('.');
    std::string stem = (dot == std::string::npos) ? base : base.substr(0, dot);
    std::string ext = (dot == std::string::npos) ? "" : base.substr(dot);
    path = dir + stem + " (" + std::to_string(i) + ")" + ext;
  }

  HANDLE hf = CreateFileA(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, NULL);
  if (hf == INVALID_HANDLE_VALUE) {
    browser_ui.set_status("Could not write file to Downloads");
    PostMessage(g_hwnd, WM_USER + 5, 0, 0);
    return 0;
  }
  DWORD written = 0;
  WriteFile(hf, bytes.data(), (DWORD)bytes.size(), &written, NULL);
  CloseHandle(hf);
  browser_ui.set_status("Saved to Downloads: " + base + " (" +
                        std::to_string(bytes.size() / 1024) + " KB)");
  PostMessage(g_hwnd, WM_USER + 5, 0, 0);
  return 0;
}

static void start_download(const std::string &url) {
  browser_ui.set_status("Downloading " + url + " ...");
  DownloadArg *arg = new DownloadArg{url};
  HANDLE t = CreateThread(NULL, 0, download_thread, arg, 0, NULL);
  if (t) CloseHandle(t);
}

// ── View Source ───────────────────────────────────────────────────────────────
std::string g_raw_html_source;

// ── Per-tab page state ────────────────────────────────────────────────────────
// Each tab keeps its own loaded document so switching tabs swaps content
// instead of refetching the page.

struct TabPageState {
  std::shared_ptr<Node>        dom_root;
  Stylesheet                   main_stylesheet;
  Stylesheet                   hover_stylesheet;
  Stylesheet                   focus_stylesheet;
  std::shared_ptr<LayoutBox>   layout_root;
  DisplayList                  display_list;
  std::vector<ScrollContainer> scroll_containers;
  std::string                  page_url;
  std::string                  raw_html;
  int                          scroll_y = 0;
  QJSEngine                   *qjs_engine = nullptr;
  BrowserUI::SecurityLevel     security = BrowserUI::SecurityLevel::None;
};

// Classify the address-bar security state from the final URL + load result.
static BrowserUI::SecurityLevel security_for(const std::string &url,
                                             bool cert_error) {
  if (cert_error) return BrowserUI::SecurityLevel::Danger;
  if (url.substr(0, 8) == "https://") return BrowserUI::SecurityLevel::Secure;
  if (url.substr(0, 7) == "http://") return BrowserUI::SecurityLevel::Insecure;
  if (url.substr(0, 7) == "file://" ||
      url.substr(0, 12) == "view-source:")
    return BrowserUI::SecurityLevel::Local;
  return BrowserUI::SecurityLevel::None;
}

static std::map<int, TabPageState> g_tab_pages;
static int g_installed_tab_id = -1; // tab whose page is in the globals

// Free a closed tab's cached page (and its JS engine).
static void drop_tab_page(int tab_id) {
  auto it = g_tab_pages.find(tab_id);
  if (it == g_tab_pages.end()) return;
  if (it->second.qjs_engine) {
    if (get_g_qjs_engine() == it->second.qjs_engine)
      get_g_qjs_engine() = nullptr;
    qjs_destroy(it->second.qjs_engine);
  }
  g_tab_pages.erase(it);
  if (g_installed_tab_id == tab_id) g_installed_tab_id = -1;
}

// Swap a tab's cached page into the live globals. Returns false if the tab
// has no cached page yet (caller should navigate instead).
static bool install_tab_page(int tab_id) {
  auto it = g_tab_pages.find(tab_id);
  if (it == g_tab_pages.end()) return false;

  // Remember scroll position of the page we're switching away from
  if (g_installed_tab_id != tab_id) {
    auto cur = g_tab_pages.find(g_installed_tab_id);
    if (cur != g_tab_pages.end()) cur->second.scroll_y = scroll_y;
  }

  TabPageState &st = it->second;
  get_g_dom_root()          = st.dom_root;
  get_g_main_stylesheet()   = st.main_stylesheet;
  get_g_hover_stylesheet()  = st.hover_stylesheet;
  get_g_focus_stylesheet()  = st.focus_stylesheet;
  get_g_qjs_engine()        = st.qjs_engine;
  global_layout_root        = st.layout_root;
  g_current_page_url        = st.page_url;
  g_raw_html_source         = st.raw_html;

  focused_box = nullptr;
  has_selection = false;
  g_has_selection = false;
  g_find_bar_open = false;
  g_find_query.clear();
  g_find_matches.clear();
  g_find_total_matches = 0;

  // Relayout at the current viewport (window size / zoom may have changed
  // since this page was cached)
  if (global_layout_root && buffer_width > 0) {
    Dimensions viewport;
    viewport.content.width  = (float)buffer_width / g_zoom_level;
    viewport.content.height = 0.0f;
    global_layout_root->layout(viewport);
    master_display_list = build_display_list(global_layout_root);
    rebuild_scroll_containers();
  } else {
    master_display_list = st.display_list;
    g_scroll_containers = st.scroll_containers;
  }

  scroll_y = st.scroll_y;
  clamp_main_scroll();
  g_installed_tab_id = tab_id;
  browser_ui.set_security_level(st.security);
  browser_ui.set_status(st.page_url.empty() ? "Ready" : st.page_url);
  return true;
}

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
  // Save the scroll position of the tab we're leaving
  {
    auto cur = g_tab_pages.find(g_installed_tab_id);
    if (cur != g_tab_pages.end()) cur->second.scroll_y = scroll_y;
  }
  std::string vs_url = "view-source:" + g_current_page_url;
  int vs_tab_id = browser_ui.add_tab(vs_url, tab_title);
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
      get_g_qjs_engine() = nullptr; // view-source pages run no JS
      global_layout_root = lr;
      master_display_list = build_display_list(lr);
      rebuild_scroll_containers();
      scroll_y = 0;
      g_current_page_url = vs_url;
      // Register the view-source page as this tab's cached state
      TabPageState vs;
      vs.dom_root        = root;
      vs.main_stylesheet = ss;
      vs.layout_root     = lr;
      vs.display_list    = master_display_list;
      vs.page_url        = vs_url;
      vs.raw_html        = source_html;
      g_tab_pages[vs_tab_id] = std::move(vs);
      g_installed_tab_id = vs_tab_id;
    }
  }
  InvalidateRect(hwnd, NULL, FALSE);
}

// ── Form submission ───────────────────────────────────────────────────────────

// Walk up from `start` to the enclosing <form>, gather its successful
// controls, and navigate to action?name=value&... (GET). Returns false if
// there is no enclosing form.
static bool submit_form_for_node(std::shared_ptr<Node> start) {
  auto form = start;
  while (form &&
         !(form->type == NodeType::Element && form->data == "form"))
    form = form->parent.lock();
  if (!form) return false;

  auto attr_of = [](const std::shared_ptr<Node> &n,
                    const char *key) -> std::string {
    auto it = n->attributes.find(key);
    return it == n->attributes.end() ? "" : it->second;
  };

  std::string query;
  auto append_pair = [&](const std::string &name, const std::string &value) {
    if (name.empty()) return;
    if (!query.empty()) query += "&";
    query += percent_encode_query(name) + "=" + percent_encode_query(value);
  };

  std::function<void(const std::shared_ptr<Node> &)> collect;
  collect = [&](const std::shared_ptr<Node> &n) {
    if (!n) return;
    if (n->type == NodeType::Element) {
      const std::string &tag = n->data;
      if (tag == "input") {
        std::string type = attr_of(n, "type");
        for (auto &c : type) c = (char)tolower((unsigned char)c);
        bool skip = (type == "submit" || type == "button" || type == "reset" ||
                     type == "file" || type == "image");
        bool needs_checked = (type == "checkbox" || type == "radio");
        if (!skip && (!needs_checked || n->attributes.count("checked")))
          append_pair(attr_of(n, "name"), attr_of(n, "value"));
      } else if (tag == "textarea") {
        std::string value;
        for (auto &c : n->children)
          if (c->type == NodeType::Text) value += c->data;
        append_pair(attr_of(n, "name"), value);
      } else if (tag == "select") {
        std::shared_ptr<Node> chosen, first_opt;
        std::function<void(const std::shared_ptr<Node> &)> find_opt;
        find_opt = [&](const std::shared_ptr<Node> &o) {
          if (!o) return;
          if (o->type == NodeType::Element && o->data == "option") {
            if (!first_opt) first_opt = o;
            if (!chosen && o->attributes.count("selected")) chosen = o;
          }
          for (auto &c : o->children) find_opt(c);
        };
        find_opt(n);
        if (!chosen) chosen = first_opt;
        if (chosen) {
          std::string value = attr_of(chosen, "value");
          if (value.empty())
            for (auto &c : chosen->children)
              if (c->type == NodeType::Text) value += c->data;
          append_pair(attr_of(n, "name"), value);
        }
      }
    }
    for (auto &c : n->children) collect(c);
  };
  collect(form);

  std::string action = attr_of(form, "action");
  std::string url = action.empty() ? g_current_page_url
                                   : resolve_url(action, g_current_page_url);
  // Security gate: don't let a web form POST/GET to a local file:// target.
  if (!is_content_navigation_allowed(url, g_current_page_url)) {
    browser_ui.set_status("Blocked form submission to " + url);
    return true;
  }
  std::string method = attr_of(form, "method");
  for (auto &c : method) c = (char)tolower((unsigned char)c);

  // Replace any existing query/fragment with ours
  size_t cut = url.find_first_of("?#");
  if (cut != std::string::npos) url = url.substr(0, cut);
  bool is_post = (method == "post");
  if (!is_post && !query.empty()) url += "?" + query;

  Tab *tab = browser_ui.active_tab();
  if (tab) {
    tab->push_url(url);
    browser_ui.set_address_text(url);
  }
  if (is_post)
    navigate_to_post(url, query);
  else
    navigate_to(url);
  return true;
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

      // If the click landed on a non-editable wrapper, focus the first text
      // control inside it (clicking a search box's container should still
      // focus the search field, like real browsers do).
      {
        auto is_editable = [](const std::shared_ptr<LayoutBox> &b) -> bool {
          if (!b || !b->style_node || !b->style_node->node) return false;
          const std::string &tag = b->style_node->node->data;
          if (tag == "textarea") return true;
          if (tag != "input") return false;
          std::string type;
          auto it = b->style_node->node->attributes.find("type");
          if (it != b->style_node->node->attributes.end()) type = it->second;
          for (auto &c : type) c = (char)tolower((unsigned char)c);
          return type == "" || type == "text" || type == "search" ||
                 type == "url" || type == "email" || type == "tel" ||
                 type == "number" || type == "password";
        };
        if (focused_box && !is_editable(focused_box)) {
          std::function<std::shared_ptr<LayoutBox>(
              const std::shared_ptr<LayoutBox> &)> find_editable;
          find_editable = [&](const std::shared_ptr<LayoutBox> &b)
              -> std::shared_ptr<LayoutBox> {
            if (!b) return nullptr;
            if (is_editable(b)) return b;
            for (auto &c : b->children) {
              auto r = find_editable(c);
              if (r) return r;
            }
            return nullptr;
          };
          auto editable = find_editable(focused_box);
          if (editable) focused_box = editable;
        }
      }

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
          bool has_onclick = btn_node->attributes.count("onclick") > 0;
          std::string btn_type = btn_node->attributes.count("type")
                                     ? btn_node->attributes["type"]
                                     : "";
          // <button type="button"> never submits; plain <button> does
          bool submits = (btn_node->data != "button" || btn_type != "button");
          if (!has_onclick && submits && submit_form_for_node(btn_node))
            focused_box = nullptr;
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
            std::string resolved = resolve_url(href, g_current_page_url);
            // Security gate: block content-initiated navigation to local
            // files or other dangerous schemes from a web page.
            if (!is_content_navigation_allowed(resolved, g_current_page_url)) {
              std::cerr << "Blocked navigation to " << resolved
                        << " from " << g_current_page_url << "\n";
              browser_ui.set_status("Blocked: " + resolved);
            } else {
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
      // Scroll container bounds are in layout coords; convert from pixels
      float doc_x = (float)wmx / g_zoom_level;
      float doc_y = (float)(wmy - browser_ui.content_y() + scroll_y) / g_zoom_level;
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
      clamp_main_scroll();
    }
    InvalidateRect(hwnd, NULL, FALSE);
  } break;

  case WM_KEYDOWN: {
    if (!app_initialized) break;
    browser_ui.on_key_down((int)wParam);

    if (GetKeyState(VK_CONTROL) & 0x8000) {
      bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
      if (wParam == 'N' && shift) {
        // Ctrl+Shift+N — toggle private / incognito browsing.
        g_private_mode = !g_private_mode;
        browser_ui.set_private_mode(g_private_mode);
        browser_ui.set_status(g_private_mode
                                  ? "Private browsing ON — cookies and cache "
                                    "are not stored"
                                  : "Private browsing OFF");
        InvalidateRect(hwnd, NULL, FALSE);
      } else if (wParam == VK_DELETE && shift) {
        // Ctrl+Shift+Del — clear cookies + cache.
        int n = clear_browsing_data();
        browser_ui.set_status("Cleared browsing data (" + std::to_string(n) +
                              " cache entries + cookies)");
        InvalidateRect(hwnd, NULL, FALSE);
      } else if (wParam == 'D' && shift) {
        // Ctrl+Shift+D — toggle dark mode, then reload so media queries
        // re-evaluate against the new prefers-color-scheme.
        g_dark_mode = !g_dark_mode;
        browser_ui.set_status(g_dark_mode ? "Dark mode ON" : "Dark mode OFF");
        Tab *t = browser_ui.active_tab();
        if (t && !t->url.empty()) navigate_to(t->url);
        InvalidateRect(hwnd, NULL, FALSE);
      } else if (wParam == 'T') {
        browser_ui.add_tab("", "New Tab");
        browser_ui.focus_address_bar();
        navigate_to("");
        InvalidateRect(hwnd, NULL, FALSE);
      } else if (wParam == 'W') {
        Tab *closing = browser_ui.active_tab();
        if (closing) drop_tab_page(closing->id);
        browser_ui.close_tab(browser_ui.active_tab_index());
        Tab *t = browser_ui.active_tab();
        if (t && !install_tab_page(t->id)) navigate_to(t->url);
        InvalidateRect(hwnd, NULL, FALSE);
      } else if (wParam == 'D') {
        // Ctrl+D — bookmark the current page.
        Tab *t = browser_ui.active_tab();
        if (t && !t->url.empty()) {
          bool added = add_bookmark(t->url, t->title);
          browser_ui.set_status(added ? "Bookmarked: " + t->url
                                       : "Already bookmarked");
          InvalidateRect(hwnd, NULL, FALSE);
        }
      } else if (wParam == 'H') {
        // Ctrl+H — open history.
        Tab *t = browser_ui.active_tab();
        if (t) { t->push_url("about:history");
                 browser_ui.set_address_text("about:history"); }
        navigate_to("about:history");
      } else if (wParam == 'B' && (GetKeyState(VK_SHIFT) & 0x8000)) {
        // Ctrl+Shift+B — open bookmarks.
        Tab *t = browser_ui.active_tab();
        if (t) { t->push_url("about:bookmarks");
                 browser_ui.set_address_text("about:bookmarks"); }
        navigate_to("about:bookmarks");
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

    // ── Keyboard scrolling ──
    // Only when typing isn't being captured by the address bar / find bar /
    // a focused <input>, and no Ctrl/Alt chord is active.
    if (!browser_ui.is_address_focused() &&
        !(GetKeyState(VK_CONTROL) & 0x8000) &&
        !(GetKeyState(VK_MENU) & 0x8000)) {
      bool text_capture =
          g_find_bar_open ||
          (focused_box && focused_box->style_node &&
           focused_box->style_node->node &&
           focused_box->style_node->node->data == "input");
      int content_h = browser_ui.content_height();
      int page_step = content_h > 60 ? content_h - 40 : content_h;
      bool scrolled = false;
      switch (wParam) {
      case VK_DOWN:  scroll_y += SCROLL_STEP; scrolled = true; break;
      case VK_UP:    scroll_y -= SCROLL_STEP; scrolled = true; break;
      case VK_NEXT:  scroll_y += page_step;   scrolled = true; break;
      case VK_PRIOR: scroll_y -= page_step;   scrolled = true; break;
      case VK_HOME:  scroll_y = 0;            scrolled = true; break;
      case VK_END:   scroll_y = (int)document_pixel_height(); scrolled = true; break;
      case VK_SPACE:
        if (!text_capture) {
          scroll_y += (GetKeyState(VK_SHIFT) & 0x8000) ? -page_step : page_step;
          scrolled = true;
        }
        break;
      default: break;
      }
      if (scrolled) {
        clamp_main_scroll();
        InvalidateRect(hwnd, NULL, FALSE);
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
        (focused_box->style_node->node->data == "input" ||
         focused_box->style_node->node->data == "textarea")) {
      auto node = focused_box->style_node->node;
      bool is_textarea = (node->data == "textarea");

      // <input> keeps its value in the value attribute; <textarea> keeps it
      // in its inner text node (that's where paint reads it from).
      std::string *val = nullptr;
      if (is_textarea) {
        std::shared_ptr<Node> text_child;
        for (auto &c : node->children)
          if (c->type == NodeType::Text) { text_child = c; break; }
        if (!text_child) {
          text_child = TextNode("");
          node->append_child(text_child);
        }
        val = &text_child->data;
      } else {
        val = &node->attributes["value"];
      }

      if (typed == '\b') {
        if (!val->empty()) val->pop_back();
      } else if (typed >= 32 && typed <= 126) {
        *val += typed;
      } else if (typed == '\r') {
        // Enter submits the enclosing form; fall back to a web search
        if (!submit_form_for_node(node) && !val->empty()) {
          std::string nav_url =
              "https://www.google.com/search?q=" + percent_encode_query(*val);
          Tab *tab = browser_ui.active_tab();
          if (tab) {
            tab->push_url(nav_url);
            browser_ui.set_address_text(nav_url);
          }
          navigate_to(nav_url);
        }
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
    // Page load complete — cache under its tab and install if active
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

    Tab *active = browser_ui.active_tab();
    int active_id = active ? active->id : -1;
    int tid = (pr->tab_id >= 0) ? pr->tab_id : active_id;

    // Destroy the tab's previous JS engine (safe here on the main thread;
    // detach it from the global pointer first if it is the live engine)
    auto old_it = g_tab_pages.find(tid);
    if (old_it != g_tab_pages.end() && old_it->second.qjs_engine &&
        old_it->second.qjs_engine != pr->qjs_engine) {
      if (g_qjs_engine == old_it->second.qjs_engine)
        g_qjs_engine = nullptr;
      qjs_destroy(old_it->second.qjs_engine);
      old_it->second.qjs_engine = nullptr;
    }

    TabPageState st;
    st.dom_root          = pr->dom_root;
    st.main_stylesheet   = std::move(pr->main_stylesheet);
    st.hover_stylesheet  = std::move(pr->hover_stylesheet);
    st.focus_stylesheet  = std::move(pr->focus_stylesheet);
    st.layout_root       = pr->layout_root;
    st.display_list      = std::move(pr->display_list);
    st.scroll_containers = std::move(pr->scroll_containers);
    st.page_url          = pr->page_url;
    st.raw_html          = std::move(pr->raw_html);
    st.scroll_y          = 0;
    st.qjs_engine        = pr->qjs_engine;
    st.security          = security_for(pr->page_url, pr->cert_error);
    g_tab_pages[tid]     = std::move(st);

    // Update the owning tab's title + URL (it may no longer be the active
    // tab). pr->page_url is the FINAL url after any redirects, so this keeps
    // the tab history and address bar honest about where we actually landed.
    for (int i = 0; i < browser_ui.tab_count(); ++i) {
      Tab *t = browser_ui.tab_at(i);
      if (!t || t->id != tid) continue;
      if (!pr->page_title.empty()) t->title = pr->page_title;
      else if (!pr->page_url.empty()) t->title = pr->page_url;
      if (!pr->page_url.empty() && pr->page_url != t->url) {
        t->url = pr->page_url;
        if (t->history_index >= 0 &&
            t->history_index < (int)t->history.size())
          t->history[t->history_index] = pr->page_url;
      }
      break;
    }
    bool url_is_final = (tid == active_id && !pr->page_url.empty());
    std::string final_url = pr->page_url;

    // Record successful loads in history (skipped for error pages and
    // incognito — record_history() enforces the private-mode rule itself).
    if (!pr->load_error)
      record_history(pr->page_url, pr->page_title);

    if (!pr->img_urls.empty()) {
      ImgFetchParams *p = new ImgFetchParams();
      p->urls = pr->img_urls;
      p->hwnd = g_hwnd;
      HANDLE ht = CreateThread(NULL, 0, fetch_images_thread, p, 0, NULL);
      if (ht) CloseHandle(ht);
    }
    delete pr;

    if (tid == active_id) {
      // Force a fresh install even if this tab was already showing
      g_installed_tab_id = -1;
      install_tab_page(tid);
      scroll_y = 0;
      // Reflect the final (post-redirect) URL in the address bar, unless the
      // user has since focused it to type something else.
      if (url_is_final && !browser_ui.is_address_focused())
        browser_ui.set_address_text(final_url);
    }

    browser_ui.set_loading(false);
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
          if ((cur->data == "img" || cur->data == "svg") &&
              cur->attributes.count("src")) {
            is_image = true; img_src = cur->attributes["src"];
          }
          cur = cur->parent.lock();
        }
      }

      if (has_selection) AppendMenuA(hMenu, MF_STRING, IDM_COPY, "Copy\tCtrl+C");
      if (is_link) {
        AppendMenuA(hMenu, MF_STRING, IDM_OPEN_LINK, "Open Link");
        AppendMenuA(hMenu, MF_STRING, IDM_COPY_LINK, "Copy Link Address");
        AppendMenuA(hMenu, MF_STRING, IDM_SAVE_LINK, "Save Link As...");
      }
      if (is_image) {
        AppendMenuA(hMenu, MF_STRING, IDM_COPY_IMAGE, "Copy Image Address");
        AppendMenuA(hMenu, MF_STRING, IDM_SAVE_IMAGE, "Save Image As...");
      }
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
        std::string resolved = resolve_url(s_ctx_link_url, g_current_page_url);
        if (is_content_navigation_allowed(resolved, g_current_page_url)) {
          Tab *t = browser_ui.active_tab();
          if (t) { t->push_url(resolved); browser_ui.set_address_text(resolved); }
          navigate_to(resolved);
        } else {
          browser_ui.set_status("Blocked: " + resolved);
        }
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
    case IDM_SAVE_LINK: {
      if (!s_ctx_link_url.empty())
        start_download(resolve_url(s_ctx_link_url, g_current_page_url));
      break;
    }
    case IDM_SAVE_IMAGE: {
      if (!s_ctx_img_src.empty()) {
        std::string u = s_ctx_img_src;
        // Rasterized SVGs carry a __svg_ cache key, not a real URL — skip.
        if (u.substr(0, 6) != "__svg_")
          start_download(u.substr(0, 5) == "data:"
                             ? u
                             : resolve_url(u, g_current_page_url));
        else
          browser_ui.set_status("Cannot save inline SVG");
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

  case WM_USER + 5:
    // A download finished (status already updated) — repaint status bar.
    InvalidateRect(hwnd, NULL, FALSE);
    break;

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

  browser_ui.set_tab_switch_callback([](int tab_id) {
    if (!install_tab_page(tab_id)) {
      // No cached page for this tab — load its URL (or the homepage)
      Tab *t = browser_ui.active_tab();
      navigate_to(t ? t->url : "");
    }
    InvalidateRect(g_hwnd, NULL, FALSE);
  });

  browser_ui.set_tab_close_callback([](int tab_id) {
    drop_tab_page(tab_id);
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

  // Parse the command line: optional --incognito/--private flag plus a URL.
  std::string cmdLine(pCmdLine);
  {
    auto strip_flag = [&](const std::string &flag) {
      size_t p = cmdLine.find(flag);
      if (p == std::string::npos) return false;
      cmdLine.erase(p, flag.size());
      return true;
    };
    bool incog = strip_flag("--incognito") | strip_flag("--private") |
                 strip_flag("-incognito") | strip_flag("-private");
    if (incog) {
      g_private_mode = true;
      browser_ui.set_private_mode(true);
      browser_ui.set_status("Private browsing ON");
    }
    if (strip_flag("--dark") | strip_flag("-dark")) g_dark_mode = true;
    // Trim surrounding whitespace left by flag removal.
    size_t b = cmdLine.find_first_not_of(" \t");
    size_t e = cmdLine.find_last_not_of(" \t");
    cmdLine = (b == std::string::npos) ? "" : cmdLine.substr(b, e - b + 1);
  }
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
