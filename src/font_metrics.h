#pragma once
#include <string>
#define NOMINMAX
#include <windows.h>
#include <map>

struct TextMetrics {
    float width;
    float height;
};

class FontMetrics {
public:
    static FontMetrics& get_instance();

    TextMetrics measure_text(const std::string& text, int font_size, bool bold,
                              float letter_spacing = 0.f, bool italic = false);

private:
    FontMetrics();
    ~FontMetrics();

    struct FontKey { int size; bool bold; bool italic;
      bool operator<(const FontKey &o) const {
        if (size != o.size) return size < o.size;
        if (bold != o.bold) return bold < o.bold;
        return italic < o.italic;
      }
    };
    HFONT get_font(int size, bool bold, bool italic = false);

    std::map<FontKey, HFONT> font_cache;
    HDC hdc;
};
