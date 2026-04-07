#include "utils.h"

#include <algorithm>
#include <cstring>
#include <sstream>

namespace Proxy {
namespace Utils {

// Extract "Host" header value from raw HTTP request buffer.
static std::string extract_host_header(const char* buffer) {
	std::istringstream ss(buffer);
	std::string line;
	// Skip request line
	std::getline(ss, line);
	while (std::getline(ss, line)) {
		// End of headers
		if (line.empty() || line == "\r")
			break;
		// Case-insensitive match for "Host:"
		if (line.size() > 5 &&
		    (line[0] == 'H' || line[0] == 'h') &&
		    (line[1] == 'O' || line[1] == 'o') &&
		    (line[2] == 'S' || line[2] == 's') &&
		    (line[3] == 'T' || line[3] == 't') &&
		    line[4] == ':') {
			std::string value = line.substr(5);
			// Trim leading whitespace and trailing \r
			size_t start = value.find_first_not_of(" \t");
			if (start == std::string::npos)
				return "";
			value = value.substr(start);
			if (!value.empty() && value.back() == '\r')
				value.pop_back();
			return value;
		}
	}
	return "";
}

HostPort parse_request_target(const char* buffer) {
	std::istringstream ss(buffer);
	std::string request_line;
	std::getline(ss, request_line);
	std::istringstream line_ss(request_line);
	std::string method, target, version;
	line_ss >> method >> target >> version;

	HostPort hp{};

	if (method.empty() || target.empty()) {
		hp.error = true;
		hp.error_msg = "Malformed request line";
		return hp;
	}

	if (method == "CONNECT") {
		hp.is_connect = true;
		auto colon_pos = target.find(':');
		if (colon_pos == std::string::npos) {
			hp.error = true;
			hp.error_msg = "Invalid CONNECT target: " + target;
			return hp;
		}
		hp.host = target.substr(0, colon_pos);
		try {
			hp.port = static_cast<uint16_t>(
			    std::stoi(target.substr(colon_pos + 1)));
		} catch (...) {
			hp.error = true;
			hp.error_msg = "Invalid port in CONNECT target: " + target;
			return hp;
		}
	} else {
		hp.is_connect = false;
		std::string prot = "http://";

		// Check for absolute-URI form (http://host/path)
		if (target.size() >= prot.size() &&
		    target.substr(0, prot.size()) == prot) {
			std::string url = target.substr(prot.size());
			auto slash_pos = url.find('/');
			std::string host_port =
			    (slash_pos == std::string::npos) ? url : url.substr(0, slash_pos);

			auto colon_pos = host_port.find(':');
			if (colon_pos != std::string::npos) {
				hp.host = host_port.substr(0, colon_pos);
				try {
					hp.port = static_cast<uint16_t>(
					    std::stoi(host_port.substr(colon_pos + 1)));
				} catch (...) {
					hp.error = true;
					hp.error_msg = "Invalid port in URL: " + target;
					return hp;
				}
			} else {
				hp.host = host_port;
				hp.port = 80;
			}
		}
		// Relative URI form — fall back to Host header
		else {
			std::string host_value = extract_host_header(buffer);
			if (host_value.empty()) {
				hp.error = true;
				hp.error_msg = "Relative URI with no Host header";
				return hp;
			}
			auto colon_pos = host_value.find(':');
			if (colon_pos != std::string::npos) {
				hp.host = host_value.substr(0, colon_pos);
				try {
					hp.port = static_cast<uint16_t>(
					    std::stoi(host_value.substr(colon_pos + 1)));
				} catch (...) {
					hp.error = true;
					hp.error_msg = "Invalid port in Host header: " + host_value;
					return hp;
				}
			} else {
				hp.host = host_value;
				hp.port = 80;
			}
		}
	}

	return hp;
}

std::vector<char> rewrite_request(const char* buffer, ssize_t len) {
	// For plain HTTP requests, rewrite absolute-URI to relative form.
	// "GET http://example.com/path HTTP/1.1" -> "GET /path HTTP/1.1"
	// The rest of the request (headers + body) is left untouched.
	std::string prot = "http://";
	std::string buf_str(buffer, len);

	// Find end of request line
	auto crlf_pos = buf_str.find("\r\n");
	if (crlf_pos == std::string::npos) {
		// No CRLF — just return as-is
		return std::vector<char>(buffer, buffer + len);
	}

	std::string request_line = buf_str.substr(0, crlf_pos);
	std::string rest = buf_str.substr(crlf_pos); // includes the \r\n

	// Find the absolute URI
	auto prot_pos = request_line.find(prot);
	if (prot_pos == std::string::npos) {
		// Not an absolute URI, return as-is
		return std::vector<char>(buffer, buffer + len);
	}

	// Find the path after the host
	auto host_start = prot_pos + prot.size();
	auto slash_pos = request_line.find('/', host_start);

	std::string new_request_line;
	if (slash_pos != std::string::npos) {
		// "GET " + "/path HTTP/1.1"
		new_request_line = request_line.substr(0, prot_pos) +
		                   request_line.substr(slash_pos);
	} else {
		// No path, e.g. "GET http://example.com HTTP/1.1"
		// Find the space before HTTP version
		auto space_pos = request_line.find(' ', host_start);
		if (space_pos != std::string::npos) {
			new_request_line = request_line.substr(0, prot_pos) +
			                   "/" + request_line.substr(space_pos);
		} else {
			// Malformed, return as-is
			return std::vector<char>(buffer, buffer + len);
		}
	}

	std::string result = new_request_line + rest;
	return std::vector<char>(result.begin(), result.end());
}

} // namespace Utils
} // namespace Proxy