#pragma once
#include <string>

// Fetches a resource over HTTPS.
// Returns an empty string on failure or if the URL is not HTTPS.
std::string fetch_https(const std::string &url);
