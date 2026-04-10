#include "browser_ui.h"
#include <algorithm>
#include <cstring>

// ============================================================================
// BrowserUI Implementation
// ============================================================================

BrowserUI::BrowserUI() {}

BrowserUI::~BrowserUI() { destroy_fonts(); }

// ────────────────────────────────────────────────────────────────────────────
//  Initialization & Lifecycle
// ────────────────────────────────────────────────────────────────────────────

void BrowserUI::init(HWND hwnd, int w, int h) {
  hwnd_ = hwnd;
  window_w_ = w;
  window_h_ = h;
  create_fonts();
  recalc_layout();
  // Start with one default tab
  if (tabs_.empty()) {
    add_tab("", "New Tab");
  }
}

void BrowserUI::resize(int w, int h) {
  window_w_ = w;
  window_h_ = h;
  recalc_layout();
}

// ────────────────────────────────────────────────────────────────────────────
//  Font Management
// ────────────────────────────────────────────────────────────────────────────

void BrowserUI::create_fonts() {
  destroy_fonts();

  font_tab_ =
      CreateFontA(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                  OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                  VARIABLE_PITCH, "Segoe UI");

  font_toolbar_ =
      CreateFontA(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                  OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                  VARIABLE_PITCH, "Segoe UI");

  font_addr_ =
      CreateFontA(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                  OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                  VARIABLE_PITCH, "Segoe UI");

  font_status_ =
      CreateFontA(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                  OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                  VARIABLE_PITCH, "Segoe UI");

  font_icon_ =
      CreateFontA(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                  OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                  VARIABLE_PITCH, "Segoe UI Symbol");
}

void BrowserUI::destroy_fonts() {
  if (font_tab_) {
    DeleteObject(font_tab_);
    font_tab_ = nullptr;
  }
  if (font_toolbar_) {
    DeleteObject(font_toolbar_);
    font_toolbar_ = nullptr;
  }
  if (font_addr_) {
    DeleteObject(font_addr_);
    font_addr_ = nullptr;
  }
  if (font_status_) {
    DeleteObject(font_status_);
    font_status_ = nullptr;
  }
  if (font_icon_) {
    DeleteObject(font_icon_);
    font_icon_ = nullptr;
  }
}

// ────────────────────────────────────────────────────────────────────────────
//  Layout Recalculation
// ────────────────────────────────────────────────────────────────────────────

void BrowserUI::recalc_layout() {
  int toolbar_y = TAB_BAR_HEIGHT;
  int btn_y = toolbar_y + (TOOLBAR_HEIGHT - NAV_BTN_SIZE) / 2;
  int x = 8;

  // Nav buttons: Back, Forward, Reload, Home
  back_btn_ = {x, btn_y, NAV_BTN_SIZE, NAV_BTN_SIZE};
  x += NAV_BTN_SIZE + NAV_BTN_SPACING;
  forward_btn_ = {x, btn_y, NAV_BTN_SIZE, NAV_BTN_SIZE};
  x += NAV_BTN_SIZE + NAV_BTN_SPACING;
  reload_btn_ = {x, btn_y, NAV_BTN_SIZE, NAV_BTN_SIZE};
  x += NAV_BTN_SIZE + NAV_BTN_SPACING;
  home_btn_ = {x, btn_y, NAV_BTN_SIZE, NAV_BTN_SIZE};
  x += NAV_BTN_SIZE + 10;

  // Address bar fills middle
  int addr_y = toolbar_y + (TOOLBAR_HEIGHT - ADDR_BAR_HEIGHT) / 2;
  int go_x = window_w_ - GO_BTN_WIDTH - 8;
  int addr_w = go_x - x - 6;
  address_rect_ = {x, addr_y, addr_w, ADDR_BAR_HEIGHT};

  // Go button
  go_btn_ = {go_x, addr_y, GO_BTN_WIDTH, ADDR_BAR_HEIGHT};
}

