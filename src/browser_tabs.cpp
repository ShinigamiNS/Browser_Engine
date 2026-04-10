// browser_tabs.cpp — Tab management and tab rect helpers for BrowserUI
// Moved from browser_ui.cpp (Split F)
#include "browser_ui.h"
#include <algorithm>

// ────────────────────────────────────────────────────────────────────────────
//  Tab Rect Helpers
// ────────────────────────────────────────────────────────────────────────────

UIRect BrowserUI::tab_rect(int index) const {
  // Calculate dynamic tab width based on available space
  int available = window_w_ - NEW_TAB_BTN_W - 8;
  int num = (int)tabs_.size();
  int tw =
      (num > 0) ? std::min((int)TAB_WIDTH, available / num) : (int)TAB_WIDTH;
  tw = std::max(tw, (int)TAB_MIN_WIDTH);
  return {index * tw, 0, tw, TAB_BAR_HEIGHT};
}

UIRect BrowserUI::tab_close_rect(int index) const {
  UIRect tr = tab_rect(index);
  int cx = tr.x + tr.w - TAB_CLOSE_SIZE - 8;
  int cy = tr.y + (tr.h - TAB_CLOSE_SIZE) / 2;
  return {cx, cy, TAB_CLOSE_SIZE, TAB_CLOSE_SIZE};
}

UIRect BrowserUI::new_tab_rect() const {
  int num = (int)tabs_.size();
  int available = window_w_ - NEW_TAB_BTN_W - 8;
  int tw =
      (num > 0) ? std::min((int)TAB_WIDTH, available / num) : (int)TAB_WIDTH;
  tw = std::max(tw, (int)TAB_MIN_WIDTH);
  int x = num * tw + 4;
  return {x, (TAB_BAR_HEIGHT - NEW_TAB_BTN_W) / 2, NEW_TAB_BTN_W,
          NEW_TAB_BTN_W};
}

// ────────────────────────────────────────────────────────────────────────────
//  Tab Management
// ────────────────────────────────────────────────────────────────────────────

int BrowserUI::add_tab(const std::string &url, const std::string &title) {
  Tab tab;
  tab.id = next_tab_id_++;
  tab.url = url;
  tab.title = title;
  if (!url.empty()) {
    tab.history.push_back(url);
    tab.history_index = 0;
  }
  tabs_.push_back(tab);
  active_tab_ = (int)tabs_.size() - 1;
  if (!url.empty()) {
    address_text_ = url;
  } else {
    address_text_ = "";
  }
  return tab.id;
}

void BrowserUI::close_tab(int index) {
  if (index < 0 || index >= (int)tabs_.size())
    return;
  tabs_.erase(tabs_.begin() + index);
  if (tabs_.empty()) {
    // Always keep at least one tab
    add_tab("", "New Tab");
    return;
  }
  if (active_tab_ >= (int)tabs_.size()) {
    active_tab_ = (int)tabs_.size() - 1;
  }
  // Sync address bar with new active tab
  Tab *t = active_tab();
  if (t) {
    address_text_ = t->url;
  }
}

void BrowserUI::set_active_tab(int index) {
  if (index < 0 || index >= (int)tabs_.size())
    return;
  active_tab_ = index;
  Tab *t = active_tab();
  if (t) {
    address_text_ = t->url;
  }
}

Tab *BrowserUI::active_tab() {
  if (active_tab_ >= 0 && active_tab_ < (int)tabs_.size())
    return &tabs_[active_tab_];
  return nullptr;
}

const Tab *BrowserUI::active_tab() const {
  if (active_tab_ >= 0 && active_tab_ < (int)tabs_.size())
    return &tabs_[active_tab_];
  return nullptr;
}
