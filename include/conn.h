#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Proxy {
struct Connection {
	int fd;
	bool closed;
	std::vector<char> write_buf = {};
	Connection* peer = nullptr;

	bool is_upstream = false;
	bool connect_in_progress = false;
	bool is_connect_method = false;
};

struct HostPort {
	std::string host;
	uint16_t port;
	bool is_connect = false;
};

} // namespace Proxy