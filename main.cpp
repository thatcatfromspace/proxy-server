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

#include "include/conn.h"
#include "src/utils.cpp"

constexpr int PORT = 8080;
constexpr int MAX_EVENTS = 64;
constexpr int BUF_SIZE = 4096;

const char* CLIENT_CLOSED = "Upstream client closed connection.\n";
const char* CLIENT_CLOSED_UNEXP = "Upstream client unexpectedly closed connection.\n";
const char* OK = "HTTP/1.1 200 Connection Established\r\n\r\n";

using Proxy::Connection;

Connection* waiting_conn = nullptr;

int set_nonblocking(int fd) {
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags == -1)
		return -1;
	return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
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

					Connection* conn = new Connection{client_fd, false};

					// if there is a waiting connection, pair it
					if (waiting_conn) {
						conn->peer = waiting_conn;
						conn->peer->peer = conn;
						waiting_conn = nullptr;
					}

					// else make it wait for a peer to connect
					else {
						waiting_conn = conn;
					}

					epoll_event cev{};
					cev.events = EPOLLIN;
					cev.data.ptr = conn;

					epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &cev);
				}
			}
			// client socket event
			else {
				Connection* conn =
				    static_cast<Connection*>(events[i].data.ptr);
				int fd = conn->fd;

				// drain write buffer if required
				if (events[i].events & EPOLLOUT) {
					// check for in-progress connection with server
					if (conn->is_upstream && conn->connect_in_progress) {
						int err = 0;
						socklen_t len = sizeof(err);

						if (getsockopt(conn->fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
							// connect failed
							conn->closed = true;
							conn->peer->closed = true;
						}

						// connect succeeded
						conn->connect_in_progress = false;

						// send 200 Connection Established to client
						conn->peer->write_buf.insert(
						    conn->peer->write_buf.end(), OK, OK + strlen(OK));

						// enable EPOLLOUT on client to flush 200
						epoll_event cev{};
						cev.events = EPOLLIN | EPOLLOUT;
						cev.data.ptr = conn->peer;
						epoll_ctl(epfd, EPOLL_CTL_MOD, conn->peer->fd, &cev);

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
						}

						else if (errno == EAGAIN || errno == EWOULDBLOCK) {
							break;
						}

						else {
							perror("write");
							conn->closed = true;
							break;
						}
					}

					// if write buffer is drained, disarm EPOLLOUT
					if (conn->write_buf.empty() && !conn->closed) {
						epoll_event ev{};
						ev.events = EPOLLIN;
						ev.data.ptr = conn;
						epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
					}
				}

				while (true) {
					ssize_t bytes = read(fd, buffer, BUF_SIZE);

					if (bytes > 0) {

						if (!conn->peer) {
							Proxy::HostPort hp = Proxy::Utils::parse_connect_target(buffer);
							// create upstream socket
							int upstream_fd = socket(AF_INET, SOCK_STREAM, 0);
							if (upstream_fd < 0) {
								perror("socket");
								conn->closed = true;
							}

							set_nonblocking(upstream_fd);

							// create upstream connection object
							Connection* upstream = new Connection{};
							upstream->fd = upstream_fd;
							upstream->is_upstream = true;
							upstream->connect_in_progress = true;

							// pair client <-> upstream
							conn->peer = upstream;
							upstream->peer = conn;

							// resolve domain of requested resource
							// TODO: getaddrinfo is blocking: offload to background thread

							addrinfo hints{}, *res;
							hints.ai_family = AF_INET;
							hints.ai_socktype = SOCK_STREAM;
							int dns_rc = getaddrinfo(hp.host.c_str(), std::to_string(hp.port).c_str(), &hints, &res);

							if (dns_rc != 0) {
								fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(dns_rc));
								conn->closed = true;
							}

							sockaddr_in addr = *(sockaddr_in*)res->ai_addr;
							freeaddrinfo(res);

							int connect_rc = connect(
							    upstream_fd,
							    (sockaddr*)&addr,
							    sizeof(addr));

							if (connect_rc == 0) {
								// connected immediately
								upstream->connect_in_progress = false;

							} else if (errno == EINPROGRESS) {
								// if connection in progress, arm EPOLLOUT
								upstream->connect_in_progress = true;
								epoll_event uev{};
								uev.events = EPOLLOUT;
								uev.data.ptr = upstream;
								epoll_ctl(epfd, EPOLL_CTL_ADD, upstream_fd, &uev);

							} else {
								perror("connect");
								conn->closed = true;
							}
						}

						if (conn->peer) {
							// write to peer's write buffer
							conn->peer->write_buf.insert(conn->peer->write_buf.end(), buffer, buffer + bytes);

							// create a new write event on the peer
							epoll_event pev{};
							pev.events = EPOLLIN | EPOLLOUT;
							pev.data.ptr = conn->peer;
							epoll_ctl(epfd, EPOLL_CTL_MOD, conn->peer->fd, &pev);
						}

					} else if (bytes == 0) {
						// 0 bytes sent -> client closed connection
						// close the connection, notify peer and move peer to waiting
						conn->closed = true;
						if (conn->peer) {
							// write(conn->peer->fd, CLIENT_CLOSED, strlen(CLIENT_CLOSED));
							waiting_conn = conn->peer;
							conn->peer->peer = nullptr;
						} else if (waiting_conn == conn) {
							waiting_conn = nullptr;
						}

						break;

					} else {
						// in case of error, terminate clients
						if (errno == EAGAIN || errno == EWOULDBLOCK)
							break;
						perror("read");
						conn->closed = true;

						if (conn->peer) {
							conn->peer->peer = nullptr;
							// this is no longer required - debug only
							// write(conn->peer->fd, CLIENT_CLOSED_UNEXP, strlen(CLIENT_CLOSED_UNEXP));
						} else if (waiting_conn == conn) {
							waiting_conn = nullptr;
						}
						break;
					}
				}

				if (conn->closed) {
					epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
					close(fd);
					delete conn;
				}
			}
		}
	}

	close(listen_fd);
	close(epfd);
	return 0;
}