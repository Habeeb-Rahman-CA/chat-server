#include "net/socket.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>

namespace {
volatile std::sig_atomic_t g_running = 1;

void sigint_handler(int) {
    g_running = 0;
}

void print_usage(const char* prog_name) {
    std::cout << "Usage: " << prog_name << " [options]\n"
              << "Options:\n"
              << "  -h, --help            Show this help message\n"
              << "  -a, --address <ip>    Server IP address (default: 127.0.0.1)\n"
              << "  -p, --port <port>     Server port number (default: 8080)\n";
}

bool send_all(int fd, const char* data, size_t length) {
    size_t total_sent = 0;
    while (total_sent < length) {
        ssize_t bytes = ::send(fd, data + total_sent, length - total_sent, MSG_NOSIGNAL);
        if (bytes < 0) {
            if (errno == EINTR) continue;
            std::cerr << "[Client] send() error: " << std::strerror(errno) << " (errno " << errno << ")\n";
            return false;
        }
        total_sent += static_cast<size_t>(bytes);
    }
    return true;
}

} // namespace

int main(int argc, char* argv[]) {
    std::string server_ip = "127.0.0.1";
    uint16_t server_port = 8080;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if ((arg == "-a" || arg == "--address") && i + 1 < argc) {
            server_ip = argv[++i];
        } else if ((arg == "-p" || arg == "--port") && i + 1 < argc) {
            server_port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    ::sigaction(SIGINT, &sa, nullptr);

    ::signal(SIGPIPE, SIG_IGN);

    try {
        std::cout << "========================================================\n";
        std::cout << "          Stage 1: Basic TCP Client                     \n";
        std::cout << "========================================================\n";

        // Step 1: Create client socket
        std::cout << "[1/2] Creating TCP socket via socket(AF_INET, SOCK_STREAM, 0)...\n";
        net::Socket client_sock = net::create_tcp_socket();
        std::cout << "      -> Success. Client Socket FD = " << client_sock.get() << "\n";

        // Step 2: Connect to server
        struct sockaddr_in serv_addr;
        if (!net::parse_ipv4(server_ip, server_port, serv_addr)) {
            std::cerr << "Error: Invalid server IP address: " << server_ip << "\n";
            return 1;
        }

        std::cout << "[2/2] Connecting to " << server_ip << ":" << server_port << " via connect()...\n";
        if (::connect(client_sock.get(), reinterpret_cast<struct sockaddr*>(&serv_addr), sizeof(serv_addr)) < 0) {
            int err = errno;
            std::cerr << "Error: connect() failed: " << std::strerror(err) << " (errno " << err << ")\n";
            if (err == ECONNREFUSED) {
                std::cerr << "Hint: Is the server running on " << server_ip << ":" << server_port << "?\n";
            }
            return 1;
        }
        std::cout << "      -> Connected successfully!\n\n";

        // Receive server greeting
        char recv_buf[4096];
        ssize_t bytes_read = ::recv(client_sock.get(), recv_buf, sizeof(recv_buf) - 1, 0);
        if (bytes_read > 0) {
            recv_buf[bytes_read] = '\0';
            std::cout << recv_buf;
        }

        // Interactive communication loop
        std::string line;
        while (g_running) {
            std::cout << "chat> " << std::flush;
            if (!std::getline(std::cin, line)) {
                // EOF encountered (Ctrl+D)
                std::cout << "\n[Client] Exiting (EOF detected).\n";
                break;
            }

            if (line.empty()) {
                continue;
            }

            // Append newline for framing
            std::string to_send = line + "\n";
            if (!send_all(client_sock.get(), to_send.data(), to_send.size())) {
                std::cerr << "[Client] Failed to send data to server.\n";
                break;
            }

            if (line == "quit" || line == "exit") {
                std::cout << "[Client] Disconnecting...\n";
                break;
            }

            // Wait for echo response
            bytes_read = ::recv(client_sock.get(), recv_buf, sizeof(recv_buf) - 1, 0);
            if (bytes_read > 0) {
                recv_buf[bytes_read] = '\0';
                std::cout << recv_buf;
            } else if (bytes_read == 0) {
                std::cout << "[Client] Server closed the connection.\n";
                break;
            } else {
                if (errno == EINTR) continue;
                std::cerr << "[Client] recv() error: " << std::strerror(errno) << "\n";
                break;
            }
        }

        std::cout << "[Client] Closed connection.\n";

    } catch (const std::exception& ex) {
        std::cerr << "Fatal Exception: " << ex.what() << "\n";
        return 1;
    }

    return 0;
}
