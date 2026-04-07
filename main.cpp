#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <iostream>
#include <vector>

#include "include/conn.h"
#include "src/utils.h"

constexpr int PORT = 8080;
constexpr int MAX_EVENTS = 64;
constexpr int BUF_SIZE = 4096;

const char* OK = "HTTP/1.1 200 Connection Established\r\n\r\n";
const char* BAD_REQUEST = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
const char* BAD_GATEWAY = "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";

using Proxy::Connection;

int set_nonblocking(int fd) {
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags == -1)
		return -1;
	return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// Send an error response to the client and mark it for closure.
static void send_error_and_close(int epfd, Connection* conn, const char* response) {
	conn->write_buf.insert(conn->write_buf.end(), response, response + strlen(response));
	conn->closed = true;

	epoll_event ev{};
	ev.events = EPOLLOUT;
	ev.data.ptr = conn;
	epoll_ctl(epfd, EPOLL_CTL_MOD, conn->fd, &ev);
}

// Cleanly close a connection and its peer (if any).
// Removes both from epoll, closes fds, and frees memory.
static void cleanup_connection(int epfd, Connection* conn) {
	if (!conn)
		return;

	Connection* peer = conn->peer;

	// Unlink peers
	if (peer) {
		peer->peer = nullptr;
	}

	// Remove and close this connection
	epoll_ctl(epfd, EPOLL_CTL_DEL, conn->fd, nullptr);
	close(conn->fd);
	delete conn;

	// Also close the peer
	if (peer) {
		peer->closed = true;
		epoll_ctl(epfd, EPOLL_CTL_DEL, peer->fd, nullptr);
		close(peer->fd);
		delete peer;
	}
}

