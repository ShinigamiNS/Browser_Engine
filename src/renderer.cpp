#include "renderer.h"
#include "browser_ui.h"
#include "page_loader.h"
#include "paint.h"
#include <algorithm>
#include <iostream>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <string>
#include <vector>
#include <windows.h>

// ── Globals defined here ───────────────────────────────────────────────────────
void    *buffer_memory = nullptr;
int      buffer_width  = 800;
int      buffer_height = 600;
BITMAPINFO bitmap_info;

// ── Globals declared elsewhere ─────────────────────────────────────────────────
extern BrowserUI browser_ui;
extern bool app_initialized;

// ── Pixel buffer helpers ───────────────────────────────────────────────────────

void FillRect(int start_x, int start_y, int width, int height, uint32_t color) {
  if (!buffer_memory)
    return;
  uint32_t *pixel = (uint32_t *)buffer_memory;
  for (int y = start_y; y < start_y + height && y < buffer_height; ++y) {
    if (y < 0) continue;
    for (int x = start_x; x < start_x + width && x < buffer_width; ++x) {
      if (x < 0) continue;
      pixel[y * buffer_width + x] = color;
    }
  }
}

void FillRoundedRect(int sx, int sy, int w, int h, int r, uint32_t color) {
  if (!buffer_memory || w <= 0 || h <= 0) return;
  if (r <= 0) { FillRect(sx, sy, w, h, color); return; }
  if (r > w / 2) r = w / 2;
  if (r > h / 2) r = h / 2;
  uint32_t *pixel = (uint32_t *)buffer_memory;
  int cx_l = sx + r;
  int cx_r = sx + w - 1 - r;
  int cy_t = sy + r;
  int cy_b = sy + h - 1 - r;
  for (int y = sy; y < sy + h && y < buffer_height; ++y) {
    if (y < 0) continue;
    for (int x = sx; x < sx + w && x < buffer_width; ++x) {
      if (x < 0) continue;
      bool inside = true;
      if (x < cx_l && y < cy_t) {
        int dx = x - cx_l, dy = y - cy_t;
        inside = (dx*dx + dy*dy) <= r*r;
      } else if (x > cx_r && y < cy_t) {
        int dx = x - cx_r, dy = y - cy_t;
        inside = (dx*dx + dy*dy) <= r*r;
      } else if (x < cx_l && y > cy_b) {
        int dx = x - cx_l, dy = y - cy_b;
        inside = (dx*dx + dy*dy) <= r*r;
      } else if (x > cx_r && y > cy_b) {
        int dx = x - cx_r, dy = y - cy_b;
        inside = (dx*dx + dy*dy) <= r*r;
      }
      if (inside)
        pixel[y * buffer_width + x] = color;
    }
  }
}

static inline uint32_t blend_pixel(uint32_t dst, uint32_t src, float opacity) {
  int a = (int)(opacity * 255.f + 0.5f);
  if (a <= 0)   return dst;
  if (a >= 255) return src;
  uint8_t sr = (src >> 16) & 0xFF;
  uint8_t sg = (src >>  8) & 0xFF;
  uint8_t sb = (src      ) & 0xFF;
  uint8_t dr = (dst >> 16) & 0xFF;
  uint8_t dg = (dst >>  8) & 0xFF;
  uint8_t db = (dst      ) & 0xFF;
  uint8_t r = (uint8_t)((sr * a + dr * (255 - a)) / 255);
  uint8_t g = (uint8_t)((sg * a + dg * (255 - a)) / 255);
  uint8_t b = (uint8_t)((sb * a + db * (255 - a)) / 255);
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

void FillRectOpacity(int start_x, int start_y, int width, int height,
                     uint32_t color, float opacity) {
  if (!buffer_memory || opacity < 0.01f) return;
  if (opacity >= 0.99f) { FillRect(start_x, start_y, width, height, color); return; }
  uint32_t *pixel = (uint32_t *)buffer_memory;
  for (int y = start_y; y < start_y + height && y < buffer_height; ++y) {
    if (y < 0) continue;
    for (int x = start_x; x < start_x + width && x < buffer_width; ++x) {
      if (x < 0) continue;
      int idx = y * buffer_width + x;
      pixel[idx] = blend_pixel(pixel[idx], color, opacity);
    }
  }
}

static void FillLinearGradient(int x0, int y0, int w, int h,
                               const std::vector<GradientStop> &stops,
                               float angle_deg, float opacity = 1.0f) {
  if (!buffer_memory || w <= 0 || h <= 0 || stops.size() < 2) return;
  const float PI = 3.14159265f;
  float angle_rad = angle_deg * PI / 180.f;
  float sa = sinf(angle_rad), ca = cosf(angle_rad);
  float half_w = w * 0.5f, half_h = h * 0.5f;
  float L = (float)w * fabsf(sa) + (float)h * fabsf(ca);
  if (L < 1e-6f) L = 1.f;

  auto interp = [&](float t) -> uint32_t {
    if (t <= stops.front().pos) return stops.front().color;
    if (t >= stops.back().pos)  return stops.back().color;
    for (int i = 0; i + 1 < (int)stops.size(); i++) {
      if (t <= stops[i + 1].pos) {
        float seg = stops[i + 1].pos - stops[i].pos;
        float f = (seg > 1e-6f) ? (t - stops[i].pos) / seg : 0.f;
        uint32_t c0 = stops[i].color, c1 = stops[i + 1].color;
        int r = (int)(((c0 >> 16) & 0xFF) * (1.f - f) + ((c1 >> 16) & 0xFF) * f + 0.5f);
        int g = (int)(((c0 >>  8) & 0xFF) * (1.f - f) + ((c1 >>  8) & 0xFF) * f + 0.5f);
        int b = (int)(( c0        & 0xFF) * (1.f - f) + ( c1        & 0xFF) * f + 0.5f);
        return ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF);
      }
    }
    return stops.back().color;
  };

  uint32_t *pixel = (uint32_t *)buffer_memory;
  for (int py = 0; py < h; py++) {
    int gy = y0 + py;
    if (gy < 0 || gy >= buffer_height) continue;
    float dy = (float)py - half_h;
    for (int px = 0; px < w; px++) {
      int gx = x0 + px;
      if (gx < 0 || gx >= buffer_width) continue;
      float dx = (float)px - half_w;
      float proj = dx * sa - dy * ca;
      float t = proj / L + 0.5f;
      uint32_t c = interp(t);
      int idx = gy * buffer_width + gx;
      pixel[idx] = (opacity < 0.99f) ? blend_pixel(pixel[idx], c, opacity) : c;
    }
  }
}

static void FillRadialGradient(int x0, int y0, int w, int h,
                               const std::vector<GradientStop> &stops,
                               bool ellipse, float opacity = 1.0f) {
  if (!buffer_memory || w <= 0 || h <= 0 || stops.size() < 2) return;
  float cx = x0 + w * 0.5f;
  float cy = y0 + h * 0.5f;
  float rx = w * 0.5f, ry = ellipse ? h * 0.5f : rx;
  if (rx < 1.f) rx = 1.f;
  if (ry < 1.f) ry = 1.f;

  auto interp = [&](float t) -> uint32_t {
    if (t <= stops.front().pos) return stops.front().color;
    if (t >= stops.back().pos)  return stops.back().color;
    for (int i = 0; i + 1 < (int)stops.size(); i++) {
      if (t <= stops[i+1].pos) {
        float seg = stops[i+1].pos - stops[i].pos;
        float f = (seg > 1e-6f) ? (t - stops[i].pos) / seg : 0.f;
        uint32_t c0 = stops[i].color, c1 = stops[i+1].color;
        int r = (int)(((c0>>16)&0xFF)*(1.f-f)+((c1>>16)&0xFF)*f+.5f);
        int g = (int)(((c0>> 8)&0xFF)*(1.f-f)+((c1>> 8)&0xFF)*f+.5f);
        int b = (int)(( c0     &0xFF)*(1.f-f)+( c1     &0xFF)*f+.5f);
        return ((r&0xFF)<<16)|((g&0xFF)<<8)|(b&0xFF);
      }
    }
    return stops.back().color;
  };

  uint32_t *pixel = (uint32_t *)buffer_memory;
  for (int py = y0; py < y0 + h && py < buffer_height; py++) {
    if (py < 0) continue;
    float dy = (py - cy) / ry;
    for (int px = x0; px < x0 + w && px < buffer_width; px++) {
      if (px < 0) continue;
      float dx = (px - cx) / rx;
      float t = sqrtf(dx*dx + dy*dy);
      if (t > 1.f) t = 1.f;
      uint32_t c = interp(t);
      int idx = py * buffer_width + px;
      pixel[idx] = (opacity < 0.99f) ? blend_pixel(pixel[idx], c, opacity) : c;
    }
  }
}

