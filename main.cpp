#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <iostream>

#include "include/conn.h"

constexpr int PORT = 8080;
constexpr int MAX_EVENTS = 64;
constexpr int BUF_SIZE = 4096;

const std::string CLIENT_CLOSED = "Client closed connection.";
const std::string CLIENT_CLOSED_UNEXP = "Client unexpectedly closed connection.";

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
							continue;
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
						write(conn->peer->fd, CLIENT_CLOSED.c_str(), strlen(CLIENT_CLOSED.c_str()));

						waiting_conn = conn->peer;
						conn->peer->peer = nullptr;

						break;

					} else {
						// in case of error, terminate clients
						if (errno == EAGAIN || errno == EWOULDBLOCK)
							break;
						perror("read");
						conn->closed = true;

						if (conn->peer) {
							conn->peer->peer = nullptr;
						}

						write(conn->peer->fd, CLIENT_CLOSED_UNEXP.c_str(), strlen(CLIENT_CLOSED_UNEXP.c_str()));
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