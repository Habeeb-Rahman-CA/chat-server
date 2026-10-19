#include "net/socket.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <stdexcept>
#include <system_error>

namespace net {

Socket create_tcp_socket() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::system_error(errno, std::generic_category(), "socket() failed");
    }
    return Socket(fd);
}

void set_reuse_addr(int fd, bool enable) {
    int optval = enable ? 1 : 0;
    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
        throw std::system_error(errno, std::generic_category(), "setsockopt(SO_REUSEADDR) failed");
    }
}

void set_nonblocking(int fd, bool enable) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        throw std::system_error(errno, std::generic_category(), "fcntl(F_GETFL) failed");
    }
    if (enable) {
        flags |= O_NONBLOCK;
    } else {
        flags &= ~O_NONBLOCK;
    }
    if (::fcntl(fd, F_SETFL, flags) < 0) {
        throw std::system_error(errno, std::generic_category(), "fcntl(F_SETFL) failed");
    }
}

void set_tcp_keepalive(int fd, int idle_sec, int interval_sec, int count) {
    int optval = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof(optval));
#ifdef TCP_KEEPIDLE
    ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle_sec, sizeof(idle_sec));
#endif
#ifdef TCP_KEEPINTVL
    ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval_sec, sizeof(interval_sec));
#endif
#ifdef TCP_KEEPCNT
    ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
#endif
}

bool parse_ipv4(const std::string& ip_str, uint16_t port, struct sockaddr_in& out_addr) {
    std::memset(&out_addr, 0, sizeof(out_addr));
    out_addr.sin_family = AF_INET;
    out_addr.sin_port = htons(port);

    if (ip_str.empty() || ip_str == "0.0.0.0") {
        out_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        return true;
    }

    int ret = ::inet_pton(AF_INET, ip_str.c_str(), &out_addr.sin_addr);
    return ret == 1;
}

std::string format_address(const struct sockaddr_in& addr) {
    char ip_buf[INET_ADDRSTRLEN];
    const char* ip_ptr = ::inet_ntop(AF_INET, &addr.sin_addr, ip_buf, sizeof(ip_buf));
    uint16_t port = ntohs(addr.sin_port);
    if (!ip_ptr) {
        return "unknown:" + std::to_string(port);
    }
    return std::string(ip_buf) + ":" + std::to_string(port);
}

std::string format_peer_address(int fd) {
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    if (::getpeername(fd, reinterpret_cast<struct sockaddr*>(&addr), &len) < 0) {
        return "unknown";
    }
    return format_address(addr);
}

} // namespace net