static void FillConicGradient(int x0, int y0, int w, int h,
                              const std::vector<GradientStop> &stops,
                              float from_deg, float opacity = 1.0f) {
  if (!buffer_memory || w <= 0 || h <= 0 || stops.size() < 2) return;
  const float PI = 3.14159265f;
  float cx = x0 + w * 0.5f;
  float cy = y0 + h * 0.5f;
  float from_rad = from_deg * PI / 180.f;

  auto interp = [&](float t) -> uint32_t {
    if (t < 0.f) t += 1.f;
    if (t > 1.f) t -= 1.f;
    if (t <= stops.front().pos) return stops.front().color;
    if (t >= stops.back().pos)  return stops.back().color;
    for (int i = 0; i + 1 < (int)stops.size(); i++) {
      if (t <= stops[i+1].pos) {
        float seg = stops[i+1].pos - stops[i].pos;
        float f = (seg > 1e-6f) ? (t - stops[i].pos) / seg : 0.f;
        uint32_t c0 = stops[i].color, c1 = stops[i+1].color;
        int r = (int)(((c0>>16)&0xFF)*(1.f-f)+((c1>>16)&0xFF)*f+.5f);
        int g = (int)(((c0>> 8)&0xFF)*(1.f-f)+((c1>> 8)&0xFF)*f+.5f);
        int b = (int)(( c0     &0xFF)*(1.f-f)+( c1     &0xFF)*f+.5f);
        return ((r&0xFF)<<16)|((g&0xFF)<<8)|(b&0xFF);
      }
    }
    return stops.back().color;
  };

  uint32_t *pixel = (uint32_t *)buffer_memory;
  for (int py = y0; py < y0 + h && py < buffer_height; py++) {
    if (py < 0) continue;
    for (int px = x0; px < x0 + w && px < buffer_width; px++) {
      if (px < 0) continue;
      float angle = atan2f(py - cy, px - cx) - from_rad + PI * 0.5f;
      float t = angle / (2.f * PI);
      t = t - floorf(t);
      uint32_t c = interp(t);
      int idx = py * buffer_width + px;
      pixel[idx] = (opacity < 0.99f) ? blend_pixel(pixel[idx], c, opacity) : c;
    }
  }
}

static void ApplyFilter(int x0, int y0, int w, int h,
                        float blur, float brightness, float contrast,
                        float grayscale, float sepia) {
  if (!buffer_memory || w <= 0 || h <= 0) return;
  uint32_t *pixel = (uint32_t *)buffer_memory;

  int x1 = std::max(0, x0), y1 = std::max(0, y0);
  int x2 = std::min(buffer_width,  x0 + w);
  int y2 = std::min(buffer_height, y0 + h);
  if (x2 <= x1 || y2 <= y1) return;
  int rw = x2 - x1, rh = y2 - y1;

  if (blur > 0.5f) {
    int r = (int)(blur + 0.5f);
    if (r < 1) r = 1;
    if (r > 32) r = 32;
    std::vector<uint32_t> tmp(rw * rh);
    for (int ry = 0; ry < rh; ry++) {
      int gy = y1 + ry;
      for (int rx2 = 0; rx2 < rw; rx2++) {
        int gx = x1 + rx2;
        int sr = 0, sg = 0, sb = 0, cnt = 0;
        for (int k = -r; k <= r; k++) {
          int nx = gx + k;
          if (nx < x1 || nx >= x2) continue;
          uint32_t p = pixel[gy * buffer_width + nx];
          sr += (p>>16)&0xFF; sg += (p>>8)&0xFF; sb += p&0xFF; cnt++;
        }
        if (cnt) tmp[ry * rw + rx2] = ((sr/cnt)<<16)|((sg/cnt)<<8)|(sb/cnt);
      }
    }
    for (int rx2 = 0; rx2 < rw; rx2++) {
      int gx = x1 + rx2;
      for (int ry = 0; ry < rh; ry++) {
        int gy = y1 + ry;
        int sr = 0, sg = 0, sb = 0, cnt = 0;
        for (int k = -r; k <= r; k++) {
          int ny = ry + k;
          if (ny < 0 || ny >= rh) continue;
          uint32_t p = tmp[ny * rw + rx2];
          sr += (p>>16)&0xFF; sg += (p>>8)&0xFF; sb += p&0xFF; cnt++;
        }
        if (cnt) pixel[gy * buffer_width + gx] = ((sr/cnt)<<16)|((sg/cnt)<<8)|(sb/cnt);
      }
    }
  }

  bool need_adjust = (brightness != 1.f) || (contrast != 1.f) ||
                     (grayscale > 0.f)  || (sepia > 0.f);
  if (!need_adjust) return;

  for (int gy = y1; gy < y2; gy++) {
    for (int gx = x1; gx < x2; gx++) {
      uint32_t p = pixel[gy * buffer_width + gx];
      float R = ((p>>16)&0xFF) / 255.f;
      float G = ((p>> 8)&0xFF) / 255.f;
      float B = ( p     &0xFF) / 255.f;

      if (brightness != 1.f) { R *= brightness; G *= brightness; B *= brightness; }
      if (contrast != 1.f) {
        R = (R - 0.5f) * contrast + 0.5f;
        G = (G - 0.5f) * contrast + 0.5f;
        B = (B - 0.5f) * contrast + 0.5f;
      }
      if (grayscale > 0.f) {
        float lum = R * 0.299f + G * 0.587f + B * 0.114f;
        R = R * (1.f - grayscale) + lum * grayscale;
        G = G * (1.f - grayscale) + lum * grayscale;
        B = B * (1.f - grayscale) + lum * grayscale;
      }
      if (sepia > 0.f) {
        float sr = R * 0.393f + G * 0.769f + B * 0.189f;
        float sg = R * 0.349f + G * 0.686f + B * 0.168f;
        float sb = R * 0.272f + G * 0.534f + B * 0.131f;
        R = R * (1.f - sepia) + sr * sepia;
        G = G * (1.f - sepia) + sg * sepia;
        B = B * (1.f - sepia) + sb * sepia;
      }

      auto clamp_ch = [](float v) -> int { return v < 0.f ? 0 : v > 1.f ? 255 : (int)(v*255.f+.5f); };
      pixel[gy * buffer_width + gx] =
          ((uint32_t)clamp_ch(R) << 16) | ((uint32_t)clamp_ch(G) << 8) | (uint32_t)clamp_ch(B);
    }
  }
}

// ── render_frame() ────────────────────────────────────────────────────────────
// All the content-rendering logic from WM_PAINT, extracted here.
// The caller (WndProc) handles the GDI boilerplate (BeginPaint / BitBlt / EndPaint).
// mem_dc already has mem_bmp selected; we draw the chrome + content into it.

static std::string s_resolve_url_local(const std::string &src,
                                        const std::string &page_url) {
  if (src.empty()) return "";
  if (src.substr(0, 8) == "https://" || src.substr(0, 7) == "http://") return src;
  if (src.substr(0, 2) == "//") return "https:" + src;
  if (src[0] == '/') {
    size_t scheme_end = page_url.find("://");
    if (scheme_end == std::string::npos) return "";
    size_t host_end = page_url.find('/', scheme_end + 3);
    std::string origin = (host_end == std::string::npos) ? page_url : page_url.substr(0, host_end);
    return "https://" + origin.substr(origin.find("://") + 3) + src;
  }
  size_t last_slash = page_url.rfind('/');
  if (last_slash == std::string::npos) return src;
  return page_url.substr(0, last_slash + 1) + src;
}

