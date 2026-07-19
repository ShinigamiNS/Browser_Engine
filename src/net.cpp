#include "net.h"
#include <iostream>
#include <windows.h>
#include <wininet.h>

// MinGW's wininet.h omits several TLS/certificate error constants. Define any
// that are missing (values are stable per the Windows SDK).
#ifndef ERROR_INTERNET_SEC_CERT_REV_FAILED
#define ERROR_INTERNET_SEC_CERT_REV_FAILED 12057
#endif
#ifndef ERROR_INTERNET_SEC_CERT_ERRORS
#define ERROR_INTERNET_SEC_CERT_ERRORS 12055
#endif
#ifndef ERROR_INTERNET_SEC_INVALID_CERT
#define ERROR_INTERNET_SEC_INVALID_CERT 12169
#endif
#ifndef ERROR_INTERNET_SEC_CERT_REVOKED
#define ERROR_INTERNET_SEC_CERT_REVOKED 12170
#endif
#ifndef ERROR_INTERNET_SEC_CERT_NO_REV
#define ERROR_INTERNET_SEC_CERT_NO_REV 12056
#endif
#ifndef ERROR_INTERNET_DECODING_FAILED
#define ERROR_INTERNET_DECODING_FAILED 12175
#endif
#ifndef ERROR_INTERNET_SEC_CERT_CN_INVALID
#define ERROR_INTERNET_SEC_CERT_CN_INVALID 12038
#endif
#ifndef ERROR_INTERNET_SEC_CERT_DATE_INVALID
#define ERROR_INTERNET_SEC_CERT_DATE_INVALID 12037
#endif
#ifndef ERROR_INTERNET_INVALID_CA
#define ERROR_INTERNET_INVALID_CA 12045
#endif

// Incognito: when set, fetches suppress cookies and never write to cache.
bool g_private_mode = false;

static const char *USER_AGENT =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, "
    "like Gecko) Chrome/120.0.0.0 Safari/537.36";

static const size_t MAX_DOWNLOAD_SIZE = 8 * 1024 * 1024; // 8MB cap

// Map a WinINet error code to a coarse, user-facing error kind. TLS/certificate
// failures are called out specifically so the loader can show a distinct
// "your connection is not private" interstitial instead of a generic error.
static std::string classify_wininet_error(DWORD err) {
  switch (err) {
  case ERROR_INTERNET_SEC_CERT_CN_INVALID:
  case ERROR_INTERNET_SEC_CERT_DATE_INVALID:
  case ERROR_INTERNET_SEC_CERT_REV_FAILED:
  case ERROR_INTERNET_SEC_CERT_ERRORS:
  case ERROR_INTERNET_INVALID_CA:
  case ERROR_INTERNET_SEC_INVALID_CERT:
  case ERROR_INTERNET_SEC_CERT_REVOKED:
  case ERROR_INTERNET_SEC_CERT_NO_REV:
  case ERROR_INTERNET_DECODING_FAILED:
    return "cert";
  case ERROR_INTERNET_NAME_NOT_RESOLVED:
    return "dns";
  case ERROR_INTERNET_CANNOT_CONNECT:
  case ERROR_INTERNET_CONNECTION_RESET:
  case ERROR_INTERNET_CONNECTION_ABORTED:
    return "connect";
  case ERROR_INTERNET_TIMEOUT:
    return "timeout";
  case ERROR_INTERNET_INVALID_URL:
  case ERROR_INTERNET_UNRECOGNIZED_SCHEME:
    return "protocol";
  default:
    return "other";
  }
}

// Read the final effective URL from a request handle (reflects redirects).
static std::string query_final_url(HINTERNET h) {
  char buf[4096];
  DWORD len = sizeof(buf);
  if (InternetQueryOptionA(h, INTERNET_OPTION_URL, buf, &len) && len > 0)
    return std::string(buf, len);
  return "";
}

