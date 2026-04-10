#include "font_metrics.h"
#include <vector>

FontMetrics& FontMetrics::get_instance() {
    static FontMetrics instance;
    return instance;
}

FontMetrics::FontMetrics() {
    hdc = CreateCompatibleDC(NULL);
}

FontMetrics::~FontMetrics() {
    for (auto& pair : font_cache) {
        DeleteObject(pair.second);
    }
    DeleteDC(hdc);
}

HFONT FontMetrics::get_font(int size, bool bold, bool italic) {
    FontKey key{size, bold, italic};
    auto it = font_cache.find(key);
    if (it != font_cache.end()) return it->second;

    HFONT hFont = CreateFontA(
        size, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, italic ? TRUE : FALSE,
        FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH,
        "Arial");

    font_cache[key] = hFont;
    return hFont;
}

TextMetrics FontMetrics::measure_text(const std::string& text, int font_size, bool bold,
                                      float letter_spacing, bool italic) {
    if (text.empty()) return {0, (float)font_size};

    HFONT hFont = get_font(font_size, bold, italic);
    HGDIOBJ old_font = SelectObject(hdc, hFont);

    int wlen = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.length(), NULL, 0);
    std::wstring wstr(wlen, 0);
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.length(), &wstr[0], wlen);

    SIZE size;
    GetTextExtentPoint32W(hdc, wstr.c_str(), (int)wstr.length(), &size);

    SelectObject(hdc, old_font);

    float w = (float)size.cx;
    if (letter_spacing != 0.f && wlen > 1)
        w += letter_spacing * (wlen - 1);
    return {w, (float)size.cy};
}