// tab_rect(), tab_close_rect(), new_tab_rect(), add_tab(), close_tab(),
// set_active_tab(), active_tab() moved to browser_tabs.cpp (Split F)

// set_address_text(), focus_address_bar(), blur_address_bar(),
// invalidate_toolbar(), address_click() moved to browser_address.cpp (Split F)

// ────────────────────────────────────────────────────────────────────────────
//  Hit Testing
// ────────────────────────────────────────────────────────────────────────────

UIHitResult BrowserUI::hit_test(int x, int y, int *out_tab_index) {
  // Tab bar region
  if (y < TAB_BAR_HEIGHT) {
    // New tab button
    UIRect ntr = new_tab_rect();
    if (ntr.contains(x, y))
      return UIHitResult::NewTab;

    // Individual tabs
    for (int i = 0; i < (int)tabs_.size(); ++i) {
      UIRect cr = tab_close_rect(i);
      if (cr.contains(x, y)) {
        if (out_tab_index)
          *out_tab_index = i;
        return UIHitResult::TabClose;
      }
      UIRect tr = tab_rect(i);
      if (tr.contains(x, y)) {
        if (out_tab_index)
          *out_tab_index = i;
        return UIHitResult::TabItem;
      }
    }
    return UIHitResult::TabBar;
  }

  // Toolbar region
  if (y < CHROME_HEIGHT) {
    if (back_btn_.contains(x, y))
      return UIHitResult::BackButton;
    if (forward_btn_.contains(x, y))
      return UIHitResult::ForwardButton;
    if (reload_btn_.contains(x, y))
      return UIHitResult::ReloadButton;
    if (home_btn_.contains(x, y))
      return UIHitResult::HomeButton;
    if (go_btn_.contains(x, y))
      return UIHitResult::GoButton;
    if (address_rect_.contains(x, y))
      return UIHitResult::AddressBar;
    return UIHitResult::None;
  }

  // Status bar
  if (y >= window_h_ - STATUS_BAR_HEIGHT) {
    return UIHitResult::StatusBar;
  }

  return UIHitResult::ContentArea;
}

// ────────────────────────────────────────────────────────────────────────────
//  Mouse Event Handlers
// ────────────────────────────────────────────────────────────────────────────

void BrowserUI::on_mouse_move(int x, int y) {
  int tab_idx = -1;
  UIHitResult hr = hit_test(x, y, &tab_idx);
  bool changed = (hr != hover_target_) || (tab_idx != hover_tab_index_);
  hover_target_ = hr;
  hover_tab_index_ = tab_idx;
  if (changed && hwnd_) {
    // Repaint chrome areas on hover change for visual feedback
    RECT r;
    r.left = 0;
    r.top = 0;
    r.right = window_w_;
    r.bottom = CHROME_HEIGHT;
    InvalidateRect(hwnd_, &r, FALSE);
  }
}

