#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <windows.h>

// ============================================================================
// Browser UI Backend — Manages the browser's own "chrome"
// (address bar, navigation buttons, tab bar, status bar)
//
// Rendering Toolkit: Win32 GDI (CreateFont, FillRect, DrawText, etc.)
// Event Loop:        Windows message pump with custom hit-testing
// ============================================================================

// ──────────────────────────────────────────────
//  Geometry Helpers
// ──────────────────────────────────────────────
struct UIRect {
  int x = 0, y = 0, w = 0, h = 0;
  bool contains(int px, int py) const {
    return px >= x && px < x + w && py >= y && py < y + h;
  }
};

// ──────────────────────────────────────────────
//  Tab Data
// ──────────────────────────────────────────────
struct Tab {
  std::string title = "New Tab";
  std::string url;
  int id = 0;
  // Per-tab navigation history
  std::vector<std::string> history;
  int history_index = -1;

  bool can_go_back() const { return history_index > 0; }
  bool can_go_forward() const {
    return history_index >= 0 && history_index < (int)history.size() - 1;
  }
  void push_url(const std::string &new_url) {
    // Trim forward history when navigating to a new page
    if (history_index >= 0 && history_index < (int)history.size() - 1) {
      history.erase(history.begin() + history_index + 1, history.end());
    }
    history.push_back(new_url);
    history_index = (int)history.size() - 1;
    url = new_url;
  }
  std::string go_back() {
    if (can_go_back()) {
      --history_index;
      url = history[history_index];
    }
    return url;
  }
  std::string go_forward() {
    if (can_go_forward()) {
      ++history_index;
      url = history[history_index];
    }
    return url;
  }
};

// ──────────────────────────────────────────────
//  Hit-test result enum
// ──────────────────────────────────────────────
enum class UIHitResult {
  None,
  TabBar,
  TabItem,
  TabClose,
  NewTab,
  BackButton,
  ForwardButton,
  ReloadButton,
  HomeButton,
  AddressBar,
  GoButton,
  StatusBar,
  ContentArea,
};

// ──────────────────────────────────────────────
//  Callback types for communicating with main
// ──────────────────────────────────────────────
using NavigateCallback = std::function<void(const std::string &url)>;

// ──────────────────────────────────────────────
//  Color palette for the chrome
// ──────────────────────────────────────────────
struct UIColors {
  // Tab bar
  COLORREF tab_bar_bg = RGB(34, 36, 42);           // Dark charcoal
  COLORREF tab_active_bg = RGB(48, 50, 58);        // Slightly lighter
  COLORREF tab_inactive_bg = RGB(38, 40, 46);      // Darker inactive
  COLORREF tab_hover_bg = RGB(55, 58, 66);         // Hover highlight
  COLORREF tab_text_active = RGB(230, 232, 240);   // Bright text
  COLORREF tab_text_inactive = RGB(140, 145, 160); // Dimmed text
  COLORREF tab_close_fg = RGB(160, 165, 175);      // Close button "×"
  COLORREF tab_close_hover = RGB(232, 72, 72);     // Red on hover

  // Toolbar
  COLORREF toolbar_bg = RGB(48, 50, 58);      // Matches active tab
  COLORREF toolbar_border = RGB(60, 63, 72);  // Subtle separator
  COLORREF btn_fg = RGB(180, 185, 200);       // Button icon color
  COLORREF btn_hover_bg = RGB(65, 68, 78);    // Hover circle
  COLORREF btn_disabled_fg = RGB(80, 84, 95); // Greyed-out

  // Address bar
  COLORREF addr_bg = RGB(30, 32, 38);              // Dark input field
  COLORREF addr_border = RGB(70, 73, 82);          // Border
  COLORREF addr_border_focus = RGB(100, 140, 255); // Blue focus ring
  COLORREF addr_text = RGB(220, 224, 235);         // Input text
  COLORREF addr_placeholder = RGB(100, 105, 120);  // Placeholder text
  COLORREF addr_selection = RGB(60, 100, 200);     // Selection highlight

  // Go button
  COLORREF go_bg = RGB(80, 130, 255);        // Blue accent
  COLORREF go_bg_hover = RGB(100, 150, 255); // Light blue hover
  COLORREF go_text = RGB(255, 255, 255);     // White text

  // Status bar
  COLORREF status_bg = RGB(34, 36, 42);
  COLORREF status_text = RGB(120, 125, 140);

  // Content splitter line
  COLORREF content_border = RGB(22, 24, 28);
};

