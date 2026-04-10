#pragma once
#include "image_cache.h"
#include "page_loader.h"
#include "paint.h"
#include <cstdint>
#include <vector>
#include <windows.h>

// ── Software pixel buffer globals ─────────────────────────────────────────────
extern void    *buffer_memory;
extern int      buffer_width;
extern int      buffer_height;
extern BITMAPINFO bitmap_info;

// ── Pixel buffer drawing helpers ──────────────────────────────────────────────
void FillRect(int start_x, int start_y, int width, int height, uint32_t color);
void FillRoundedRect(int sx, int sy, int w, int h, int r, uint32_t color);
void FillRectOpacity(int start_x, int start_y, int width, int height,
                     uint32_t color, float opacity);

// ── Find-in-page match rectangle ──────────────────────────────────────────────
struct FindMatch { float x, y, w, h; };

// ── Frame rendering ────────────────────────────────────────────────────────────
// render_frame() performs the full WM_PAINT body, drawing into mem_dc.
// Parameters match the values available in WndProc's WM_PAINT handler.
void render_frame(HWND hwnd, HDC mem_dc, int win_w, int win_h,
                  int scroll_y, bool is_scrolling,
                  const std::string &g_current_page_url_ref);
