#include <vector>

namespace Proxy {
struct Connection {
	int fd;
	bool closed;

	std::vector<char> write_buf = {};

	Connection* peer = nullptr;
};
} // namespace Proxy