// ──────────────────────────────────────────────
//  BrowserUI — the main UI backend class
// ──────────────────────────────────────────────
class BrowserUI {
public:
  // ── Layout constants (enum to avoid ODR issues with MinGW) ──
  enum {
    TAB_BAR_HEIGHT = 36,
    TOOLBAR_HEIGHT = 44,
    STATUS_BAR_HEIGHT = 24,
    CHROME_HEIGHT = TAB_BAR_HEIGHT + TOOLBAR_HEIGHT,

    TAB_WIDTH = 200,
    TAB_MIN_WIDTH = 60,
    TAB_CLOSE_SIZE = 16,
    NEW_TAB_BTN_W = 32,
    NAV_BTN_SIZE = 30,
    NAV_BTN_SPACING = 4,
    GO_BTN_WIDTH = 48,
    ADDR_BAR_HEIGHT = 30,
  };

  BrowserUI();
  ~BrowserUI();

  // ── Initialization ──
  void init(HWND hwnd, int window_width, int window_height);
  void resize(int window_width, int window_height);

  // ── Painting ──
  void paint(HDC hdc);

  // ── Event handling ──
  UIHitResult hit_test(int x, int y, int *out_tab_index = nullptr);
  void on_mouse_move(int x, int y);
  void on_mouse_down(int x, int y);
  void on_mouse_up(int x, int y);
  void on_char(char ch);
  void on_key_down(int vk);

  // ── Tab management ──
  int add_tab(const std::string &url = "",
              const std::string &title = "New Tab");
  void close_tab(int index);
  void set_active_tab(int index);
  int active_tab_index() const { return active_tab_; }
  Tab *active_tab();
  const Tab *active_tab() const;
  int tab_count() const { return (int)tabs_.size(); }
  Tab *tab_at(int i) {
    return (i >= 0 && i < (int)tabs_.size()) ? &tabs_[i] : nullptr;
  }

  // ── Address bar ──
  void set_address_text(const std::string &text);
  std::string get_address_text() const { return address_text_; }
  bool is_address_focused() const { return address_focused_; }
  void focus_address_bar();
  void blur_address_bar();
  void address_click(int x, int y);  // click-to-position cursor
  void invalidate_toolbar();         // repaint only toolbar area

  // ── Status bar ──
  void set_status(const std::string &text) { status_text_ = text; }

  // ── Navigation state ──
  void set_loading(bool loading) { is_loading_ = loading; }
  bool is_loading() const { return is_loading_; }

  // ── Callbacks ──
  void set_navigate_callback(NavigateCallback cb) { on_navigate_ = cb; }

  // ── Accessors ──
  int content_y() const { return CHROME_HEIGHT; }
  int content_height() const {
    int h = window_h_ - CHROME_HEIGHT - STATUS_BAR_HEIGHT;
    return h > 0 ? h : 1;
  }
  int content_width() const { return window_w_; }

private:
  // ── Painting helpers ──
  void paint_tab_bar(HDC hdc);
  void paint_toolbar(HDC hdc);
  void paint_status_bar(HDC hdc);
  void draw_nav_button(HDC hdc, const UIRect &r, const wchar_t *symbol,
                       bool enabled, bool hovered);
  void draw_rounded_rect(HDC hdc, UIRect r, int radius, COLORREF fill,
                         COLORREF border);

  // ── Layout recalculation ──
  void recalc_layout();
  UIRect tab_rect(int index) const;
  UIRect tab_close_rect(int index) const;
  UIRect new_tab_rect() const;

  // ── State ──
  HWND hwnd_ = nullptr;
  int window_w_ = 800, window_h_ = 600;
  std::vector<Tab> tabs_;
  int active_tab_ = -1;
  int next_tab_id_ = 1;

  // Address bar
  std::string address_text_;
  bool address_focused_ = false;
  int cursor_pos_ = 0;       // Cursor position in address text
  int selection_start_ = -1; // -1 = no selection

  // Hover tracking
  UIHitResult hover_target_ = UIHitResult::None;
  int hover_tab_index_ = -1;

  // Toolbar button rects (recalculated on resize)
  UIRect back_btn_;
  UIRect forward_btn_;
  UIRect reload_btn_;
  UIRect home_btn_;
  UIRect address_rect_;
  UIRect go_btn_;

  // Status
  std::string status_text_ = "Ready";
  bool is_loading_ = false;

  // Colors
  UIColors colors_;

  // Callbacks
  NavigateCallback on_navigate_;

  // GDI resources (cached)
  HFONT font_tab_ = nullptr;
  HFONT font_toolbar_ = nullptr;
  HFONT font_addr_ = nullptr;
  HFONT font_status_ = nullptr;
  HFONT font_icon_ = nullptr;

  void create_fonts();
  void destroy_fonts();
};
