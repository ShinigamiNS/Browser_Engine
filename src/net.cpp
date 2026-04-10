#include "net.h"
#include <iostream>
#include <windows.h>
#include <wininet.h>

std::string fetch_https(const std::string &url) {
  // Accept https:// and http:// (some image CDNs still use plain HTTP)
  bool is_https = (url.length() >= 8 && url.substr(0, 8) == "https://");
  bool is_http  = (url.length() >= 7 && url.substr(0, 7) == "http://");
  if (!is_https && !is_http) {
    std::cerr << "fetch_https: unsupported protocol, rejected: " << url << "\n";
    return "";
  }

  HINTERNET hInternet = InternetOpenA(
      "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, "
      "like Gecko) Chrome/120.0.0.0 Safari/537.36",
      INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
  if (!hInternet) {
    std::cerr << "InternetOpen failed.\n";
    return "";
  }

  // Only set INTERNET_FLAG_SECURE for HTTPS connections
  DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE;
  if (is_https) flags |= INTERNET_FLAG_SECURE;
  HINTERNET hConnect =
      InternetOpenUrlA(hInternet, url.c_str(), NULL, 0, flags, 0);
  if (!hConnect) {
    std::cerr << "InternetOpenUrl failed for URL: " << url << "\n";
    InternetCloseHandle(hInternet);
    return "";
  }

  std::string result;
  char buffer[4096];
  DWORD bytesRead = 0;

  // Limit download to 2MB to handle modern pages (google.com is ~400-600KB)
  static const size_t MAX_DOWNLOAD_SIZE = 2 * 1024 * 1024;

  while (InternetReadFile(hConnect, buffer, sizeof(buffer), &bytesRead) &&
         bytesRead > 0) {
    result.append(buffer, bytesRead);
    if (result.size() > MAX_DOWNLOAD_SIZE) {
      std::cerr << "Download truncated at " << MAX_DOWNLOAD_SIZE / 1024
                << "KB for URL: " << url << "\n";
      break;
    }
  }

  InternetCloseHandle(hConnect);
  InternetCloseHandle(hInternet);

  return result;
}
