#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include <windows.h>

// GDI+ for image decoding
#include <objidl.h>
#define GDIPVER 0x0110
#include <gdiplus.h>

// ============================================================================
//  Image Cache
// ============================================================================

struct CachedImage {
  std::vector<uint32_t> pixels; // BGRA pixels (GDI DIB format 0x00RRGGBB)
  int width = 0;
  int height = 0;
};

extern std::map<std::string, CachedImage> g_image_cache;
extern CRITICAL_SECTION g_image_cache_cs;
extern ULONG_PTR g_gdip_token;

std::string base64_decode(const std::string &in);
std::string decode_data_uri(const std::string &uri);
bool decode_image_bytes(const std::string &bytes, CachedImage &out);
