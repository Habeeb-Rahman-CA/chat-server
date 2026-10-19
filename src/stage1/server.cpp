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
              << "  -a, --address <ip>    Bind IP address (default: 0.0.0.0)\n"
              << "  -p, --port <port>     Bind port number (default: 8080)\n"
              << "  -b, --backlog <n>     Listen backlog size (default: 128)\n"
              << "  --once                Exit after serving a single client\n";
}

// Sends all bytes in the buffer, handling partial sends and EINTR.
bool send_all(int fd, const char* data, size_t length) {
    size_t total_sent = 0;
    while (total_sent < length) {
        ssize_t bytes = ::send(fd, data + total_sent, length - total_sent, MSG_NOSIGNAL);
        if (bytes < 0) {
            if (errno == EINTR) {
                // Interrupted by signal, retry
                continue;
            }
            // Broken pipe (client closed write end) or connection reset
            std::cerr << "[Server] send() error: " << std::strerror(errno) << " (errno " << errno << ")\n";
            return false;
        }
        total_sent += static_cast<size_t>(bytes);
    }
    return true;
}

// Handles communication with a single accepted client connection.
void handle_client(net::Socket client_sock, const struct sockaddr_in& client_addr) {
    std::string client_ip_port = net::format_address(client_addr);
    std::cout << "\n[Server] >>> New connection accepted from " << client_ip_port
              << " (Socket FD: " << client_sock.get() << ") <<<\n";

    // Send a welcome greeting
    std::string welcome = "Welcome to the Stage 1 TCP Server! Type a message and press Enter (or 'quit' to exit).\n";
    if (!send_all(client_sock.get(), welcome.data(), welcome.size())) {
        std::cerr << "[Server] Failed to send welcome message to " << client_ip_port << "\n";
        return;
    }

    constexpr size_t BUFFER_SIZE = 4096;
    char buffer[BUFFER_SIZE];

    while (g_running) {
        // recv() blocks here until:
        // 1. Data arrives from the peer (returns > 0)
        // 2. Peer closes the connection cleanly via FIN (returns 0)
        // 3. An error or signal occurs (returns -1)
        ssize_t bytes_read = ::recv(client_sock.get(), buffer, sizeof(buffer) - 1, 0);

        if (bytes_read > 0) {
            buffer[bytes_read] = '\0';
            std::cout << "[Server] Received " << bytes_read << " bytes from " << client_ip_port << ": ";
            // Print printable representation (handling newline cleanly)
            std::cout.write(buffer, bytes_read);
            if (bytes_read > 0 && buffer[bytes_read - 1] != '\n') {
                std::cout << '\n';
            }

            // Check if client asked to quit
            std::string_view msg(buffer, bytes_read);
            if (msg.starts_with("quit") || msg.starts_with("exit")) {
                std::cout << "[Server] Client requested disconnection.\n";
                std::string goodbye = "Goodbye!\n";
                send_all(client_sock.get(), goodbye.data(), goodbye.size());
                break;
            }

            // Echo back to client with an [Echo] prefix
            std::string response = "[Echo] " + std::string(buffer, bytes_read);
            if (!send_all(client_sock.get(), response.data(), response.size())) {
                std::cerr << "[Server] Connection lost while echoing to " << client_ip_port << "\n";
                break;
            }
        } else if (bytes_read == 0) {
            // TCP FIN packet received: peer initiated graceful shutdown
            std::cout << "[Server] Client " << client_ip_port << " closed connection (received EOF / FIN).\n";
            break;
        } else {
            // bytes_read < 0
            if (errno == EINTR) {
                // Signal interrupted syscall; if still running, retry
                if (g_running) continue;
                break;
            } else if (errno == ECONNRESET) {
                // TCP RST received: client process crashed or socket closed with unread data
                std::cerr << "[Server] Connection reset by peer " << client_ip_port << " (ECONNRESET)\n";
                break;
            } else {
                std::cerr << "[Server] recv() error from " << client_ip_port << ": "
                          << std::strerror(errno) << " (errno " << errno << ")\n";
                break;
            }
        }
    }

    std::cout << "[Server] <<< Closing connection with " << client_ip_port
              << " (Socket FD: " << client_sock.get() << ") >>>\n";
    // client_sock is closed automatically here by RAII Socket destructor
}

} // namespace

