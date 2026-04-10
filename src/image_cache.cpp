#include "image_cache.h"
#include <cstdint>
#include <string>
#include <vector>

// ============================================================================
//  Global image cache state
// ============================================================================

std::map<std::string, CachedImage> g_image_cache;
CRITICAL_SECTION g_image_cache_cs;
ULONG_PTR g_gdip_token = 0;

// ============================================================================
//  MemStream — simple read-only IStream over a byte buffer
// ============================================================================

struct MemStream : public IStream {
  const uint8_t *data;
  SIZE_T size;
  SIZE_T pos;
  LONG ref_count;

  MemStream(const void *d, SIZE_T s)
      : data((const uint8_t *)d), size(s), pos(0), ref_count(1) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **) {
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() { return ++ref_count; }
  ULONG STDMETHODCALLTYPE Release() {
    if (--ref_count == 0) {
      delete this;
      return 0;
    }
    return ref_count;
  }

  HRESULT STDMETHODCALLTYPE Read(void *pv, ULONG cb, ULONG *pcbRead) {
    ULONG avail = (ULONG)(size - pos);
    ULONG n = cb < avail ? cb : avail;
    memcpy(pv, data + pos, n);
    pos += n;
    if (pcbRead)
      *pcbRead = n;
    return (n == cb) ? S_OK : S_FALSE;
  }
  HRESULT STDMETHODCALLTYPE Write(const void *, ULONG, ULONG *) {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER dlibMove, DWORD dwOrigin,
                                 ULARGE_INTEGER *plibNewPos) {
    LONGLONG newpos;
    if (dwOrigin == STREAM_SEEK_SET)
      newpos = dlibMove.QuadPart;
    else if (dwOrigin == STREAM_SEEK_CUR)
      newpos = (LONGLONG)pos + dlibMove.QuadPart;
    else if (dwOrigin == STREAM_SEEK_END)
      newpos = (LONGLONG)size + dlibMove.QuadPart;
    else
      return STG_E_INVALIDFUNCTION;
    if (newpos < 0 || (SIZE_T)newpos > size)
      return STG_E_INVALIDFUNCTION;
    pos = (SIZE_T)newpos;
    if (plibNewPos)
      plibNewPos->QuadPart = pos;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE CopyTo(IStream *, ULARGE_INTEGER, ULARGE_INTEGER *,
                                   ULARGE_INTEGER *) {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE Commit(DWORD) { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE Revert() { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER,
                                         DWORD) {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE Stat(STATSTG *s, DWORD) {
    if (s) {
      memset(s, 0, sizeof(*s));
      s->cbSize.QuadPart = size;
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Clone(IStream **) { return E_NOTIMPL; }
};

// ============================================================================
//  Image decoding
// ============================================================================

bool decode_image_bytes(const std::string &bytes, CachedImage &out) {
  if (bytes.empty())
    return false;

  IStream *stream = new MemStream(bytes.data(), bytes.size());

  Gdiplus::Bitmap *bmp = Gdiplus::Bitmap::FromStream(stream);
  stream->Release();

  if (!bmp || bmp->GetLastStatus() != Gdiplus::Ok) {
    delete bmp;
    return false;
  }

  int w = (int)bmp->GetWidth();
  int h = (int)bmp->GetHeight();
  if (w <= 0 || h <= 0 || w > 8192 || h > 8192) {
    delete bmp;
    return false;
  }

  out.width = w;
  out.height = h;
  out.pixels.resize(w * h);

  Gdiplus::Rect rc(0, 0, w, h);
  Gdiplus::BitmapData data;
  if (bmp->LockBits(&rc, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB,
                    &data) == Gdiplus::Ok) {
    uint8_t *src_row = (uint8_t *)data.Scan0;
    for (int y = 0; y < h; y++) {
      uint32_t *src = (uint32_t *)(src_row + y * data.Stride);
      uint32_t *dst = out.pixels.data() + y * w;
      for (int x = 0; x < w; x++) {
        uint32_t px = src[x];
        uint8_t a = (px >> 24) & 0xFF;
        uint8_t r = (px >> 16) & 0xFF;
        uint8_t g = (px >> 8) & 0xFF;
        uint8_t b = px & 0xFF;
        // Composite over white for semi-transparent pixels
        r = (uint8_t)(r + (255 - a) * (255 - r) / 255);
        g = (uint8_t)(g + (255 - a) * (255 - g) / 255);
        b = (uint8_t)(b + (255 - a) * (255 - b) / 255);
        dst[x] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
      }
    }
    bmp->UnlockBits(&data);
  }
  delete bmp;
  return !out.pixels.empty();
}

// ============================================================================
//  Base64 / data URI decoding
// ============================================================================

std::string base64_decode(const std::string &in) {
  static const int T[256] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
    52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
    15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
    -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
    41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
  };
  std::string out;
  out.reserve(in.size() * 3 / 4);
  int val = 0, bits = -8;
  for (unsigned char c : in) {
    if (T[c] == -1) continue;
    val = (val << 6) + T[c];
    bits += 6;
    if (bits >= 0) {
      out.push_back((char)((val >> bits) & 0xFF));
      bits -= 8;
    }
  }
  return out;
}

std::string decode_data_uri(const std::string &uri) {
  if (uri.size() < 5 || uri.substr(0, 5) != "data:") return "";
  size_t comma = uri.find(',');
  if (comma == std::string::npos) return "";
  std::string header = uri.substr(5, comma - 5);
  std::string payload = uri.substr(comma + 1);
  if (header.find("base64") != std::string::npos) {
    return base64_decode(payload);
  }
  return payload;
}