void BrowserUI::on_mouse_down(int x, int y) {
  int tab_idx = -1;
  UIHitResult hr = hit_test(x, y, &tab_idx);

  switch (hr) {
  case UIHitResult::TabItem:
    if (tab_idx >= 0) {
      set_active_tab(tab_idx);
      blur_address_bar();
      if (on_navigate_ && active_tab() && !active_tab()->url.empty()) {
        on_navigate_(active_tab()->url);
      }
    }
    break;

  case UIHitResult::TabClose:
    if (tab_idx >= 0) {
      close_tab(tab_idx);
      if (on_navigate_ && active_tab() && !active_tab()->url.empty()) {
        on_navigate_(active_tab()->url);
      }
    }
    break;

  case UIHitResult::NewTab:
    add_tab("", "New Tab");
    focus_address_bar();
    break;

  case UIHitResult::BackButton:
    if (active_tab() && active_tab()->can_go_back()) {
      std::string url = active_tab()->go_back();
      address_text_ = url;
      if (on_navigate_)
        on_navigate_(url);
    }
    break;

  case UIHitResult::ForwardButton:
    if (active_tab() && active_tab()->can_go_forward()) {
      std::string url = active_tab()->go_forward();
      address_text_ = url;
      if (on_navigate_)
        on_navigate_(url);
    }
    break;

  case UIHitResult::ReloadButton:
    if (active_tab() && !active_tab()->url.empty()) {
      if (on_navigate_)
        on_navigate_(active_tab()->url);
    }
    break;

  case UIHitResult::HomeButton:
    if (active_tab()) {
      active_tab()->push_url("");
      address_text_ = "";
      if (on_navigate_)
        on_navigate_("");
    }
    break;

  case UIHitResult::AddressBar:
    address_click(x, y);
    break;

  case UIHitResult::GoButton:
    blur_address_bar();
    if (!address_text_.empty() && active_tab()) {
      active_tab()->push_url(address_text_);
      active_tab()->title = address_text_;
      if (on_navigate_)
        on_navigate_(address_text_);
    }
    break;

  default:
    blur_address_bar();
    break;
  }

  if (hwnd_)
    InvalidateRect(hwnd_, NULL, FALSE);
}

void BrowserUI::on_mouse_up(int /*x*/, int /*y*/) {
  // Currently no drag operations
}

// ────────────────────────────────────────────────────────────────────────────
//  Keyboard Event Handlers
// ────────────────────────────────────────────────────────────────────────────

void BrowserUI::on_char(char ch) {
  if (!address_focused_)
    return;

  bool has_selection = selection_start_ >= 0 && selection_start_ != cursor_pos_;

  if (ch == '\b') {
    // Backspace — delete selection or one char before cursor
    if (has_selection) {
      int sel_lo = std::min(selection_start_, cursor_pos_);
      int sel_hi = std::max(selection_start_, cursor_pos_);
      address_text_.erase(sel_lo, sel_hi - sel_lo);
      cursor_pos_ = sel_lo;
      selection_start_ = -1;
    } else if (cursor_pos_ > 0 && !address_text_.empty()) {
      address_text_.erase(cursor_pos_ - 1, 1);
      cursor_pos_--;
    }
  } else if (ch == '\r' || ch == '\n') {
    // Enter — navigate
    blur_address_bar();
    if (!address_text_.empty() && active_tab()) {
      active_tab()->push_url(address_text_);
      active_tab()->title = address_text_;
      if (on_navigate_)
        on_navigate_(address_text_);
    }
    if (hwnd_) InvalidateRect(hwnd_, NULL, FALSE);
    return;
  } else if (ch == 27) {
    // Escape
    blur_address_bar();
    if (active_tab())
      address_text_ = active_tab()->url;
  } else if (ch >= 32) {
    // Printable character — delete selection first, then insert
    if (has_selection) {
      int sel_lo = std::min(selection_start_, cursor_pos_);
      int sel_hi = std::max(selection_start_, cursor_pos_);
      address_text_.erase(sel_lo, sel_hi - sel_lo);
      cursor_pos_ = sel_lo;
      selection_start_ = -1;
    }
    address_text_.insert(address_text_.begin() + cursor_pos_, ch);
    cursor_pos_++;
  }

  invalidate_toolbar();
}

