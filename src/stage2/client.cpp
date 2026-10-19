#include "net/socket.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

namespace {

std::atomic<bool> g_running{true};

void sigint_handler(int) {
    g_running = false;
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
            return false;
        }
        total_sent += static_cast<size_t>(bytes);
    }
    return true;
}

// Background thread continuously receives incoming messages from server
void receive_thread_func(int sock_fd) {
    constexpr size_t BUFFER_SIZE = 4096;
    char buffer[BUFFER_SIZE];

    while (g_running) {
        ssize_t bytes_read = ::recv(sock_fd, buffer, sizeof(buffer) - 1, 0);
        if (bytes_read > 0) {
            buffer[bytes_read] = '\0';
            std::cout << buffer << std::flush;
        } else if (bytes_read == 0) {
            std::cout << "\n[Client] Server closed the connection (FIN received).\n";
            g_running = false;
            break;
        } else {
            if (errno == EINTR) continue;
            if (g_running) {
                std::cout << "\n[Client] Disconnected from server: " << std::strerror(errno) << "\n";
            }
            g_running = false;
            break;
        }
    }
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
        std::cout << "       Stage 2: Full-Duplex Multi-Threaded TCP Client   \n";
        std::cout << "========================================================\n";

        net::Socket client_sock = net::create_tcp_socket();
        struct sockaddr_in serv_addr;
        if (!net::parse_ipv4(server_ip, server_port, serv_addr)) {
            std::cerr << "Error: Invalid server IP: " << server_ip << "\n";
            return 1;
        }

        std::cout << "[Client] Connecting to " << server_ip << ":" << server_port << "...\n";
        if (::connect(client_sock.get(), reinterpret_cast<struct sockaddr*>(&serv_addr), sizeof(serv_addr)) < 0) {
            std::cerr << "Error: connect() failed: " << std::strerror(errno) << "\n";
            return 1;
        }
        std::cout << "[Client] Connected! Type messages to chat.\n\n";

        // Launch background receiver thread
        std::thread receiver(receive_thread_func, client_sock.get());

        // Foreground loop for user input
        std::string line;
        while (g_running && std::getline(std::cin, line)) {
            if (line.empty()) continue;

            std::string to_send = line + "\n";
            if (!send_all(client_sock.get(), to_send.data(), to_send.size())) {
                std::cerr << "[Client] Failed to send message to server.\n";
                break;
            }

            if (line == "/quit" || line == "quit" || line == "/exit") {
                g_running = false;
                break;
            }
        }

        g_running = false;
        // Shutdown socket write side and close
        ::shutdown(client_sock.get(), SHUT_RDWR);
        client_sock.close();

        if (receiver.joinable()) {
            receiver.join();
        }

        std::cout << "[Client] Session ended.\n";

    } catch (const std::exception& ex) {
        std::cerr << "Fatal Exception: " << ex.what() << "\n";
        return 1;
    }

    return 0;
}
