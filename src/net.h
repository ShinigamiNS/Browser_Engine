#pragma once
#include <string>

// ── Rich HTTP response ────────────────────────────────────────────────────────
struct HttpResponse {
  std::string body;        // response payload (empty on transport failure)
  std::string final_url;   // URL after following redirects
  long        status_code = 0; // HTTP status (0 = transport never completed)
  std::string error;       // transport error kind: "", "cert", "dns",
                           // "connect", "timeout", "protocol", "other"
  bool        ok = false;  // a response body was received
};

// Fetch a URL over http/https with strict TLS certificate validation.
// method: "GET" or "POST". body: request body for POST (x-www-form-urlencoded).
// private_mode: suppress cookies and caching (incognito).
HttpResponse http_fetch(const std::string &url,
                        const std::string &method = "GET",
                        const std::string &body = std::string(),
                        bool private_mode = false);

// ── Back-compat thin wrappers (return body only) ─────────────────────────────
std::string fetch_https(const std::string &url);
std::string fetch_http_post(const std::string &url, const std::string &body);

// Global incognito flag — when true, all fetches suppress cookies/cache.
extern bool g_private_mode;

// Purge WinINet's on-disk cache and session cookies for this user.
// Returns the number of cache entries deleted.
int clear_browsing_data();