void BrowserUI::on_key_down(int vk) {
  if (!address_focused_)
    return;

  bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
  bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
  bool has_selection = selection_start_ >= 0 && selection_start_ != cursor_pos_;
  int len = (int)address_text_.size();

  // Helper: delete current selection, returns true if there was one
  auto delete_selection = [&]() -> bool {
    if (!has_selection) return false;
    int sel_lo = std::min(selection_start_, cursor_pos_);
    int sel_hi = std::max(selection_start_, cursor_pos_);
    address_text_.erase(sel_lo, sel_hi - sel_lo);
    cursor_pos_ = sel_lo;
    selection_start_ = -1;
    has_selection = false;
    return true;
  };

  // Helper: get selected text
  auto get_selected_text = [&]() -> std::string {
    if (!has_selection) return "";
    int sel_lo = std::min(selection_start_, cursor_pos_);
    int sel_hi = std::max(selection_start_, cursor_pos_);
    return address_text_.substr(sel_lo, sel_hi - sel_lo);
  };

  // Helper: copy text to clipboard
  auto copy_to_clipboard = [&](const std::string &text) {
    if (text.empty() || !hwnd_) return;
    if (OpenClipboard(hwnd_)) {
      EmptyClipboard();
      HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
      if (hMem) {
        char *p = static_cast<char*>(GlobalLock(hMem));
        memcpy(p, text.c_str(), text.size() + 1);
        GlobalUnlock(hMem);
        SetClipboardData(CF_TEXT, hMem);
      }
      CloseClipboard();
    }
  };

  // ── Arrow keys with Shift selection ──
  if (vk == VK_LEFT) {
    if (shift) {
      if (selection_start_ < 0) selection_start_ = cursor_pos_;
      if (ctrl) {
        // Ctrl+Shift+Left: move to previous word boundary
        int p = cursor_pos_;
        while (p > 0 && address_text_[p-1] == ' ') p--;
        while (p > 0 && address_text_[p-1] != ' ') p--;
        cursor_pos_ = p;
      } else if (cursor_pos_ > 0) {
        cursor_pos_--;
      }
    } else {
      if (has_selection) {
        cursor_pos_ = std::min(selection_start_, cursor_pos_);
        selection_start_ = -1;
      } else if (ctrl) {
        int p = cursor_pos_;
        while (p > 0 && address_text_[p-1] == ' ') p--;
        while (p > 0 && address_text_[p-1] != ' ') p--;
        cursor_pos_ = p;
      } else if (cursor_pos_ > 0) {
        cursor_pos_--;
      }
      selection_start_ = -1;
    }
  } else if (vk == VK_RIGHT) {
    if (shift) {
      if (selection_start_ < 0) selection_start_ = cursor_pos_;
      if (ctrl) {
        int p = cursor_pos_;
        while (p < len && address_text_[p] != ' ') p++;
        while (p < len && address_text_[p] == ' ') p++;
        cursor_pos_ = p;
      } else if (cursor_pos_ < len) {
        cursor_pos_++;
      }
    } else {
      if (has_selection) {
        cursor_pos_ = std::max(selection_start_, cursor_pos_);
        selection_start_ = -1;
      } else if (ctrl) {
        int p = cursor_pos_;
        while (p < len && address_text_[p] != ' ') p++;
        while (p < len && address_text_[p] == ' ') p++;
        cursor_pos_ = p;
      } else if (cursor_pos_ < len) {
        cursor_pos_++;
      }
      selection_start_ = -1;
    }
  } else if (vk == VK_HOME) {
    if (shift) {
      if (selection_start_ < 0) selection_start_ = cursor_pos_;
    } else {
      selection_start_ = -1;
    }
    cursor_pos_ = 0;
  } else if (vk == VK_END) {
    if (shift) {
      if (selection_start_ < 0) selection_start_ = cursor_pos_;
    } else {
      selection_start_ = -1;
    }
    cursor_pos_ = len;
  } else if (vk == VK_DELETE) {
    if (!delete_selection() && cursor_pos_ < len) {
      address_text_.erase(cursor_pos_, 1);
    }
  } else if (ctrl && vk == 'A') {
    selection_start_ = 0;
    cursor_pos_ = len;
  } else if (ctrl && vk == 'C') {
    copy_to_clipboard(get_selected_text());
  } else if (ctrl && vk == 'X') {
    std::string sel = get_selected_text();
    if (!sel.empty()) {
      copy_to_clipboard(sel);
      delete_selection();
    }
  } else if (ctrl && vk == 'V') {
    if (OpenClipboard(hwnd_)) {
      HANDLE hData = GetClipboardData(CF_TEXT);
      if (hData) {
        char *pszText = static_cast<char *>(GlobalLock(hData));
        if (pszText) {
          std::string paste_text(pszText);
          // Strip newlines from pasted text
          paste_text.erase(std::remove(paste_text.begin(), paste_text.end(), '\n'), paste_text.end());
          paste_text.erase(std::remove(paste_text.begin(), paste_text.end(), '\r'), paste_text.end());
          delete_selection();
          address_text_.insert(cursor_pos_, paste_text);
          cursor_pos_ += (int)paste_text.size();
          GlobalUnlock(hData);
        }
      }
      CloseClipboard();
    }
  }

  invalidate_toolbar();
}