int main() {
	// listening socket
	int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (listen_fd < 0) {
		perror("socket");
		return 1;
	}

	int opt = 1;
	setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(PORT);

	if (bind(listen_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
		perror("bind");
		return 1;
	}

	if (listen(listen_fd, 128) < 0) {
		perror("listen");
		return 1;
	}

	set_nonblocking(listen_fd);

	// create epoll instance
	int epfd = epoll_create1(0);
	if (epfd < 0) {
		perror("epoll_create1");
		return 1;
	}

	epoll_event ev{};
	ev.events = EPOLLIN;
	ev.data.fd = listen_fd;
	epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

	epoll_event events[MAX_EVENTS];
	char buffer[BUF_SIZE];

	std::cout << "Server listening on port " << PORT << "\n";

	// event loop
	while (true) {
		int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			perror("epoll_wait");
			break;
		}

		for (int i = 0; i < n; i++) {
			// listening socket event
			if (events[i].data.fd == listen_fd) {
				while (true) {
					sockaddr_in client{};
					socklen_t len = sizeof(client);
					int client_fd = accept(listen_fd, (sockaddr*)&client, &len);

					if (client_fd < 0) {
						if (errno == EAGAIN || errno == EWOULDBLOCK)
							break;
						perror("accept");
						break;
					}

					set_nonblocking(client_fd);

					Connection* conn = new Connection{};
					conn->fd = client_fd;

					epoll_event cev{};
					cev.events = EPOLLIN;
					cev.data.ptr = conn;

					epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &cev);
				}
				continue;
			}

			// client/upstream socket event
			Connection* conn =
			    static_cast<Connection*>(events[i].data.ptr);
			int fd = conn->fd;

			// drain write buffer if EPOLLOUT is set
			if (events[i].events & EPOLLOUT) {
				// check for in-progress connection with server
				if (conn->is_upstream && conn->connect_in_progress) {
					int err = 0;
					socklen_t len = sizeof(err);

					if (getsockopt(conn->fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
						// connect failed — send 502 to client
						conn->connect_in_progress = false;
						if (conn->peer) {
							send_error_and_close(epfd, conn->peer, BAD_GATEWAY);
							conn->peer->peer = nullptr;
							conn->peer = nullptr;
						}
						conn->closed = true;
						epoll_ctl(epfd, EPOLL_CTL_DEL, conn->fd, nullptr);
						close(conn->fd);
						delete conn;
						continue;
					}

					// connect succeeded
					conn->connect_in_progress = false;

					// send 200 Connection Established to client ONLY for CONNECT tunnels
					if (conn->peer->is_connect_method) {
						conn->peer->write_buf.insert(
						    conn->peer->write_buf.end(), OK, OK + strlen(OK));

						// enable EPOLLOUT on client to flush 200
						epoll_event cev{};
						cev.events = EPOLLIN | EPOLLOUT;
						cev.data.ptr = conn->peer;
						epoll_ctl(epfd, EPOLL_CTL_MOD, conn->peer->fd, &cev);
					} else {
						// for standard HTTP, just ensure client is watching for input
						epoll_event cev{};
						cev.events = EPOLLIN;
						cev.data.ptr = conn->peer;
						epoll_ctl(epfd, EPOLL_CTL_MOD, conn->peer->fd, &cev);
					}

					// now enable EPOLLIN on upstream
					epoll_event uev{};
					uev.events = EPOLLIN;
					uev.data.ptr = conn;
					epoll_ctl(epfd, EPOLL_CTL_MOD, conn->fd, &uev);
				}

				while (!conn->write_buf.empty()) {
					ssize_t w = write(fd, conn->write_buf.data(), conn->write_buf.size());

					if (w > 0) {
						conn->write_buf.erase(conn->write_buf.begin(), conn->write_buf.begin() + w);
					} else if (w == 0 || errno == EAGAIN || errno == EWOULDBLOCK) {
						// write returned 0 or would block — try again later
						break;
					} else {
						perror("write");
						conn->closed = true;
						break;
					}
				}

				// if write buffer is drained, disarm EPOLLOUT
				if (conn->write_buf.empty() && !conn->closed) {
					epoll_event wev{};
					wev.events = EPOLLIN;
					wev.data.ptr = conn;
					epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &wev);
				}

				// if we marked it closed during error response drain, finish the cleanup
				if (conn->closed && conn->write_buf.empty()) {
					epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
					close(fd);
					delete conn;
					continue;
				}
			}

			// read loop
			while (true) {
				ssize_t bytes = read(fd, buffer, BUF_SIZE);

				if (bytes > 0) {
					bool just_paired = false;

					if (!conn->peer) {
						Proxy::HostPort hp = Proxy::Utils::parse_request_target(buffer);

						// Handle parse errors
						if (hp.error) {
							fprintf(stderr, "parse error: %s\n", hp.error_msg.c_str());
							send_error_and_close(epfd, conn, BAD_REQUEST);
							break;
						}

						conn->is_connect_method = hp.is_connect;

						// create upstream socket
						int upstream_fd = socket(AF_INET, SOCK_STREAM, 0);
						if (upstream_fd < 0) {
							perror("socket");
							send_error_and_close(epfd, conn, BAD_GATEWAY);
							break;
						}

						set_nonblocking(upstream_fd);

						// resolve domain
						addrinfo hints{}, *res = nullptr;
						hints.ai_family = AF_INET;
						hints.ai_socktype = SOCK_STREAM;
						int dns_rc = getaddrinfo(hp.host.c_str(), std::to_string(hp.port).c_str(), &hints, &res);

						if (dns_rc != 0 || !res) {
							fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(dns_rc));
							close(upstream_fd);
							if (res) freeaddrinfo(res);
							send_error_and_close(epfd, conn, BAD_GATEWAY);
							break;
						}

						sockaddr_in upstream_addr = *(sockaddr_in*)res->ai_addr;
						freeaddrinfo(res);

						// create upstream connection object
						Connection* upstream = new Connection{};
						upstream->fd = upstream_fd;
						upstream->is_upstream = true;
						upstream->connect_in_progress = true;

						// pair client <-> upstream
						conn->peer = upstream;
						upstream->peer = conn;
						just_paired = true;

						int connect_rc = connect(
						    upstream_fd,
						    (sockaddr*)&upstream_addr,
						    sizeof(upstream_addr));

						if (connect_rc == 0) {
							// connected immediately
							upstream->connect_in_progress = false;

							if (conn->is_connect_method) {
								conn->write_buf.insert(conn->write_buf.end(), OK, OK + strlen(OK));
								epoll_event cev{};
								cev.events = EPOLLIN | EPOLLOUT;
								cev.data.ptr = conn;
								epoll_ctl(epfd, EPOLL_CTL_MOD, conn->fd, &cev);
							}

							epoll_event uev{};
							uev.events = EPOLLIN;
							uev.data.ptr = upstream;
							epoll_ctl(epfd, EPOLL_CTL_ADD, upstream_fd, &uev);

						} else if (connect_rc < 0 && errno == EINPROGRESS) {
							// connection in progress, arm EPOLLOUT
							upstream->connect_in_progress = true;
							epoll_event uev{};
							uev.events = EPOLLOUT;
							uev.data.ptr = upstream;
							epoll_ctl(epfd, EPOLL_CTL_ADD, upstream_fd, &uev);

						} else {
							perror("connect");
							// Unlink and clean up the upstream
							conn->peer = nullptr;
							upstream->peer = nullptr;
							close(upstream_fd);
							delete upstream;
							send_error_and_close(epfd, conn, BAD_GATEWAY);
							break;
						}
					}

					if (conn->peer) {
						// If we just paired and it's CONNECT, do NOT forward the buffer.
						// If we just paired and it's NOT CONNECT (HTTP), we MUST rewrite & forward.
						// If we were already paired, we always forward.
						if (!just_paired || !conn->is_connect_method) {
							if (just_paired && !conn->is_connect_method) {
								// Rewrite absolute-URI to relative for origin server
								std::vector<char> rewritten =
								    Proxy::Utils::rewrite_request(buffer, bytes);
								conn->peer->write_buf.insert(
								    conn->peer->write_buf.end(),
								    rewritten.begin(), rewritten.end());
							} else {
								conn->peer->write_buf.insert(
								    conn->peer->write_buf.end(), buffer, buffer + bytes);
							}

							// arm EPOLLOUT on the peer
							epoll_event pev{};
							pev.events = EPOLLIN | EPOLLOUT;
							pev.data.ptr = conn->peer;
							epoll_ctl(epfd, EPOLL_CTL_MOD, conn->peer->fd, &pev);
						}
					}
				} else if (bytes == 0) {
					// Peer closed — clean up both sides
					cleanup_connection(epfd, conn);
					conn = nullptr;
					break;
				} else {
					if (errno == EAGAIN || errno == EWOULDBLOCK)
						break;
					perror("read");
					cleanup_connection(epfd, conn);
					conn = nullptr;
					break;
				}
			}

			// If conn was cleaned up inside the read loop, skip the post-loop check
			if (!conn)
				continue;

			// Handle deferred close (e.g. after send_error_and_close scheduled a write)
			if (conn->closed && conn->write_buf.empty()) {
				cleanup_connection(epfd, conn);
			}
		}
	}

	close(listen_fd);
	close(epfd);
	return 0;
}