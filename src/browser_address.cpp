// browser_address.cpp — Address bar methods for BrowserUI
// Moved from browser_ui.cpp (Split F)
#include "browser_ui.h"
#include <algorithm>

// ────────────────────────────────────────────────────────────────────────────
//  Address Bar
// ────────────────────────────────────────────────────────────────────────────

void BrowserUI::set_address_text(const std::string &text) {
  address_text_ = text;
  cursor_pos_ = (int)text.size();
}

void BrowserUI::focus_address_bar() {
  if (!address_focused_) {
    address_focused_ = true;
    cursor_pos_ = (int)address_text_.size();
    selection_start_ = 0; // Select all on first focus (like a real browser)
  }
}

void BrowserUI::blur_address_bar() {
  address_focused_ = false;
  selection_start_ = -1;
}

void BrowserUI::invalidate_toolbar() {
  if (!hwnd_) return;
  RECT r = {0, 0, window_w_, CHROME_HEIGHT};
  InvalidateRect(hwnd_, &r, FALSE);
}

void BrowserUI::address_click(int x, int /*y*/) {
  bool was_focused = address_focused_;
  address_focused_ = true;

  if (!was_focused) {
    // First click: select all (like real browsers)
    selection_start_ = 0;
    cursor_pos_ = (int)address_text_.size();
    invalidate_toolbar();
    return;
  }

  // Already focused: position cursor at click location
  if (address_text_.empty()) {
    cursor_pos_ = 0;
    selection_start_ = -1;
    invalidate_toolbar();
    return;
  }

  // Use GDI to find character position under click
  HDC hdc = GetDC(hwnd_);
  HGDIOBJ old_font = SelectObject(hdc, font_addr_);
  int text_origin_x = address_rect_.x + 10;
  int click_offset = x - text_origin_x;
  if (click_offset < 0) click_offset = 0;

  int best_pos = 0;
  int best_dist = click_offset; // distance if cursor at position 0
  int len = (int)address_text_.size();
  for (int i = 1; i <= len; i++) {
    SIZE sz;
    GetTextExtentPoint32A(hdc, address_text_.c_str(), i, &sz);
    int dist = std::abs(click_offset - (int)sz.cx);
    if (dist < best_dist) {
      best_dist = dist;
      best_pos = i;
    }
  }

  SelectObject(hdc, old_font);
  ReleaseDC(hwnd_, hdc);

  cursor_pos_ = best_pos;
  selection_start_ = -1;
  invalidate_toolbar();
}
