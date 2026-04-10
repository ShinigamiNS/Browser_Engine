// svg_rasterizer.cpp — SVG rasterization via lunasvg
#define LUNASVG_BUILD_STATIC
#include "svg_rasterizer.h"
#include "image_cache.h"
#include <lunasvg.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <iostream>

static std::atomic<int> g_svg_counter(0);

std::string rasterize_svg(const std::string& svg_str, int hint_w, int hint_h) {
    if (svg_str.size() < 20) return "";

    std::unique_ptr<lunasvg::Document> doc;
    try {
        doc = lunasvg::Document::loadFromData(svg_str);
    } catch (...) {
        return "";
    }
    if (!doc) return "";

    int w = hint_w > 0 ? hint_w : (int)doc->width();
    int h = hint_h > 0 ? hint_h : (int)doc->height();

    // Skip SVGs with no intrinsic size (likely CSS-sized containers, not images)
    if (w <= 0 || h <= 0) return "";

    // Scale down very large SVGs instead of skipping them.
    // Anything bigger than 1920x1920 is proportionally reduced.
    if (w > 1920 || h > 1920) {
        float scale = std::min(1920.f / (float)w, 1920.f / (float)h);
        w = std::max(1, (int)(w * scale));
        h = std::max(1, (int)(h * scale));
        std::cerr << "SVG: scaled oversized to " << w << "x" << h << "\n";
    }

    lunasvg::Bitmap bitmap;
    try {
        bitmap = doc->renderToBitmap((uint32_t)w, (uint32_t)h, 0x00000000);
    } catch (...) {
        return "";
    }
    if (!bitmap.valid()) return "";

    // Convert ARGB32 premultiplied → unpremultiplied RGBA (byte order: R G B A)
    bitmap.convertToRGBA();

    uint8_t* px = bitmap.data();
    int npixels = w * h;

    // Pre-blend against white background (GDI StretchDIBits ignores alpha).
    // Then swap R↔B to produce GDI DIB BGRA format.
    for (int i = 0; i < npixels; i++) {
        uint8_t* p = px + i * 4;
        uint8_t a = p[3];
        if (a < 255) {
            // Composite over white: out = src*a + 255*(1-a)
            p[0] = (uint8_t)((p[0] * a + 255 * (255 - a)) / 255);
            p[1] = (uint8_t)((p[1] * a + 255 * (255 - a)) / 255);
            p[2] = (uint8_t)((p[2] * a + 255 * (255 - a)) / 255);
            p[3] = 255;
        }
        // Swap R and B: RGBA → BGRA
        uint8_t tmp = p[0]; p[0] = p[2]; p[2] = tmp;
    }

    CachedImage img;
    img.width  = w;
    img.height = h;
    img.pixels.resize((size_t)npixels);
    std::memcpy(img.pixels.data(), px, (size_t)npixels * 4);

    std::string key = "__svg_" + std::to_string(g_svg_counter.fetch_add(1));

    EnterCriticalSection(&g_image_cache_cs);
    g_image_cache[key] = std::move(img);
    LeaveCriticalSection(&g_image_cache_cs);

    std::cerr << "SVG rasterized: " << key << " (" << w << "x" << h << ")\n";
    return key;
}