static std::string s_pick_srcset_url(const std::string &srcset) {
  if (srcset.empty()) return "";
  std::string best_url;
  float best_val = -1.f;
  size_t pos = 0;
  while (pos < srcset.size()) {
    size_t comma = srcset.find(',', pos);
    std::string entry = (comma == std::string::npos) ? srcset.substr(pos) : srcset.substr(pos, comma - pos);
    pos = (comma == std::string::npos) ? srcset.size() : comma + 1;
    size_t s = entry.find_first_not_of(" \t");
    if (s == std::string::npos) continue;
    entry = entry.substr(s);
    size_t last_sp = entry.rfind(' ');
    std::string cand_url, desc;
    if (last_sp != std::string::npos) { cand_url = entry.substr(0, last_sp); desc = entry.substr(last_sp + 1); }
    else { cand_url = entry; }
    float val = 1.f;
    if (!desc.empty()) { try { val = std::stof(desc); } catch (...) { val = 1.f; } }
    if (val > best_val && !cand_url.empty()) { best_val = val; best_url = cand_url; }
  }
  return best_url;
}

void render_frame(HWND hwnd, HDC mem_dc, int win_w, int win_h,
                  int scroll_y, bool is_scrolling,
                  const std::string &g_current_page_url_ref) {
  extern float g_zoom_level;
  float zf = g_zoom_level;

  // Fill entire window with a dark background as fallback
  RECT fill_r = {0, 0, win_w, win_h};
  HBRUSH dark_br = CreateSolidBrush(RGB(34, 36, 42));
  ::FillRect(mem_dc, &fill_r, dark_br);
  DeleteObject(dark_br);

  // Paint chrome (tabs + toolbar + status bar)
  if (app_initialized) {
    browser_ui.paint(mem_dc);
  }

  if (!app_initialized || !buffer_memory || buffer_width <= 0 || buffer_height <= 0)
    return;

  // Clear pixel buffer to white
  uint32_t bg_color = 0x00FFFFFF;
  uint32_t *pixel = (uint32_t *)buffer_memory;
  int total_pixels = buffer_width * buffer_height;
  for (int i = 0; i < total_pixels; ++i)
    pixel[i] = bg_color;

  // Z-index sort of display list
  {
    std::vector<size_t> order(master_display_list.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
      const auto& ca = master_display_list[a];
      const auto& cb = master_display_list[b];
      int za = (ca.type == DisplayCommandType::ClipPush || ca.type == DisplayCommandType::ClipPop) ? 0 : ca.z_index;
      int zb = (cb.type == DisplayCommandType::ClipPush || cb.type == DisplayCommandType::ClipPop) ? 0 : cb.z_index;
      int fa = ca.fixed ? 1 : 0, fb = cb.fixed ? 1 : 0;
      if (fa != fb) return fa < fb;
      return za < zb;
    });
    DisplayList sorted;
    sorted.reserve(master_display_list.size());
    for (size_t i : order) sorted.push_back(master_display_list[i]);
    master_display_list = std::move(sorted);
  }

  // Clip rect helpers
  struct ClipEntry { int x, y, x2, y2; };
  auto apply_clip = [&](const std::vector<ClipEntry>& stk,
                        int& x1, int& y1, int& x2, int& y2) -> bool {
    if (stk.empty()) return true;
    const auto& cr = stk.back();
    x1 = std::max(x1, cr.x); y1 = std::max(y1, cr.y);
    x2 = std::min(x2, cr.x2); y2 = std::min(y2, cr.y2);
    return x1 < x2 && y1 < y2;
  };

  auto draw_borders = [&](const DisplayCommand &cmd, int bx1, int by1, int bx2, int by2) {
    // Skip transparent borders (0x01000000 sentinel)
    if (cmd.border_top_w > 0 && cmd.border_top_c != 0x01000000) {
      int th = (int)cmd.border_top_w;
      FillRect(bx1, by1, bx2-bx1, th, cmd.border_top_c);
    }
    if (cmd.border_bottom_w > 0 && cmd.border_bottom_c != 0x01000000) {
      int bh = (int)cmd.border_bottom_w;
      FillRect(bx1, by2-bh, bx2-bx1, bh, cmd.border_bottom_c);
    }
    if (cmd.border_left_w > 0 && cmd.border_left_c != 0x01000000) {
      int lw = (int)cmd.border_left_w;
      FillRect(bx1, by1, lw, by2-by1, cmd.border_left_c);
    }
    if (cmd.border_right_w > 0 && cmd.border_right_c != 0x01000000) {
      int rw = (int)cmd.border_right_w;
      FillRect(bx2-rw, by1, rw, by2-by1, cmd.border_right_c);
    }
    if (cmd.outline_w > 0) {
      int ow = (int)cmd.outline_w;
      int oo = (int)cmd.outline_offset; // outline-offset: gap between border and outline
      uint32_t oc = cmd.outline_c;
      int ox1 = bx1 - oo, oy1 = by1 - oo, ox2 = bx2 + oo, oy2 = by2 + oo;
      FillRect(ox1-ow, oy1-ow, ox2-ox1+2*ow, ow, oc);  // top
      FillRect(ox1-ow, oy2,    ox2-ox1+2*ow, ow, oc);  // bottom
      FillRect(ox1-ow, oy1,    ow, oy2-oy1, oc);        // left
      FillRect(ox2,    oy1,    ow, oy2-oy1, oc);        // right
    }
  };

  auto get_inner_scroll = [&](const Rect &r) -> float {
    for (const auto &sc : g_scroll_containers) {
      if (std::abs(sc.bounds.x - r.x) < 1.f &&
          std::abs(sc.bounds.y - r.y) < 1.f &&
          std::abs(sc.bounds.width - r.width) < 1.f) {
        return sc.scroll_y;
      }
    }
    return 0.f;
  };

  // ── Pass 1: non-fixed SolidColor backgrounds ──────────────────────────────
  {
    std::vector<ClipEntry> clip_stk;
    std::vector<float> inner_scroll_stk;
    float cur_inner_scroll = 0.f;
    for (const auto &cmd : master_display_list) {
      if (cmd.type == DisplayCommandType::ClipPush && !cmd.fixed) {
        float iscroll = cmd.clip_scrollable ? get_inner_scroll(cmd.rect) : 0.f;
        inner_scroll_stk.push_back(iscroll);
        cur_inner_scroll += iscroll;
        int cy = (int)(cmd.rect.y * zf - scroll_y);
        ClipEntry ce { (int)(cmd.rect.x * zf), cy,
                       (int)((cmd.rect.x + cmd.rect.width) * zf),
                       (int)(cy + cmd.rect.height * zf) };
        if (!clip_stk.empty()) {
          ce.x  = std::max(ce.x,  clip_stk.back().x);
          ce.y  = std::max(ce.y,  clip_stk.back().y);
          ce.x2 = std::min(ce.x2, clip_stk.back().x2);
          ce.y2 = std::min(ce.y2, clip_stk.back().y2);
        }
        clip_stk.push_back(ce);
      } else if (cmd.type == DisplayCommandType::ClipPop) {
        if (!clip_stk.empty()) clip_stk.pop_back();
        if (!inner_scroll_stk.empty()) {
          cur_inner_scroll -= inner_scroll_stk.back();
          inner_scroll_stk.pop_back();
        }
      } else if (cmd.type == DisplayCommandType::SolidColor && !cmd.fixed) {
        int y;
        if (cmd.sticky) {
          int normal_y = (int)(cmd.sticky_orig_y * zf - scroll_y - cur_inner_scroll);
          int pinned_y = !clip_stk.empty() ? clip_stk.back().y : (int)(cmd.sticky_top * zf);
          y = std::max(normal_y, pinned_y);
        } else {
          y = (int)(cmd.rect.y * zf - scroll_y - cur_inner_scroll);
        }
        int x1 = (int)(cmd.rect.x * zf) + (int)(cmd.transform_tx * zf);
        int y1 = y + (int)(cmd.transform_ty * zf);
        int x2 = x1 + (int)(cmd.rect.width * zf), y2 = y1 + (int)(cmd.rect.height * zf);
        if (cmd.shadow_c != 0 && (cmd.shadow_x != 0 || cmd.shadow_y != 0 || cmd.shadow_blur != 0)) {
          int spread = std::max(1, (int)(cmd.shadow_blur * 0.3f));
          int sx1 = x1 + (int)cmd.shadow_x - spread;
          int sy1 = y1 + (int)cmd.shadow_y - spread;
          int sx2 = x2 + (int)cmd.shadow_x + spread;
          int sy2 = y2 + (int)cmd.shadow_y + spread;
          float shadow_opacity = cmd.shadow_blur > 0 ? 0.15f : 0.3f;
          FillRectOpacity(sx1, sy1, sx2-sx1, sy2-sy1, cmd.shadow_c, shadow_opacity);
        }
        int cx1=x1, cy1=y1, cx2=x2, cy2=y2;
        if (!apply_clip(clip_stk, cx1, cy1, cx2, cy2)) continue;
        int w = cx2 - cx1, h = cy2 - cy1;
        if (!cmd.gradient_stops.empty()) {
          switch (cmd.gradient_type) {
            case GradientType::Radial:
              FillRadialGradient(x1, y1, x2-x1, y2-y1,
                  cmd.gradient_stops, cmd.gradient_radial_ellipse, cmd.opacity);
              break;
            case GradientType::Conic:
              FillConicGradient(x1, y1, x2-x1, y2-y1,
                  cmd.gradient_stops, cmd.gradient_conic_from, cmd.opacity);
              break;
            default:
              FillLinearGradient(x1, y1, x2-x1, y2-y1,
                  cmd.gradient_stops, cmd.gradient_angle, cmd.opacity);
              break;
          }
        } else if (cmd.color != 0x01000000) {
          if (cmd.opacity < 0.99f) {
            FillRectOpacity(cx1, cy1, w, h, cmd.color, cmd.opacity);
          } else if (cmd.border_radius > 0) {
            FillRoundedRect(cx1, cy1, w, h, (int)cmd.border_radius, cmd.color);
          } else {
            FillRect(cx1, cy1, w, h, cmd.color);
          }
        }
        if (cmd.has_filter) {
          ApplyFilter(x1, y1, x2-x1, y2-y1,
              cmd.filter_blur, cmd.filter_brightness,
              cmd.filter_contrast, cmd.filter_grayscale, cmd.filter_sepia);
        }
        draw_borders(cmd, x1, y1, x2, y2);
      }
    }
  }

  // ── Pass 2: fixed SolidColor backgrounds ──────────────────────────────────
  {
    std::vector<ClipEntry> clip_stk;
    for (const auto &cmd : master_display_list) {
      if (cmd.type == DisplayCommandType::ClipPush && cmd.fixed) {
        ClipEntry ce { (int)(cmd.rect.x * zf), (int)(cmd.rect.y * zf),
                       (int)((cmd.rect.x + cmd.rect.width) * zf),
                       (int)((cmd.rect.y + cmd.rect.height) * zf) };
        if (!clip_stk.empty()) {
          ce.x  = std::max(ce.x,  clip_stk.back().x);
          ce.y  = std::max(ce.y,  clip_stk.back().y);
          ce.x2 = std::min(ce.x2, clip_stk.back().x2);
          ce.y2 = std::min(ce.y2, clip_stk.back().y2);
        }
        clip_stk.push_back(ce);
      } else if (cmd.type == DisplayCommandType::ClipPop) {
        if (!clip_stk.empty()) clip_stk.pop_back();
      } else if (cmd.type == DisplayCommandType::SolidColor && cmd.fixed) {
        int x1 = (int)(cmd.rect.x * zf) + (int)(cmd.transform_tx * zf);
        int y1 = (int)(cmd.rect.y * zf) + (int)(cmd.transform_ty * zf);
        int x2 = x1 + (int)(cmd.rect.width * zf), y2 = y1 + (int)(cmd.rect.height * zf);
        if (cmd.shadow_c != 0 && (cmd.shadow_x != 0 || cmd.shadow_y != 0 || cmd.shadow_blur != 0)) {
          int spread = std::max(1, (int)(cmd.shadow_blur * 0.3f));
          int sx1 = x1 + (int)cmd.shadow_x - spread;
          int sy1 = y1 + (int)cmd.shadow_y - spread;
          int sx2 = x2 + (int)cmd.shadow_x + spread;
          int sy2 = y2 + (int)cmd.shadow_y + spread;
          float shadow_opacity = cmd.shadow_blur > 0 ? 0.15f : 0.3f;
          FillRectOpacity(sx1, sy1, sx2-sx1, sy2-sy1, cmd.shadow_c, shadow_opacity);
        }
        int cx1=x1, cy1=y1, cx2=x2, cy2=y2;
        if (!apply_clip(clip_stk, cx1, cy1, cx2, cy2)) continue;
        int w = cx2 - cx1, h = cy2 - cy1;
        if (!cmd.gradient_stops.empty()) {
          switch (cmd.gradient_type) {
            case GradientType::Radial:
              FillRadialGradient(x1, y1, x2-x1, y2-y1,
                  cmd.gradient_stops, cmd.gradient_radial_ellipse, cmd.opacity);
              break;
            case GradientType::Conic:
              FillConicGradient(x1, y1, x2-x1, y2-y1,
                  cmd.gradient_stops, cmd.gradient_conic_from, cmd.opacity);
              break;
            default:
              FillLinearGradient(x1, y1, x2-x1, y2-y1,
                  cmd.gradient_stops, cmd.gradient_angle, cmd.opacity);
              break;
          }
        } else if (cmd.color != 0x01000000) {
          if (cmd.opacity < 0.99f) {
            FillRectOpacity(cx1, cy1, w, h, cmd.color, cmd.opacity);
          } else if (cmd.border_radius > 0) {
            FillRoundedRect(cx1, cy1, w, h, (int)cmd.border_radius, cmd.color);
          } else {
            FillRect(cx1, cy1, w, h, cmd.color);
          }
        }
        if (cmd.has_filter) {
          ApplyFilter(x1, y1, x2-x1, y2-y1,
              cmd.filter_blur, cmd.filter_brightness,
              cmd.filter_contrast, cmd.filter_grayscale, cmd.filter_sepia);
        }
        draw_borders(cmd, x1, y1, x2, y2);
      }
    }
  }

  // ── Blit pixel buffer → content_dc, then draw text and images ────────────
  HDC content_dc = CreateCompatibleDC(mem_dc);
  if (!content_dc) return;

  HBITMAP content_bmp = CreateCompatibleBitmap(mem_dc, buffer_width, buffer_height);
  if (!content_bmp) { DeleteDC(content_dc); return; }

  HGDIOBJ old_content_bmp = SelectObject(content_dc, content_bmp);

  StretchDIBits(content_dc, 0, 0, buffer_width, buffer_height, 0, 0,
                buffer_width, buffer_height, buffer_memory,
                &bitmap_info, DIB_RGB_COLORS, SRCCOPY);

  SetBkMode(content_dc, TRANSPARENT);

  // text drawing helper
  auto draw_text_cmd = [&](const DisplayCommand &cmd) {
    uint32_t text_col = cmd.color;
    if (cmd.opacity < 0.99f)
      text_col = blend_pixel(0x00FFFFFF, cmd.color, cmd.opacity);
    COLORREF gdi_color = RGB((text_col >> 16) & 0xFF, (text_col >> 8) & 0xFF, text_col & 0xFF);
    SetTextColor(content_dc, gdi_color);
    SetBkMode(content_dc, TRANSPARENT);
    if (cmd.letter_spacing != 0.f)
      SetTextCharacterExtra(content_dc, (int)(cmd.letter_spacing * zf));
    bool gdi_underline = cmd.underline &&
                         cmd.text_decoration_line.empty() &&
                         cmd.text_decoration_style.empty();
    const char* font_name = cmd.font_family.empty() ? "Arial" : cmd.font_family.c_str();
    int scaled_font_size = (int)(cmd.font_size * zf);
    if (scaled_font_size < 1) scaled_font_size = 1;
    HFONT hFont = CreateFontA(
        scaled_font_size, 0, 0, 0, cmd.bold ? FW_BOLD : FW_NORMAL,
        cmd.italic ? TRUE : FALSE, gdi_underline ? TRUE : FALSE,
        FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, font_name);
    if (hFont) {
      HGDIOBJ old_font = SelectObject(content_dc, hFont);
      int wlen = MultiByteToWideChar(CP_UTF8, 0, cmd.text.c_str(), (int)cmd.text.length(), NULL, 0);
      if (wlen > 0) {
        std::wstring wstr(wlen, 0);
        MultiByteToWideChar(CP_UTF8, 0, cmd.text.c_str(), (int)cmd.text.length(), &wstr[0], wlen);
        int y;
        if (cmd.fixed) {
          y = (int)(cmd.rect.y * zf);
        } else if (cmd.sticky) {
          int normal_y = (int)(cmd.sticky_orig_y * zf) - scroll_y;
          int pinned_y = (int)(cmd.sticky_top * zf);
          y = std::max(normal_y, pinned_y);
        } else {
          y = (int)(cmd.rect.y * zf) - scroll_y;
        }
        int tx_off = (int)(cmd.transform_tx * zf);
        int ty_off = (int)(cmd.transform_ty * zf);
        y += ty_off;
        int text_x = (int)(cmd.rect.x * zf);
        int text_w = (int)(cmd.rect.width * zf);
        if (cmd.text_shadow_c != 0 && (cmd.text_shadow_x != 0.f || cmd.text_shadow_y != 0.f)) {
          COLORREF sh_color = RGB((cmd.text_shadow_c >> 16) & 0xFF,
                                   (cmd.text_shadow_c >>  8) & 0xFF,
                                    cmd.text_shadow_c        & 0xFF);
          SetTextColor(content_dc, sh_color);
          int sx = text_x + tx_off + (int)(cmd.text_shadow_x * zf);
          int sy = y + (int)(cmd.text_shadow_y * zf);
          TextOutW(content_dc, sx, sy, wstr.c_str(), wlen);
          SetTextColor(content_dc, gdi_color);
        }
        bool use_draw_text = (!cmd.text_align.empty() && cmd.text_align != "left" &&
                              cmd.rect.width > 0) || cmd.ellipsis;
        if (use_draw_text) {
          RECT tr = {text_x + tx_off, y,
                     text_x + text_w + tx_off, y + scaled_font_size + 4};
          UINT fmt = DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX;
          if (cmd.text_align == "center") fmt |= DT_CENTER;
          else if (cmd.text_align == "right") fmt |= DT_RIGHT;
          if (cmd.ellipsis) fmt |= DT_END_ELLIPSIS;
          DrawTextW(content_dc, wstr.c_str(), wlen, &tr, fmt);
        } else {
          TextOutW(content_dc, text_x + tx_off, y, wstr.c_str(), wlen);
        }
        if (!cmd.text_decoration_line.empty() && cmd.text_decoration_line != "none") {
          const std::string &tdl = cmd.text_decoration_line;
          uint32_t dc2 = cmd.text_decoration_color ? cmd.text_decoration_color : cmd.color;
          COLORREF td_cr = RGB((dc2>>16)&0xFF,(dc2>>8)&0xFF,dc2&0xFF);
          const std::string &tds = cmd.text_decoration_style;
          int tdx = text_x + tx_off;
          int tdw = text_w;
          int baseline_y = y + scaled_font_size;
          auto draw_deco_line = [&](int line_y) {
            HPEN pen = NULL;
            if (tds == "dashed") pen = CreatePen(PS_DASH, 1, td_cr);
            else if (tds == "dotted") pen = CreatePen(PS_DOT, 1, td_cr);
            else pen = CreatePen(PS_SOLID, 1, td_cr);
            if (pen) {
              HGDIOBJ old_pen2 = SelectObject(content_dc, pen);
              if (tds == "double") {
                MoveToEx(content_dc, tdx, line_y - 1, NULL); LineTo(content_dc, tdx + tdw, line_y - 1);
                MoveToEx(content_dc, tdx, line_y + 1, NULL); LineTo(content_dc, tdx + tdw, line_y + 1);
              } else if (tds == "wavy") {
                MoveToEx(content_dc, tdx, line_y, NULL);
                for (int xi = tdx; xi < tdx + tdw; xi += 4) {
                  int oy = ((xi - tdx) / 4) % 2 == 0 ? -2 : 2;
                  LineTo(content_dc, xi + 2, line_y + oy);
                  LineTo(content_dc, xi + 4, line_y);
                }
              } else {
                MoveToEx(content_dc, tdx, line_y, NULL);
                LineTo(content_dc, tdx + tdw, line_y);
              }
              SelectObject(content_dc, old_pen2);
              DeleteObject(pen);
            }
          };
          if (tdl.find("underline")    != std::string::npos) draw_deco_line(baseline_y + 2);
          if (tdl.find("overline")     != std::string::npos) draw_deco_line(y - 1);
          if (tdl.find("line-through") != std::string::npos) draw_deco_line(y + scaled_font_size / 2);
        }
      }
      SelectObject(content_dc, old_font);
      DeleteObject(hFont);
    }
    if (cmd.letter_spacing != 0.f)
      SetTextCharacterExtra(content_dc, 0);
  };

  // Image blit helper
  auto blit_one_img = [&](const std::shared_ptr<LayoutBox> &box, bool want_fixed) {
    if (!box || !box->style_node || !box->style_node->node) return;
    bool is_fixed_box = box->style_node->value("position") == "fixed";
    if (is_fixed_box != want_fixed) return;
    if (box->style_node->node->type != NodeType::Element ||
        box->style_node->node->data != "img") return;
    auto &attrs = box->style_node->node->attributes;
    std::string chosen_src;
    auto ss_it = attrs.find("srcset");
    if (ss_it != attrs.end() && !ss_it->second.empty())
      chosen_src = s_pick_srcset_url(ss_it->second);
    if (chosen_src.empty()) {
      auto src_it = attrs.find("src");
      if (src_it != attrs.end()) chosen_src = src_it->second;
    }
    if (chosen_src.empty()) return;
    std::string url;
    if (chosen_src.size() > 6 && chosen_src.substr(0, 6) == "__svg_")
      url = chosen_src;
    else if (chosen_src.size() > 5 && chosen_src.substr(0, 5) == "data:")
      url = chosen_src;
    else
      url = s_resolve_url_local(chosen_src, g_current_page_url_ref);
    auto it_img = g_image_cache.find(url);
    if (it_img == g_image_cache.end() || it_img->second.width <= 0) return;
    const CachedImage &img = it_img->second;
    int dest_x = (int)(box->dimensions.content.x * zf);
    int dest_y = is_fixed_box ? (int)(box->dimensions.content.y * zf)
                              : (int)(box->dimensions.content.y * zf) - scroll_y;
    int dest_w = (int)(box->dimensions.content.width * zf);
    int dest_h = (int)(box->dimensions.content.height * zf);
    if (dest_w <= 0) dest_w = img.width;
    if (dest_h <= 0) dest_h = img.height;

    // object-fit support
    std::string obj_fit = "fill";
    if (box->style_node)
      obj_fit = box->style_node->value("object-fit");
    if (obj_fit.empty()) obj_fit = "fill";

    int src_x = 0, src_y = 0, src_w = img.width, src_h = img.height;
    int draw_x = dest_x, draw_y = dest_y, draw_w = dest_w, draw_h = dest_h;

    if (obj_fit == "contain") {
      float scale_x = (float)dest_w / img.width;
      float scale_y = (float)dest_h / img.height;
      float scale = (scale_x < scale_y) ? scale_x : scale_y;
      draw_w = (int)(img.width * scale);
      draw_h = (int)(img.height * scale);
      draw_x = dest_x + (dest_w - draw_w) / 2;
      draw_y = dest_y + (dest_h - draw_h) / 2;
    } else if (obj_fit == "cover") {
      float scale_x = (float)dest_w / img.width;
      float scale_y = (float)dest_h / img.height;
      float scale = (scale_x > scale_y) ? scale_x : scale_y;
      int scaled_w = (int)(img.width * scale);
      int scaled_h = (int)(img.height * scale);
      // Crop from center of source image
      src_w = (int)(dest_w / scale);
      src_h = (int)(dest_h / scale);
      src_x = (img.width - src_w) / 2;
      src_y = (img.height - src_h) / 2;
    } else if (obj_fit == "none") {
      // Draw at natural size, centered, clip to box
      draw_w = img.width;
      draw_h = img.height;
      draw_x = dest_x + (dest_w - draw_w) / 2;
      draw_y = dest_y + (dest_h - draw_h) / 2;
    } else if (obj_fit == "scale-down") {
      if (img.width > dest_w || img.height > dest_h) {
        // Use contain
        float scale_x = (float)dest_w / img.width;
        float scale_y = (float)dest_h / img.height;
        float scale = (scale_x < scale_y) ? scale_x : scale_y;
        draw_w = (int)(img.width * scale);
        draw_h = (int)(img.height * scale);
        draw_x = dest_x + (dest_w - draw_w) / 2;
        draw_y = dest_y + (dest_h - draw_h) / 2;
      } else {
        // Use none (natural size)
        draw_w = img.width;
        draw_h = img.height;
        draw_x = dest_x + (dest_w - draw_w) / 2;
        draw_y = dest_y + (dest_h - draw_h) / 2;
      }
    }
    // else: "fill" — default stretch behavior (draw_* == dest_*)

    // Clip to element bounds for cover/none/scale-down
    if (obj_fit == "cover" || obj_fit == "none" || obj_fit == "scale-down") {
      HRGN clip_rgn = CreateRectRgn(dest_x, dest_y, dest_x + dest_w, dest_y + dest_h);
      SelectClipRgn(content_dc, clip_rgn);
      DeleteObject(clip_rgn);
    }

    BITMAPINFO img_bmi = {};
    img_bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    img_bmi.bmiHeader.biWidth = img.width;
    img_bmi.bmiHeader.biHeight = -img.height;
    img_bmi.bmiHeader.biPlanes = 1;
    img_bmi.bmiHeader.biBitCount = 32;
    img_bmi.bmiHeader.biCompression = BI_RGB;
    StretchDIBits(content_dc, draw_x, draw_y, draw_w, draw_h, src_x, src_y,
                  src_w, src_h, img.pixels.data(), &img_bmi,
                  DIB_RGB_COLORS, SRCCOPY);

    // Restore clip region if we set one
    if (obj_fit == "cover" || obj_fit == "none" || obj_fit == "scale-down") {
      SelectClipRgn(content_dc, NULL);
    }
  };

  std::function<void(const std::shared_ptr<LayoutBox>&, bool)> blit_images_pass;
  blit_images_pass = [&](const std::shared_ptr<LayoutBox> &box, bool want_fixed) {
    if (!box) return;
    blit_one_img(box, want_fixed);
    for (auto &child : box->children) blit_images_pass(child, want_fixed);
  };

  // ── Pass 1: non-fixed text ────────────────────────────────────────────────
  {
    std::vector<int> dc_saves;
    std::vector<float> text_inner_scroll_stk;
    float text_cur_inner_scroll = 0.f;
    std::vector<int> text_clip_top_stk;
    for (const auto &cmd : master_display_list) {
      if (cmd.type == DisplayCommandType::ClipPush && !cmd.fixed) {
        float iscroll = cmd.clip_scrollable ? get_inner_scroll(cmd.rect) : 0.f;
        text_inner_scroll_stk.push_back(iscroll);
        text_cur_inner_scroll += iscroll;
        int save_id = SaveDC(content_dc);
        dc_saves.push_back(save_id);
        int cy = (int)(cmd.rect.y * zf) - scroll_y;
        text_clip_top_stk.push_back(cy);
        IntersectClipRect(content_dc, (int)(cmd.rect.x * zf), cy,
          (int)((cmd.rect.x + cmd.rect.width) * zf), (int)(cy + cmd.rect.height * zf));
      } else if (cmd.type == DisplayCommandType::ClipPop) {
        if (!dc_saves.empty()) {
          RestoreDC(content_dc, dc_saves.back());
          dc_saves.pop_back();
          SetBkMode(content_dc, TRANSPARENT);
        }
        if (!text_inner_scroll_stk.empty()) {
          text_cur_inner_scroll -= text_inner_scroll_stk.back();
          text_inner_scroll_stk.pop_back();
        }
        if (!text_clip_top_stk.empty()) text_clip_top_stk.pop_back();
      } else if (cmd.type == DisplayCommandType::Text && !cmd.fixed) {
        if (text_cur_inner_scroll > 0.01f) {
          DisplayCommand adjusted = cmd;
          if (adjusted.sticky) {
            adjusted.sticky_orig_y -= text_cur_inner_scroll;
            if (!text_clip_top_stk.empty())
              adjusted.sticky_top = (float)text_clip_top_stk.back();
          } else {
            adjusted.rect.y -= text_cur_inner_scroll;
          }
          draw_text_cmd(adjusted);
        } else {
          draw_text_cmd(cmd);
        }
      }
    }
    while (!dc_saves.empty()) { RestoreDC(content_dc, dc_saves.back()); dc_saves.pop_back(); }
  }

  // ── Pass 2: non-fixed images ──────────────────────────────────────────────
  if (global_layout_root) {
    EnterCriticalSection(&g_image_cache_cs);
    blit_images_pass(global_layout_root, false);
    LeaveCriticalSection(&g_image_cache_cs);
  }

  // ── Pass 2b: CSS background-image with size/position/repeat ─────────────
  EnterCriticalSection(&g_image_cache_cs);
  for (const auto &cmd : master_display_list) {
    if (cmd.type == DisplayCommandType::SolidColor && !cmd.fixed &&
        !cmd.bg_image_url.empty()) {
      std::string url = cmd.bg_image_url;
      if (url.size() <= 5 || url.substr(0, 5) != "data:")
        url = s_resolve_url_local(cmd.bg_image_url, g_current_page_url_ref);
      auto it_img = g_image_cache.find(url);
      if (it_img == g_image_cache.end() || it_img->second.width <= 0) continue;
      const CachedImage &img = it_img->second;
      int box_x = (int)(cmd.rect.x * zf);
      int box_y = (int)(cmd.rect.y * zf) - scroll_y;
      int box_w = (int)(cmd.rect.width * zf);
      int box_h = (int)(cmd.rect.height * zf);
      if (box_w <= 0 || box_h <= 0) continue;

      // Compute rendered image size based on background-size
      int render_w = img.width, render_h = img.height;
      const std::string &bsz = cmd.bg_size;
      if (bsz == "cover") {
        float scale = std::max((float)box_w / img.width, (float)box_h / img.height);
        render_w = (int)(img.width * scale);
        render_h = (int)(img.height * scale);
      } else if (bsz == "contain") {
        float scale = std::min((float)box_w / img.width, (float)box_h / img.height);
        render_w = (int)(img.width * scale);
        render_h = (int)(img.height * scale);
      } else if (!bsz.empty() && bsz != "auto") {
        // Parse "Wpx Hpx" or "W% H%" or single value
        std::istringstream bss(bsz);
        std::string w_s, h_s;
        bss >> w_s;
        bss >> h_s;
        auto parse_bg_dim = [](const std::string &s, int box_dim, int img_dim) -> int {
          if (s.empty() || s == "auto") return img_dim;
          if (s.back() == '%') {
            try { return (int)(std::stof(s) / 100.f * box_dim); } catch (...) { return img_dim; }
          }
          try { return (int)std::stof(s); } catch (...) { return img_dim; }
        };
        render_w = parse_bg_dim(w_s, box_w, img.width);
        if (h_s.empty()) {
          // single value: width set, height preserves aspect ratio
          float aspect = (float)img.height / (float)img.width;
          render_h = (int)(render_w * aspect);
        } else {
          render_h = parse_bg_dim(h_s, box_h, img.height);
        }
      } else {
        // "auto" or empty — per CSS spec, use the intrinsic image size.
        // Earlier code stretched to box dimensions, which broke repeat
        // tests (only one tile would ever fit) and made `no-repeat`
        // appear identical to a stretched fill.
        render_w = img.width;
        render_h = img.height;
      }
      if (render_w <= 0) render_w = 1;
      if (render_h <= 0) render_h = 1;

      // Compute position based on background-position
      int pos_x = 0, pos_y = 0; // offset from box top-left
      const std::string &bpos = cmd.bg_position;
      if (!bpos.empty()) {
        // Parse X and Y components
        std::string xp, yp;
        // Handle keyword combinations
        std::istringstream bps(bpos);
        bps >> xp;
        bps >> yp;
        if (yp.empty()) {
          // Single value: if "center", both axes center; if keyword, other axis is center
          if (xp == "center") { yp = "center"; }
          else if (xp == "top" || xp == "bottom") { yp = xp; xp = "center"; }
          else { yp = "center"; }
        }
        // Resolve X
        if (xp == "left") pos_x = 0;
        else if (xp == "right") pos_x = box_w - render_w;
        else if (xp == "center") pos_x = (box_w - render_w) / 2;
        else if (xp.back() == '%') {
          try { pos_x = (int)(std::stof(xp) / 100.f * (box_w - render_w)); } catch (...) {}
        } else {
          try { pos_x = (int)std::stof(xp); } catch (...) {}
        }
        // Resolve Y
        if (yp == "top") pos_y = 0;
        else if (yp == "bottom") pos_y = box_h - render_h;
        else if (yp == "center") pos_y = (box_h - render_h) / 2;
        else if (yp.back() == '%') {
          try { pos_y = (int)(std::stof(yp) / 100.f * (box_h - render_h)); } catch (...) {}
        } else {
          try { pos_y = (int)std::stof(yp); } catch (...) {}
        }
      } else {
        // Default: top left (same as original stretch behavior)
        pos_x = 0;
        pos_y = 0;
      }

      // Determine repeat mode
      const std::string &brep = cmd.bg_repeat;
      bool repeat_x = true, repeat_y = true;
      if (brep == "no-repeat") { repeat_x = false; repeat_y = false; }
      else if (brep == "repeat-x") { repeat_x = true; repeat_y = false; }
      else if (brep == "repeat-y") { repeat_x = false; repeat_y = true; }
      // "repeat" or empty = both true (default)

      // Set up image bitmap info
      BITMAPINFO img_bmi = {};
      img_bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
      img_bmi.bmiHeader.biWidth = img.width;
      img_bmi.bmiHeader.biHeight = -img.height;
      img_bmi.bmiHeader.biPlanes = 1;
      img_bmi.bmiHeader.biBitCount = 32;
      img_bmi.bmiHeader.biCompression = BI_RGB;

      // Clip to box bounds
      HRGN clip_rgn = CreateRectRgn(box_x, box_y, box_x + box_w, box_y + box_h);
      SelectClipRgn(content_dc, clip_rgn);

      // Calculate tile ranges
      int start_tx = repeat_x ? ((pos_x % render_w) - render_w) : pos_x;
      int start_ty = repeat_y ? ((pos_y % render_h) - render_h) : pos_y;
      int end_tx = repeat_x ? box_w : (pos_x + render_w);
      int end_ty = repeat_y ? box_h : (pos_y + render_h);

      for (int ty = start_ty; ty < end_ty; ty += render_h) {
        for (int tx = start_tx; tx < end_tx; tx += render_w) {
          StretchDIBits(content_dc, box_x + tx, box_y + ty, render_w, render_h,
                        0, 0, img.width, img.height,
                        img.pixels.data(), &img_bmi, DIB_RGB_COLORS, SRCCOPY);
          if (!repeat_x) break;
        }
        if (!repeat_y) break;
      }

      SelectClipRgn(content_dc, NULL);
      DeleteObject(clip_rgn);
    }
  }
  LeaveCriticalSection(&g_image_cache_cs);

  // ── Pass 3: fixed text ────────────────────────────────────────────────────
  {
    std::vector<int> dc_saves;
    for (const auto &cmd : master_display_list) {
      if (cmd.type == DisplayCommandType::ClipPush && cmd.fixed) {
        int save_id = SaveDC(content_dc);
        dc_saves.push_back(save_id);
        IntersectClipRect(content_dc,
          (int)cmd.rect.x, (int)cmd.rect.y,
          (int)(cmd.rect.x + cmd.rect.width),
          (int)(cmd.rect.y + cmd.rect.height));
      } else if (cmd.type == DisplayCommandType::ClipPop) {
        if (!dc_saves.empty()) {
          RestoreDC(content_dc, dc_saves.back());
          dc_saves.pop_back();
          SetBkMode(content_dc, TRANSPARENT);
        }
      } else if (cmd.type == DisplayCommandType::Text && cmd.fixed) {
        draw_text_cmd(cmd);
      }
    }
    while (!dc_saves.empty()) { RestoreDC(content_dc, dc_saves.back()); dc_saves.pop_back(); }
  }

  // ── Pass 4: fixed images ──────────────────────────────────────────────────
  if (global_layout_root) {
    EnterCriticalSection(&g_image_cache_cs);
    blit_images_pass(global_layout_root, true);
    LeaveCriticalSection(&g_image_cache_cs);
  }

  int content_y = browser_ui.content_y();
  BitBlt(mem_dc, 0, content_y, buffer_width, buffer_height, content_dc, 0, 0, SRCCOPY);

  SelectObject(content_dc, old_content_bmp);
  DeleteObject(content_bmp);
  DeleteDC(content_dc);

  // ── GDI alpha blend helper (draws semi-transparent rect on mem_dc) ──────
  auto gdi_alpha_rect = [&](int rx, int ry, int rw, int rh, COLORREF color, int alpha) {
    if (rw <= 0 || rh <= 0) return;
    // Clip to visible area
    int cy_top = content_y;
    if (ry + rh < cy_top || ry > win_h) return;
    HDC tmp_dc = CreateCompatibleDC(mem_dc);
    if (!tmp_dc) return;
    HBITMAP tmp_bmp = CreateCompatibleBitmap(mem_dc, rw, rh);
    if (!tmp_bmp) { DeleteDC(tmp_dc); return; }
    HGDIOBJ old_bmp2 = SelectObject(tmp_dc, tmp_bmp);
    RECT fill_r = {0, 0, rw, rh};
    HBRUSH br = CreateSolidBrush(color);
    ::FillRect(tmp_dc, &fill_r, br);
    DeleteObject(br);
    BLENDFUNCTION bf;
    bf.BlendOp = AC_SRC_OVER;
    bf.BlendFlags = 0;
    bf.SourceConstantAlpha = (BYTE)alpha;
    bf.AlphaFormat = 0;
    AlphaBlend(mem_dc, rx, ry, rw, rh, tmp_dc, 0, 0, rw, rh, bf);
    SelectObject(tmp_dc, old_bmp2);
    DeleteObject(tmp_bmp);
    DeleteDC(tmp_dc);
  };

  // ── Selection highlight ──────────────────────────────────────────────────
  {
    extern bool g_has_selection;
    extern float g_sel_min_x, g_sel_min_y, g_sel_max_x, g_sel_max_y;
    extern float g_zoom_level;
    if (g_has_selection) {
      float z = g_zoom_level;
      for (auto &cmd : master_display_list) {
        if (cmd.type != DisplayCommandType::Text || cmd.fixed) continue;
        float ty = cmd.rect.y;
        float tx = cmd.rect.x;
        float tw = cmd.rect.width;
        float th = cmd.font_size > 0 ? cmd.font_size * 1.4f : 20.f;
        float tb = ty + th;
        if (tb < g_sel_min_y || ty > g_sel_max_y) continue;
        float hx1 = tx, hx2 = tx + tw;
        bool single_line = (g_sel_max_y - g_sel_min_y) < th;
        if (single_line || (ty >= g_sel_min_y - 1 && ty <= g_sel_min_y + th)) {
          hx1 = std::max(hx1, g_sel_min_x);
        }
        if (single_line || (tb >= g_sel_max_y - 1 && tb <= g_sel_max_y + th + 1)) {
          hx2 = std::min(hx2, g_sel_max_x);
        }
        if (hx1 >= hx2) continue;
        int sy = (int)(ty * z) - scroll_y + content_y;
        int sx = (int)(hx1 * z);
        int sw = (int)((hx2 - hx1) * z);
        int sh = (int)(th * z);
        gdi_alpha_rect(sx, sy, sw, sh, RGB(0x33, 0x99, 0xFF), 90);
      }
    }
  }

  // ── Find-in-page highlights ────────────────────────────────────────────────
  {
    extern bool g_find_bar_open;
    extern std::vector<FindMatch> g_find_matches;
    extern int g_find_current_match;
    extern float g_zoom_level;
    if (g_find_bar_open && !g_find_matches.empty()) {
      float z = g_zoom_level;
      for (int i = 0; i < (int)g_find_matches.size(); i++) {
        auto &m = g_find_matches[i];
        int mx2 = (int)(m.x * z);
        int my2 = (int)(m.y * z) - scroll_y + content_y;
        int mw2 = (int)(m.w * z);
        int mh2 = (int)(m.h * z);
        bool current = (i == g_find_current_match);
        COLORREF color = current ? RGB(0xFF, 0x99, 0x00) : RGB(0xFF, 0xFF, 0x00);
        int alpha = current ? 153 : 90;
        gdi_alpha_rect(mx2, my2, mw2, mh2, color, alpha);
      }
    }
  }

  // ── Find bar UI ────────────────────────────────────────────────────────────
  {
    extern bool g_find_bar_open;
    extern std::string g_find_query;
    extern int g_find_current_match, g_find_total_matches;
    if (g_find_bar_open) {
      int bar_h = 30;
      int bar_y = content_y;
      int bar_w = 400;
      int bar_x = win_w - bar_w - 20;
      // Background
      RECT bar_r = {bar_x, bar_y, bar_x + bar_w, bar_y + bar_h};
      HBRUSH bar_br = CreateSolidBrush(RGB(50, 52, 60));
      ::FillRect(mem_dc, &bar_r, bar_br);
      DeleteObject(bar_br);
      // Border
      HPEN border_pen = CreatePen(PS_SOLID, 1, RGB(100, 105, 120));
      HGDIOBJ old_pen = SelectObject(mem_dc, border_pen);
      HGDIOBJ old_br = SelectObject(mem_dc, GetStockObject(NULL_BRUSH));
      Rectangle(mem_dc, bar_x, bar_y, bar_x + bar_w, bar_y + bar_h);
      SelectObject(mem_dc, old_br);
      SelectObject(mem_dc, old_pen);
      DeleteObject(border_pen);
      // Text input area
      RECT input_r = {bar_x + 8, bar_y + 4, bar_x + bar_w - 120, bar_y + bar_h - 4};
      HBRUSH input_br = CreateSolidBrush(RGB(35, 37, 43));
      ::FillRect(mem_dc, &input_r, input_br);
      DeleteObject(input_br);
      // Query text
      SetBkMode(mem_dc, TRANSPARENT);
      SetTextColor(mem_dc, RGB(220, 220, 220));
      HFONT fb_font = CreateFontA(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Arial");
      HGDIOBJ old_fb_font = SelectObject(mem_dc, fb_font);
      std::string disp = g_find_query + "|";
      RECT text_r = {bar_x + 12, bar_y + 7, bar_x + bar_w - 125, bar_y + bar_h - 4};
      DrawTextA(mem_dc, disp.c_str(), -1, &text_r, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
      // Match count
      std::string info = std::to_string(g_find_total_matches > 0 ? g_find_current_match + 1 : 0)
                         + "/" + std::to_string(g_find_total_matches);
      RECT info_r = {bar_x + bar_w - 115, bar_y + 7, bar_x + bar_w - 35, bar_y + bar_h - 4};
      DrawTextA(mem_dc, info.c_str(), -1, &info_r, DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX);
      // Close button "X"
      SetTextColor(mem_dc, RGB(180, 180, 180));
      RECT close_r = {bar_x + bar_w - 25, bar_y + 5, bar_x + bar_w - 5, bar_y + bar_h - 5};
      DrawTextA(mem_dc, "X", -1, &close_r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      // Hint text
      SetTextColor(mem_dc, RGB(120, 120, 130));
      HFONT hint_font = CreateFontA(11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Arial");
      HGDIOBJ old_hint = SelectObject(mem_dc, hint_font);
      RECT hint_r = {bar_x + 8, bar_y + bar_h, bar_x + bar_w, bar_y + bar_h + 14};
      DrawTextA(mem_dc, "Press Esc to close", -1, &hint_r, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
      SelectObject(mem_dc, old_hint);
      DeleteObject(hint_font);
      SetTextColor(mem_dc, RGB(220, 220, 220));
      SelectObject(mem_dc, old_fb_font);
      DeleteObject(fb_font);
    }
  }

  // ── Zoom indicator ─────────────────────────────────────────────────────────
  {
    extern float g_zoom_level;
    if (g_zoom_level != 1.0f) {
      char zoom_text[32];
      snprintf(zoom_text, sizeof(zoom_text), "%.0f%%", g_zoom_level * 100);
      SetBkMode(mem_dc, TRANSPARENT);
      SetTextColor(mem_dc, RGB(200, 200, 200));
      HFONT zf = CreateFontA(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Arial");
      HGDIOBJ old_zf = SelectObject(mem_dc, zf);
      RECT zr = {win_w - 60, win_h - 20, win_w - 5, win_h - 2};
      DrawTextA(mem_dc, zoom_text, -1, &zr, DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX);
      SelectObject(mem_dc, old_zf);
      DeleteObject(zf);
    }
  }

  // ── Scrollbar ─────────────────────────────────────────────────────────────
  int content_h = browser_ui.content_height();
  float total_h = global_layout_root
                    ? global_layout_root->dimensions.content.height
                    : (float)content_h;
  if (total_h > content_h) {
    const int SCROLLBAR_WIDTH = 12;
    int cy = browser_ui.content_y();
    float thumb_h = (std::max)(20.0f, (float)content_h * (float)content_h / total_h);
    float thumb_y = (float)scroll_y * (float)(content_h - thumb_h) / (total_h - (float)content_h);

    RECT track_r = {buffer_width - SCROLLBAR_WIDTH, cy, buffer_width, cy + content_h};
    HBRUSH track_br = CreateSolidBrush(RGB(45, 47, 54));
    ::FillRect(mem_dc, &track_r, track_br);
    DeleteObject(track_br);

    RECT thumb_r = {buffer_width - SCROLLBAR_WIDTH + 2, cy + (int)thumb_y,
                    buffer_width - 2, cy + (int)thumb_y + (int)thumb_h};
    HBRUSH thumb_br = CreateSolidBrush(is_scrolling ? RGB(100, 105, 120) : RGB(75, 78, 88));
    ::FillRect(mem_dc, &thumb_r, thumb_br);
    DeleteObject(thumb_br);
  }
}
