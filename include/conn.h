#pragma once

#include <vector>
#include <string>
#include <cstdint>

namespace Proxy {
struct Connection {
	int fd;
	bool closed;
	std::vector<char> write_buf = {};
	Connection* peer = nullptr;

	bool is_upstream = false;
	bool connect_in_progress = false;
};

struct HostPort {
	std::string host;
	uint16_t port;
};

} // namespace Proxy