#pragma once

#include <sstream>
#include <stdexcept>
#include <string>

#include "../include/conn.h"

namespace Proxy {
namespace Utils {

HostPort parse_request_target(const char* buffer) {
	std::istringstream ss(buffer);
	std::string request_line;
	std::getline(ss, request_line);
	std::istringstream line_ss(request_line);
	std::string method, target, version;
	line_ss >> method >> target >> version;

	HostPort hp{};

	if (method == "CONNECT") {
		hp.is_connect = true;
		auto colon_pos = target.find(':');
		if (colon_pos == std::string::npos) {
			throw std::runtime_error("Invalid CONNECT target");
		}
		hp.host = target.substr(0, colon_pos);
		hp.port = static_cast<uint16_t>(
		    std::stoi(target.substr(colon_pos + 1)));
	} else {
		hp.is_connect = false;
		std::string prot = "http://";
		if (target.substr(0, prot.size()) != prot) {
			throw std::runtime_error("Unsupported protocol or invalid proxy request");
		}

		std::string url = target.substr(prot.size());
		// return just the host:port part
		auto slash_pos = url.find('/');
		std::string host_port = (slash_pos == std::string::npos) ? url : url.substr(0, slash_pos);

		auto colon_pos = host_port.find(':');
		if (colon_pos != std::string::npos) {
			hp.host = host_port.substr(0, colon_pos);
			hp.port = static_cast<uint16_t>(
			    std::stoi(host_port.substr(colon_pos + 1)));
		} else {
			hp.host = host_port;
			hp.port = 80;
		}
	}

	return hp;
}

} // namespace Utils
} // namespace Proxy