// ────────────────────────────────────────────────────────────────────────────
//  Painting — Master Paint Function
// ────────────────────────────────────────────────────────────────────────────

void BrowserUI::paint(HDC hdc) {
  paint_tab_bar(hdc);
  paint_toolbar(hdc);
  paint_status_bar(hdc);
}

// ────────────────────── Tab Bar ──────────────────────

void BrowserUI::paint_tab_bar(HDC hdc) {
  // Background
  RECT bg = {0, 0, window_w_, TAB_BAR_HEIGHT};
  HBRUSH bg_brush = CreateSolidBrush(colors_.tab_bar_bg);
  FillRect(hdc, &bg, bg_brush);
  DeleteObject(bg_brush);

  SetBkMode(hdc, TRANSPARENT);
  HGDIOBJ old_font = SelectObject(hdc, font_tab_);

  for (int i = 0; i < (int)tabs_.size(); ++i) {
    UIRect tr = tab_rect(i);
    bool is_active = (i == active_tab_);
    bool is_hovered =
        (hover_target_ == UIHitResult::TabItem && hover_tab_index_ == i);
    bool close_hovered =
        (hover_target_ == UIHitResult::TabClose && hover_tab_index_ == i);

    // Tab background
    COLORREF tab_bg;
    if (is_active)
      tab_bg = colors_.tab_active_bg;
    else if (is_hovered)
      tab_bg = colors_.tab_hover_bg;
    else
      tab_bg = colors_.tab_inactive_bg;

    RECT tab_r = {tr.x, tr.y, tr.x + tr.w, tr.y + tr.h};

    // For active tab, draw a rounded-top effect by filling slightly differently
    if (is_active) {
      // Active tab: fill entire rect, then draw a small accent line at top
      HBRUSH tb = CreateSolidBrush(tab_bg);
      FillRect(hdc, &tab_r, tb);
      DeleteObject(tb);

      // Blue accent line at the top of the active tab
      RECT accent = {tr.x + 4, tr.y, tr.x + tr.w - 4, tr.y + 3};
      HBRUSH accent_br = CreateSolidBrush(RGB(80, 130, 255));
      FillRect(hdc, &accent, accent_br);
      DeleteObject(accent_br);
    } else {
      HBRUSH tb = CreateSolidBrush(tab_bg);
      FillRect(hdc, &tab_r, tb);
      DeleteObject(tb);
    }

    // Tab title text
    COLORREF text_col =
        is_active ? colors_.tab_text_active : colors_.tab_text_inactive;
    SetTextColor(hdc, text_col);

    // Truncate title to fit (leave room for close button)
    std::string title = tabs_[i].title;
    int max_text_w = tr.w - TAB_CLOSE_SIZE - 24;
    if (max_text_w < 20)
      max_text_w = 20;

    // Measure and truncate
    SIZE sz;
    GetTextExtentPoint32A(hdc, title.c_str(), (int)title.size(), &sz);
    while (sz.cx > max_text_w && title.size() > 3) {
      title.pop_back();
      title.back() = '.';
      title += "..";
      if (title.size() > 3)
        title.resize(title.size() - 2);
      // Actually, just simple truncation with ellipsis
      break;
    }
    if (sz.cx > max_text_w && title.size() > 6) {
      int chars = (int)(title.size() * max_text_w / sz.cx);
      if (chars < (int)title.size() && chars > 3) {
        title = title.substr(0, chars - 3) + "...";
      }
    }

    RECT text_r = {tr.x + 12, tr.y + 2, tr.x + tr.w - TAB_CLOSE_SIZE - 10,
                   tr.y + tr.h};
    DrawTextA(hdc, title.c_str(), (int)title.size(), &text_r,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                  DT_NOPREFIX);

    // Close button "×"
    UIRect cr = tab_close_rect(i);
    if (close_hovered) {
      RECT close_bg_r = {cr.x - 2, cr.y - 2, cr.x + cr.w + 2, cr.y + cr.h + 2};
      HBRUSH close_bg_br = CreateSolidBrush(colors_.tab_close_hover);
      FillRect(hdc, &close_bg_r, close_bg_br);
      DeleteObject(close_bg_br);
      SetTextColor(hdc, RGB(255, 255, 255));
    } else {
      SetTextColor(hdc, colors_.tab_close_fg);
    }
    RECT close_r = {cr.x, cr.y, cr.x + cr.w, cr.y + cr.h};
    DrawTextA(hdc, "x", 1, &close_r,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  }

  // New tab "+" button
  UIRect ntr = new_tab_rect();
  bool nt_hovered = (hover_target_ == UIHitResult::NewTab);
  if (nt_hovered) {
    RECT nt_bg = {ntr.x, ntr.y, ntr.x + ntr.w, ntr.y + ntr.h};
    HBRUSH nt_br = CreateSolidBrush(colors_.tab_hover_bg);
    FillRect(hdc, &nt_bg, nt_br);
    DeleteObject(nt_br);
  }
  SetTextColor(hdc, colors_.tab_text_inactive);
  RECT nt_r = {ntr.x, ntr.y, ntr.x + ntr.w, ntr.y + ntr.h};
  DrawTextA(hdc, "+", 1, &nt_r,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

  // Bottom separator line
  RECT sep = {0, TAB_BAR_HEIGHT - 1, window_w_, TAB_BAR_HEIGHT};
  HBRUSH sep_br = CreateSolidBrush(colors_.content_border);
  FillRect(hdc, &sep, sep_br);
  DeleteObject(sep_br);

  SelectObject(hdc, old_font);
}

// ────────────────────── Toolbar ──────────────────────

void BrowserUI::paint_toolbar(HDC hdc) {
  int toolbar_y = TAB_BAR_HEIGHT;

  // Background
  RECT bg = {0, toolbar_y, window_w_, toolbar_y + TOOLBAR_HEIGHT};
  HBRUSH bg_brush = CreateSolidBrush(colors_.toolbar_bg);
  FillRect(hdc, &bg, bg_brush);
  DeleteObject(bg_brush);

  SetBkMode(hdc, TRANSPARENT);

  // Navigation buttons
  Tab *tab = active_tab();
  bool can_back = tab && tab->can_go_back();
  bool can_fwd = tab && tab->can_go_forward();

  draw_nav_button(hdc, back_btn_, L"\x2190", can_back,
                  hover_target_ == UIHitResult::BackButton); // ←
  draw_nav_button(hdc, forward_btn_, L"\x2192", can_fwd,
                  hover_target_ == UIHitResult::ForwardButton); // →
  draw_nav_button(hdc, reload_btn_, is_loading_ ? L"\x2715" : L"\x21BB", true,
                  hover_target_ == UIHitResult::ReloadButton); // ↻ or ✕
  draw_nav_button(hdc, home_btn_, L"\x2302", true,
                  hover_target_ == UIHitResult::HomeButton); // ⌂

  // ── Address Bar ──
  bool addr_hover = (hover_target_ == UIHitResult::AddressBar);
  COLORREF border_col =
      address_focused_ ? colors_.addr_border_focus
                       : (addr_hover ? RGB(85, 88, 98) : colors_.addr_border);

  // Draw border (slightly larger rect)
  RECT addr_border_r = {address_rect_.x - 1, address_rect_.y - 1,
                        address_rect_.x + address_rect_.w + 1,
                        address_rect_.y + address_rect_.h + 1};
  HBRUSH border_br = CreateSolidBrush(border_col);
  FillRect(hdc, &addr_border_r, border_br);
  DeleteObject(border_br);

  // Draw background
  RECT addr_bg_r = {address_rect_.x, address_rect_.y,
                    address_rect_.x + address_rect_.w,
                    address_rect_.y + address_rect_.h};
  HBRUSH addr_bg_br = CreateSolidBrush(colors_.addr_bg);
  FillRect(hdc, &addr_bg_r, addr_bg_br);
  DeleteObject(addr_bg_br);

  // Draw address text
  HGDIOBJ old_font = SelectObject(hdc, font_addr_);
  RECT text_clip = {address_rect_.x + 10, address_rect_.y + 2,
                    address_rect_.x + address_rect_.w - 8,
                    address_rect_.y + address_rect_.h - 2};

  if (address_text_.empty() && !address_focused_) {
    // Placeholder
    SetTextColor(hdc, colors_.addr_placeholder);
    DrawTextA(hdc, "Search or enter URL...", -1, &text_clip,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  } else {
    // Draw selection highlight if applicable
    if (address_focused_ && selection_start_ >= 0 &&
        selection_start_ != cursor_pos_) {
      int sel_lo = std::min(selection_start_, cursor_pos_);
      int sel_hi = std::max(selection_start_, cursor_pos_);
      // Measure text up to selection start and end
      SIZE sz_lo, sz_hi;
      GetTextExtentPoint32A(hdc, address_text_.c_str(), sel_lo, &sz_lo);
      GetTextExtentPoint32A(hdc, address_text_.c_str(), sel_hi, &sz_hi);
      RECT sel_r = {address_rect_.x + 10 + (int)sz_lo.cx, address_rect_.y + 4,
                    address_rect_.x + 10 + (int)sz_hi.cx,
                    address_rect_.y + address_rect_.h - 4};
      HBRUSH sel_br = CreateSolidBrush(colors_.addr_selection);
      FillRect(hdc, &sel_r, sel_br);
      DeleteObject(sel_br);
    }

    SetTextColor(hdc, colors_.addr_text);
    DrawTextA(hdc, address_text_.c_str(), (int)address_text_.size(), &text_clip,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    // Draw cursor (blinking could be added with a timer)
    if (address_focused_) {
      SIZE sz_cursor;
      GetTextExtentPoint32A(hdc, address_text_.c_str(), cursor_pos_,
                            &sz_cursor);
      int cx = address_rect_.x + 10 + sz_cursor.cx;
      int cy_top = address_rect_.y + 5;
      int cy_bot = address_rect_.y + address_rect_.h - 5;
      HPEN cursor_pen = CreatePen(PS_SOLID, 1, colors_.addr_text);
      HGDIOBJ old_pen = SelectObject(hdc, cursor_pen);
      MoveToEx(hdc, cx, cy_top, NULL);
      LineTo(hdc, cx, cy_bot);
      SelectObject(hdc, old_pen);
      DeleteObject(cursor_pen);
    }
  }
  SelectObject(hdc, old_font);

  // ── Go Button ──
  bool go_hover = (hover_target_ == UIHitResult::GoButton);
  COLORREF go_bg = go_hover ? colors_.go_bg_hover : colors_.go_bg;

  // Rounded-ish Go button
  RECT go_r = {go_btn_.x, go_btn_.y, go_btn_.x + go_btn_.w,
               go_btn_.y + go_btn_.h};
  HBRUSH go_br = CreateSolidBrush(go_bg);
  FillRect(hdc, &go_r, go_br);
  DeleteObject(go_br);

  old_font = SelectObject(hdc, font_toolbar_);
  SetTextColor(hdc, colors_.go_text);
  DrawTextA(hdc, "Go", 2, &go_r,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  SelectObject(hdc, old_font);

  // Bottom border line
  RECT tb_sep = {0, toolbar_y + TOOLBAR_HEIGHT - 1, window_w_,
                 toolbar_y + TOOLBAR_HEIGHT};
  HBRUSH tb_sep_br = CreateSolidBrush(colors_.content_border);
  FillRect(hdc, &tb_sep, tb_sep_br);
  DeleteObject(tb_sep_br);
}

// ────────────────────── Nav Button Helper ──────────────────────

void BrowserUI::draw_nav_button(HDC hdc, const UIRect &r, const wchar_t *symbol,
                                bool enabled, bool hovered) {
  if (hovered && enabled) {
    // Draw circular hover background
    HBRUSH hb = CreateSolidBrush(colors_.btn_hover_bg);
    HBRUSH old_br = (HBRUSH)SelectObject(hdc, hb);
    HPEN null_pen = CreatePen(PS_NULL, 0, 0);
    HGDIOBJ old_pen = SelectObject(hdc, null_pen);
    Ellipse(hdc, r.x, r.y, r.x + r.w, r.y + r.h);
    SelectObject(hdc, old_br);
    SelectObject(hdc, old_pen);
    DeleteObject(hb);
    DeleteObject(null_pen);
  }

  COLORREF fg = enabled ? colors_.btn_fg : colors_.btn_disabled_fg;
  SetTextColor(hdc, fg);

  HGDIOBJ old_font = SelectObject(hdc, font_icon_);
  RECT text_r = {r.x, r.y, r.x + r.w, r.y + r.h};
  DrawTextW(hdc, symbol, -1, &text_r,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  SelectObject(hdc, old_font);
}

// ────────────────────── Rounded Rect Helper ──────────────────────

void BrowserUI::draw_rounded_rect(HDC hdc, UIRect r, int radius, COLORREF fill,
                                  COLORREF border) {
  HBRUSH br = CreateSolidBrush(fill);
  HPEN pen = CreatePen(PS_SOLID, 1, border);
  HGDIOBJ old_br = SelectObject(hdc, br);
  HGDIOBJ old_pen = SelectObject(hdc, pen);
  RoundRect(hdc, r.x, r.y, r.x + r.w, r.y + r.h, radius, radius);
  SelectObject(hdc, old_br);
  SelectObject(hdc, old_pen);
  DeleteObject(br);
  DeleteObject(pen);
}

// ────────────────────── Status Bar ──────────────────────

void BrowserUI::paint_status_bar(HDC hdc) {
  int sy = window_h_ - STATUS_BAR_HEIGHT;

  // Top separator
  RECT sep = {0, sy, window_w_, sy + 1};
  HBRUSH sep_br = CreateSolidBrush(colors_.content_border);
  FillRect(hdc, &sep, sep_br);
  DeleteObject(sep_br);

  // Background
  RECT bg = {0, sy + 1, window_w_, window_h_};
  HBRUSH bg_br = CreateSolidBrush(colors_.status_bg);
  FillRect(hdc, &bg, bg_br);
  DeleteObject(bg_br);

  // Status text
  HGDIOBJ old_font = SelectObject(hdc, font_status_);
  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, colors_.status_text);
  RECT text_r = {10, sy + 2, window_w_ - 10, window_h_};
  DrawTextA(hdc, status_text_.c_str(), (int)status_text_.size(), &text_r,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                DT_NOPREFIX);
  SelectObject(hdc, old_font);
}