// Read the numeric HTTP status code (200, 404, 503, ...) from a handle.
static long query_status_code(HINTERNET h) {
  DWORD code = 0;
  DWORD len = sizeof(code);
  DWORD idx = 0;
  if (HttpQueryInfoA(h, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &code,
                     &len, &idx))
    return (long)code;
  return 0;
}

HttpResponse http_fetch(const std::string &url, const std::string &method,
                        const std::string &body, bool private_mode) {
  HttpResponse resp;
  resp.final_url = url;

  bool is_https = (url.length() >= 8 && url.substr(0, 8) == "https://");
  bool is_http = (url.length() >= 7 && url.substr(0, 7) == "http://");
  if (!is_https && !is_http) {
    // Only http(s) is fetched over the network. Any other scheme reaching
    // here (file:, data:, javascript:, chrome:, etc.) is rejected — the
    // caller decides how to handle those locally.
    std::cerr << "http_fetch: rejected non-http scheme: " << url << "\n";
    resp.error = "protocol";
    return resp;
  }

  private_mode = private_mode || g_private_mode;

  HINTERNET hInternet =
      InternetOpenA(USER_AGENT, INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
  if (!hInternet) {
    resp.error = "other";
    return resp;
  }
  // Reasonable timeouts so a dead host doesn't hang the load thread forever.
  DWORD timeout = 20000; // 20s
  InternetSetOptionA(hInternet, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout,
                     sizeof(timeout));
  InternetSetOptionA(hInternet, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout,
                     sizeof(timeout));
  InternetSetOptionA(hInternet, INTERNET_OPTION_SEND_TIMEOUT, &timeout,
                     sizeof(timeout));

  // Security-relevant flags:
  //  - NO IGNORE_CERT_* flags: TLS certificate validation stays STRICT.
  //    A bad/expired/self-signed cert makes the request fail (surfaced as
  //    error="cert"), exactly like a real browser blocking the page.
  //  - NO_UI: never pop a system credential/cert dialog; fail cleanly.
  //  - Private mode: NO_COOKIES (don't send or store cookies) and
  //    NO_CACHE_WRITE (don't persist anything to disk).
  DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_UI;
  if (is_https) flags |= INTERNET_FLAG_SECURE;
  if (private_mode)
    flags |= INTERNET_FLAG_NO_COOKIES | INTERNET_FLAG_NO_CACHE_WRITE;
  else
    flags |= INTERNET_FLAG_NO_CACHE_WRITE;

  HINTERNET hResource = NULL;

  if (method == "POST") {
    // POST needs the connect/request path so we can attach a body.
    char host[256] = {0}, path[4096] = {0}, extra[4096] = {0};
    URL_COMPONENTSA uc = {};
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = host;   uc.dwHostNameLength = sizeof(host);
    uc.lpszUrlPath = path;    uc.dwUrlPathLength = sizeof(path);
    uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = sizeof(extra);
    if (!InternetCrackUrlA(url.c_str(), 0, 0, &uc)) {
      resp.error = "protocol";
      InternetCloseHandle(hInternet);
      return resp;
    }
    std::string object = std::string(path) + std::string(extra);
    if (object.empty()) object = "/";

    HINTERNET hConnect = InternetConnectA(hInternet, host, uc.nPort, NULL, NULL,
                                          INTERNET_SERVICE_HTTP, 0, 0);
    if (!hConnect) {
      resp.error = classify_wininet_error(GetLastError());
      InternetCloseHandle(hInternet);
      return resp;
    }
    HINTERNET hReq = HttpOpenRequestA(hConnect, "POST", object.c_str(), NULL,
                                      NULL, NULL, flags, 0);
    if (!hReq) {
      resp.error = classify_wininet_error(GetLastError());
      InternetCloseHandle(hConnect);
      InternetCloseHandle(hInternet);
      return resp;
    }
    static const char hdr[] =
        "Content-Type: application/x-www-form-urlencoded\r\n";
    if (!HttpSendRequestA(hReq, hdr, (DWORD)(sizeof(hdr) - 1),
                          (LPVOID)body.data(), (DWORD)body.size())) {
      resp.error = classify_wininet_error(GetLastError());
      InternetCloseHandle(hReq);
      InternetCloseHandle(hConnect);
      InternetCloseHandle(hInternet);
      return resp;
    }
    resp.status_code = query_status_code(hReq);
    std::string fu = query_final_url(hReq);
    if (!fu.empty()) resp.final_url = fu;
    // Note: hConnect must outlive reads; close after.
    char buffer[8192];
    DWORD n = 0;
    while (InternetReadFile(hReq, buffer, sizeof(buffer), &n) && n > 0) {
      resp.body.append(buffer, n);
      if (resp.body.size() > MAX_DOWNLOAD_SIZE) break;
    }
    resp.ok = true;
    InternetCloseHandle(hReq);
    InternetCloseHandle(hConnect);
    InternetCloseHandle(hInternet);
    return resp;
  }

  // GET (and everything else) — InternetOpenUrl follows redirects for us.
  hResource = InternetOpenUrlA(hInternet, url.c_str(), NULL, 0, flags, 0);
  if (!hResource) {
    DWORD err = GetLastError();
    resp.error = classify_wininet_error(err);
    std::cerr << "http_fetch: failed (err " << err << ", kind " << resp.error
              << ") for " << url << "\n";
    InternetCloseHandle(hInternet);
    return resp;
  }

  resp.status_code = query_status_code(hResource);
  std::string fu = query_final_url(hResource);
  if (!fu.empty()) resp.final_url = fu;

  char buffer[8192];
  DWORD n = 0;
  while (InternetReadFile(hResource, buffer, sizeof(buffer), &n) && n > 0) {
    resp.body.append(buffer, n);
    if (resp.body.size() > MAX_DOWNLOAD_SIZE) {
      std::cerr << "http_fetch: truncated at " << MAX_DOWNLOAD_SIZE / 1024
                << "KB for " << url << "\n";
      break;
    }
  }
  resp.ok = true;

  InternetCloseHandle(hResource);
  InternetCloseHandle(hInternet);
  return resp;
}

// ── Back-compat wrappers ─────────────────────────────────────────────────────

std::string fetch_https(const std::string &url) {
  return http_fetch(url, "GET", "", g_private_mode).body;
}

std::string fetch_http_post(const std::string &url, const std::string &body) {
  return http_fetch(url, "POST", body, g_private_mode).body;
}

int clear_browsing_data() {
  // Drop session cookies for the current process/user.
  InternetSetOptionA(NULL, INTERNET_OPTION_END_BROWSER_SESSION, NULL, 0);

  // Enumerate and delete every WinINet cache entry.
  int deleted = 0;
  DWORD size = 0;
  FindFirstUrlCacheEntryA(NULL, NULL, &size);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) return 0;
  std::string buf(size, '\0');
  INTERNET_CACHE_ENTRY_INFOA *info =
      reinterpret_cast<INTERNET_CACHE_ENTRY_INFOA *>(&buf[0]);
  info->dwStructSize = size;
  HANDLE h = FindFirstUrlCacheEntryA(NULL, info, &size);
  if (h == NULL) return 0;
  do {
    // Copy the name before deleting (delete may invalidate the buffer).
    std::string name = info->lpszSourceUrlName ? info->lpszSourceUrlName : "";
    if (!name.empty() && DeleteUrlCacheEntryA(name.c_str())) deleted++;

    size = (DWORD)buf.size();
    info->dwStructSize = size;
    if (!FindNextUrlCacheEntryA(h, info, &size)) {
      if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
        buf.resize(size);
        info = reinterpret_cast<INTERNET_CACHE_ENTRY_INFOA *>(&buf[0]);
        info->dwStructSize = size;
        if (!FindNextUrlCacheEntryA(h, info, &size)) break;
      } else {
        break;
      }
    }
  } while (true);
  FindCloseUrlCache(h);
  return deleted;
}
