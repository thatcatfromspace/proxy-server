#pragma once

#include <vector>

#include "../include/conn.h"

namespace Proxy {
namespace Utils {

// Parse the request target from an HTTP request buffer.
// Returns a HostPort struct; check hp.error before using.
HostPort parse_request_target(const char* buffer);

// Rewrite an absolute-URI HTTP request to relative-URI form for origin forwarding.
// e.g. "GET http://example.com/path HTTP/1.1" -> "GET /path HTTP/1.1"
std::vector<char> rewrite_request(const char* buffer, ssize_t len);

} // namespace Utils
} // namespace Proxy
