# TCP Pair Proxy Server

A simple, asynchronous TCP proxy server written in C++ for Linux using `epoll`.

## Description

This server listens on a specified port (default 8080) and acts as a relay between pairs of connected clients.

- The first client to connect waits for a peer.
- The second client to connect is paired with the waiting client.
- Once paired, data sent by one client is automatically forwarded to the other.

## To-do

The server currently only acts as a relay. The end goal is to convert the server into a working proxy server.

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

2. Connect the first client (e.g., using `netcat`):

   ```sh
   nc localhost 8080
   ```

   _This client will wait for a peer._

3. Connect the second client:
   ```sh
   nc localhost 8080
   ```

This behavior is subject to change as the project evolves more into proxy server.
