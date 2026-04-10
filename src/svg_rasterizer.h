#pragma once
#include <string>

// Rasterize a raw SVG string using lunasvg.
// Stores result in g_image_cache and returns the cache key (e.g. "__svg_0").
// Returns "" on failure.
std::string rasterize_svg(const std::string& svg_str, int hint_w = 0, int hint_h = 0);
