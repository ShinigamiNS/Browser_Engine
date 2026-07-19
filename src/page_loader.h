#pragma once
#include "image_cache.h"
#include "layout.h"
#include "paint.h"
#include "quickjs_adapter.h"
#include "style.h"
#include <memory>
#include <string>
#include <vector>
#include <windows.h>

// ── ScrollContainer ──────────────────────────────────────────────────────────

struct ScrollContainer {
  Rect bounds;          // clip rect in document coordinates
  float content_height; // total height of children
  float scroll_y;       // current scroll offset within this container
};

extern std::vector<ScrollContainer> g_scroll_containers;

// ── Page globals ─────────────────────────────────────────────────────────────

extern std::shared_ptr<LayoutBox> global_layout_root;
extern DisplayList master_display_list;
extern std::string g_current_page_url;
extern int g_viewport_height;
extern int g_viewport_width;
extern bool g_dark_mode; // prefers-color-scheme:dark toggle

// ── PageResult ───────────────────────────────────────────────────────────────

struct PageResult {
  std::shared_ptr<Node>        dom_root;
  Stylesheet                   main_stylesheet;
  Stylesheet                   hover_stylesheet;
  Stylesheet                   focus_stylesheet;
  std::shared_ptr<LayoutBox>   layout_root;
  DisplayList                  display_list;
  std::vector<ScrollContainer> scroll_containers;
  std::string                  page_url;
  std::string                  page_title;
  std::vector<std::string>     img_urls;
  std::string                  raw_html;
  int                          tab_id = -1;    // tab this load belongs to
  QJSEngine                   *qjs_engine = nullptr; // page's JS engine
  bool                         load_error = false; // built-in error/interstitial
  bool                         cert_error = false; // TLS validation failed
};

// ── Image fetch thread ────────────────────────────────────────────────────────

struct ImgFetchParams {
  std::vector<std::string> urls;
  HWND hwnd;
};

DWORD WINAPI fetch_images_thread(LPVOID param);

// ── Page load ─────────────────────────────────────────────────────────────────

std::vector<ScrollContainer> compute_scroll_containers(
    const DisplayList &dl, const std::vector<ScrollContainer> &prev);
void rebuild_scroll_containers();
void collect_img_urls(const std::shared_ptr<LayoutBox> &box,
                      const std::string &page_url,
                      std::vector<std::string> &out);
void load_page(const std::string &raw_url, int tab_id = -1,
               const std::string &post_body = std::string());
void navigate_to(const std::string &url);
// Form POST navigation: fetches `url` with an x-www-form-urlencoded body.
// History stores the URL only (revisiting via history performs a GET).
void navigate_to_post(const std::string &url, const std::string &post_body);

// URL utilities (page_loader.cpp)
std::string normalize_url_input(const std::string &raw);
std::string file_url_to_path(const std::string &url);
std::string percent_decode(const std::string &s);
std::string percent_encode_query(const std::string &s);
std::string resolve_url(const std::string &src, const std::string &page_url);
bool is_content_navigation_allowed(const std::string &target,
                                   const std::string &page_url);

// History & bookmarks (page_loader.cpp)
void record_history(const std::string &url, const std::string &title);
bool add_bookmark(const std::string &url, const std::string &title);

// Accessors for page-level globals (used by WndProc in main.cpp)
std::shared_ptr<Node>& get_g_dom_root();
Stylesheet&            get_g_main_stylesheet();
Stylesheet&            get_g_hover_stylesheet();
Stylesheet&            get_g_focus_stylesheet();
QJSEngine*&            get_g_qjs_engine();
