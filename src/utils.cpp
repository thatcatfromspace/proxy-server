#pragma once

#include <sstream>
#include <string>
#include <stdexcept>

#include "../include/conn.h"

namespace Proxy {
namespace Utils {

HostPort parse_connect_target(const char* buffer) {
    std::istringstream ss(buffer);
    std::string request_line;

    // Read first line: "CONNECT host:port HTTP/1.1"
    std::getline(ss, request_line);

    std::istringstream line_ss(request_line);
    std::string method, target, version;

    line_ss >> method >> target >> version;

    if (method != "CONNECT") {
        throw std::runtime_error("Not a CONNECT request");
    }

    auto colon_pos = target.find(':');
    if (colon_pos == std::string::npos) {
        throw std::runtime_error("Invalid CONNECT target");
    }

    HostPort hp;
    hp.host = target.substr(0, colon_pos);
    hp.port = static_cast<uint16_t>(
        std::stoi(target.substr(colon_pos + 1))
    );
    
    return hp;
}

} // namespace Utils
} // namespace Proxy