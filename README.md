# C++ Proxy Server

A simple, asynchronous TCP proxy server written in C++ for Linux using `epoll`.

## Description

This server listens on a specified port (default 8080) and proxies TCP connections to a specified web resource. This can be used as a drop-in replacement to any other proxy server.

## To-do

- 407 Proxy Authentication Required
- DNS `getaddrinfo` offloading
- IPv6 support
- Logging
- Edge-triggered `epoll`
- Connection timeouts

## Requirements

- Linux OS (due to `epoll` usage)
- g++ compiler with C++17 support
- Make

## Build

To build the project, simply run:

```sh
make
```

This will produce an executable named `proxy-server`.

## Usage

1. Start the server:

   ```sh
   ./proxy-server
   ```

2. Connect the first client (e.g., using `curl`):

   ```sh
   curl -x http://localhost:8080 https://example.com

   # optionally add the -v flag to see verbose output
   curl -x http://localhost:8080 -v https://example.com
   ```

   _This client will connect to the requested URL via the proxy server._
