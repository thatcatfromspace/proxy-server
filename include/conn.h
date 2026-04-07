#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Proxy {
struct Connection {
	int fd = -1;
	bool closed = false;
	std::vector<char> write_buf = {};
	Connection* peer = nullptr;

	bool is_upstream = false;
	bool connect_in_progress = false;
	bool is_connect_method = false;
};

struct HostPort {
	std::string host;
	uint16_t port = 80;
	bool is_connect = false;
	bool error = false;
	std::string error_msg;
};

} // namespace Proxy