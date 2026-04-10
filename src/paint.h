#pragma once
#include "layout.h"
#include <cstdint>
#include <vector>

enum class DisplayCommandType { SolidColor, Text, ClipPush, ClipPop };

// Gradient type — determines which rasterizer to invoke
enum class GradientType { None, Linear, Radial, Conic };

// One color stop in a CSS gradient (position in [0,1])
struct GradientStop {
  float pos;        // 0.0 = start, 1.0 = end
  uint32_t color;   // 0x00RRGGBB
};

struct DisplayCommand {
  DisplayCommandType type;
  Rect rect;
  uint32_t color; // 0x00RRGGBB format
  std::string text;
  int font_size = 16;
  bool bold = false;
  bool fixed = false;
  float border_radius = 0;  // px, for SolidColor commands
  bool underline = false;   // for Text commands (legacy — also set via text_decoration_line)
  bool italic = false;      // for Text commands
  std::string text_align;   // "left", "center", "right" for Text
  int z_index = 0;          // CSS z-index (0 = auto/default)
  // Per-edge border (drawn over background, only on SolidColor commands)
  float border_top_w = 0, border_right_w = 0, border_bottom_w = 0, border_left_w = 0;
  uint32_t border_top_c = 0, border_right_c = 0, border_bottom_c = 0, border_left_c = 0;
  // CSS background-image URL (blitted on top of background color)
  std::string bg_image_url;
  // background-size: "cover", "contain", "auto", or "Wpx Hpx" / "W% H%"
  std::string bg_size;
  // background-position: "center", "left top", "50% 50%", "10px 20px"
  std::string bg_position;
  // background-repeat: "repeat" (default), "no-repeat", "repeat-x", "repeat-y"
  std::string bg_repeat;
  // CSS outline (drawn outside border box)
  float outline_w = 0;
  uint32_t outline_c = 0;
  float outline_offset = 0;  // CSS outline-offset (px, 0 = default)
  // text-overflow: ellipsis
  bool ellipsis = false;
  // white-space: nowrap (no line wrapping)
  bool nowrap = false;
  // box-shadow (simplified: single shadow)
  float shadow_x = 0, shadow_y = 0, shadow_blur = 0;
  uint32_t shadow_c = 0;
  // CSS opacity (1.0 = fully opaque, 0.0 = invisible)
  float opacity = 1.0f;
  // CSS letter-spacing (px, 0 = normal)
  float letter_spacing = 0.f;
  // CSS transform: translate(x, y) — applied as offset during rendering
  float transform_tx = 0.f, transform_ty = 0.f;
  // CSS transform: scale(sx, sy)
  float transform_sx = 1.f, transform_sy = 1.f;
  // CSS position: sticky
  bool sticky = false;
  float sticky_top = 0.f;    // pinned top value (px) from ancestor sticky element
  float sticky_orig_y = 0.f; // this command's own original document y
  // CSS text-shadow (simplified: single shadow)
  float text_shadow_x = 0.f, text_shadow_y = 0.f, text_shadow_blur = 0.f;
  uint32_t text_shadow_c = 0;
  // CSS font-family (resolved to a Windows font name)
  std::string font_family; // empty = use Arial default
  // CSS gradient: color stops + angle/type
  GradientType gradient_type = GradientType::None;
  std::vector<GradientStop> gradient_stops;
  float gradient_angle = 180.f;    // linear: CSS angle deg (0=up,90=right,180=down)
  float gradient_conic_from = 0.f; // conic: starting angle in degrees
  bool  gradient_radial_ellipse = false; // radial: true=ellipse, false=circle

  // CSS filter functions applied after painting this box
  bool  has_filter        = false;
  float filter_blur       = 0.f;   // blur(Npx)
  float filter_brightness = 1.f;   // brightness(N), 1=normal
  float filter_contrast   = 1.f;   // contrast(N),   1=normal
  float filter_grayscale  = 0.f;   // grayscale(N),  0-1
  float filter_sepia      = 0.f;   // sepia(N),      0-1

  // CSS text-decoration longhands
  std::string text_decoration_line;   // "underline","overline","line-through" (space-separated)
  uint32_t    text_decoration_color = 0; // 0 = inherit text color
  std::string text_decoration_style;  // "solid","dashed","dotted","double","wavy"

  // Per-element scroll (overflow:auto/scroll clips)
  bool clip_scrollable = false;        // true for overflow:auto/scroll ClipPush
  float clip_content_height = 0.f;     // total height of children content
  // Per-axis overflow clipping (both true = normal clip, one false = clip only that axis)
  bool clip_overflow_x = true;
  bool clip_overflow_y = true;
};

// A Display List represents the sequence of drawing operations needed to render
// the frame
using DisplayList = std::vector<DisplayCommand>;

uint32_t parse_color(const std::string &color_str);
DisplayList build_display_list(const std::shared_ptr<LayoutBox> &root);
