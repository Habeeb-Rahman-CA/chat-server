#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <string>
#include <utility>

namespace net {

// RAII wrapper for an OS file descriptor.
// Ensures that close() is always called exactly once when the object goes out of scope,
// preventing resource leaks even in the presence of early returns or exceptions.
class Socket {
public:
    Socket() noexcept : fd_(-1) {}
    explicit Socket(int fd) noexcept : fd_(fd) {}

    ~Socket() {
        close();
    }

    // Move semantics: transfer ownership of the file descriptor
    Socket(Socket&& other) noexcept : fd_(other.fd_) {
        other.fd_ = -1;
    }

    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            close();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    // Non-copyable: file descriptors represent unique kernel resources
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool is_valid() const noexcept { return fd_ >= 0; }
    explicit operator bool() const noexcept { return is_valid(); }

    int release() noexcept {
        int old_fd = fd_;
        fd_ = -1;
        return old_fd;
    }

    void reset(int new_fd = -1) {
        close();
        fd_ = new_fd;
    }

    void close() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_;
};

// Creates an IPv4 TCP stream socket: socket(AF_INET, SOCK_STREAM, 0)
Socket create_tcp_socket();

// Enables SO_REUSEADDR so the server can re-bind immediately after restart
// without waiting for sockets in TIME_WAIT state to expire.
void set_reuse_addr(int fd, bool enable = true);

// Converts an IPv4 string and port into a sockaddr_in structure.
bool parse_ipv4(const std::string& ip_str, uint16_t port, struct sockaddr_in& out_addr);

// Formats a sockaddr_in as "IP:port"
std::string format_address(const struct sockaddr_in& addr);

// Queries the peer address of a connected socket via getpeername()
std::string format_peer_address(int fd);

} // namespace net