int main(int argc, char* argv[]) {
    std::string ip_str = "0.0.0.0";
    uint16_t port = 8080;
    int backlog = 128;
    bool serve_once = false;

    // Parse command line arguments
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if ((arg == "-a" || arg == "--address") && i + 1 < argc) {
            ip_str = argv[++i];
        } else if ((arg == "-p" || arg == "--port") && i + 1 < argc) {
            port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if ((arg == "-b" || arg == "--backlog") && i + 1 < argc) {
            backlog = std::stoi(argv[++i]);
        } else if (arg == "--once") {
            serve_once = true;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    // Install signal handler for graceful shutdown on SIGINT (Ctrl+C) and SIGTERM
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    ::sigaction(SIGINT, &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);

    // Ignore SIGPIPE so writing to a broken socket returns EPIPE instead of terminating process
    ::signal(SIGPIPE, SIG_IGN);

    try {
        std::cout << "========================================================\n";
        std::cout << "       Stage 1: Basic Single-Client TCP Server          \n";
        std::cout << "========================================================\n";

        // Step 1: Create the listening socket
        std::cout << "[1/4] Creating TCP socket via socket(AF_INET, SOCK_STREAM, 0)...\n";
        net::Socket server_sock = net::create_tcp_socket();
        std::cout << "      -> Success. Listening Socket FD = " << server_sock.get() << "\n";

        // Step 2: Set SO_REUSEADDR so port can be reused immediately after restart
        std::cout << "[2/4] Setting SO_REUSEADDR option...\n";
        net::set_reuse_addr(server_sock.get(), true);
        std::cout << "      -> Success.\n";

        // Step 3: Bind socket to local address & port
        struct sockaddr_in bind_addr;
        if (!net::parse_ipv4(ip_str, port, bind_addr)) {
            std::cerr << "Error: Invalid IP address: " << ip_str << "\n";
            return 1;
        }

        std::cout << "[3/4] Binding socket to " << ip_str << ":" << port << " via bind()...\n";
        if (::bind(server_sock.get(), reinterpret_cast<struct sockaddr*>(&bind_addr), sizeof(bind_addr)) < 0) {
            int err = errno;
            std::cerr << "Error: bind() failed on " << ip_str << ":" << port << ": "
                      << std::strerror(err) << " (errno " << err << ")\n";
            if (err == EADDRINUSE) {
                std::cerr << "Hint: The port is already in use by another process. Try another port or kill the process.\n";
            } else if (err == EACCES) {
                std::cerr << "Hint: Binding to ports below 1024 requires root privileges.\n";
            }
            return 1;
        }
        std::cout << "      -> Success.\n";

        // Step 4: Mark socket as passive listening socket
        std::cout << "[4/4] Listening with backlog = " << backlog << " via listen()...\n";
        if (::listen(server_sock.get(), backlog) < 0) {
            std::cerr << "Error: listen() failed: " << std::strerror(errno) << "\n";
            return 1;
        }
        std::cout << "      -> Server is now listening on " << ip_str << ":" << port << "\n";
        std::cout << "      (Run `nc " << (ip_str == "0.0.0.0" ? "127.0.0.1" : ip_str)
                  << " " << port << "` in another terminal to test)\n\n";

        // Accept loop: sequential single-client handling
        while (g_running) {
            std::cout << "[Server] Waiting for an incoming connection in accept()...\n";

            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            std::memset(&client_addr, 0, sizeof(client_addr));

            // accept() blocks until a client completes the 3-way TCP handshake
            int client_fd = ::accept(server_sock.get(),
                                     reinterpret_cast<struct sockaddr*>(&client_addr),
                                     &client_len);

            if (client_fd < 0) {
                if (errno == EINTR) {
                    // Signal caught, check g_running
                    if (!g_running) break;
                    continue;
                }
                std::cerr << "[Server] accept() error: " << std::strerror(errno) << "\n";
                continue;
            }

            // Wrap connected client fd in RAII Socket
            net::Socket client_sock(client_fd);

            // Handle client communication synchronously
            handle_client(std::move(client_sock), client_addr);

            if (serve_once) {
                std::cout << "[Server] --once specified; exiting after one client.\n";
                break;
            }
        }

        std::cout << "\n[Server] Shutting down gracefully. Closing listening socket (FD: "
                  << server_sock.get() << ").\n";

    } catch (const std::exception& ex) {
        std::cerr << "Fatal Exception: " << ex.what() << "\n";
        return 1;
    }

    return 0;
}

