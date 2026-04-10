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
void load_page(const std::string &raw_url);
void navigate_to(const std::string &url);

// Accessors for page-level globals (used by WndProc in main.cpp)
std::shared_ptr<Node>& get_g_dom_root();
Stylesheet&            get_g_main_stylesheet();
Stylesheet&            get_g_hover_stylesheet();
Stylesheet&            get_g_focus_stylesheet();
QJSEngine*&            get_g_qjs_engine();
