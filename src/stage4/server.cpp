#include "net/socket.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/select.h>
#include <unordered_map>
#include <vector>

namespace {

volatile std::sig_atomic_t g_running = 1;

void sigint_handler(int) {
    g_running = 0;
}

struct Client {
    int fd = -1;
    std::string username;
    std::string peer_addr;
    std::string in_buf;   // Accumulates bytes until complete newline-delimited messages are formed
    std::string out_buf;  // Pending bytes to be sent when socket is writable
    bool disconnect = false;
};

std::string_view trim_line(std::string_view sv) {
    while (!sv.empty() && (sv.back() == '\r' || sv.back() == '\n' || sv.back() == ' ')) sv.remove_suffix(1);
    while (!sv.empty() && sv.front() == ' ') sv.remove_prefix(1);
    return sv;
}

void queue_write(Client& client, const std::string& msg) {
    client.out_buf.append(msg);
}

void print_usage(const char* prog_name) {
    std::cout << "Usage: " << prog_name << " [options]\n"
              << "Options:\n"
              << "  -h, --help            Show this help message\n"
              << "  -a, --address <ip>    Bind IP address (default: 0.0.0.0)\n"
              << "  -p, --port <port>     Bind port number (default: 8080)\n";
}

} // namespace

int main(int argc, char* argv[]) {
    std::string ip_str = "0.0.0.0";
    uint16_t port = 8080;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if ((arg == "-a" || arg == "--address") && i + 1 < argc) {
            ip_str = argv[++i];
        } else if ((arg == "-p" || arg == "--port") && i + 1 < argc) {
            port = static_cast<uint16_t>(std::stoi(argv[++i]));
        }
    }

    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    ::sigaction(SIGINT, &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);
    ::signal(SIGPIPE, SIG_IGN);

    try {
        std::cout << "========================================================\n";
        std::cout << "     Stage 4: Non-Blocking Sockets & select() Server    \n";
        std::cout << "========================================================\n";

        net::Socket server_sock = net::create_tcp_socket();
        net::set_reuse_addr(server_sock.get(), true);

        // Configure the listening socket as non-blocking
        net::set_nonblocking(server_sock.get(), true);

        struct sockaddr_in bind_addr;
        net::parse_ipv4(ip_str, port, bind_addr);
        if (::bind(server_sock.get(), reinterpret_cast<struct sockaddr*>(&bind_addr), sizeof(bind_addr)) < 0) {
            std::cerr << "Error: bind() failed: " << std::strerror(errno) << "\n";
            return 1;
        }

        if (::listen(server_sock.get(), 128) < 0) {
            std::cerr << "Error: listen() failed: " << std::strerror(errno) << "\n";
            return 1;
        }

        std::cout << "[Server] Listening on " << ip_str << ":" << port << " (non-blocking)\n";
        std::cout << "[Server] select() event loop initialized (FD_SETSIZE = " << FD_SETSIZE << ")\n\n";

        std::unordered_map<int, Client> clients;
        uint64_t next_user_id = 1;

        auto broadcast = [&](const std::string& msg, int exclude_fd = -1) {
            for (auto& [fd, cl] : clients) {
                if (fd != exclude_fd && !cl.disconnect) {
                    queue_write(cl, msg);
                }
            }
        };

        // Main select() event loop
        while (g_running) {
            fd_set read_fds;
            fd_set write_fds;
            FD_ZERO(&read_fds);
            FD_ZERO(&write_fds);

            // Always monitor listening socket for new incoming connections
            FD_SET(server_sock.get(), &read_fds);
            int max_fd = server_sock.get();

            // Populate file descriptor sets for all active clients
            for (const auto& [fd, cl] : clients) {
                FD_SET(fd, &read_fds);
                // CRITICAL select() DESIGN: Only monitor write readiness when pending data exists!
                // If we always monitored write readiness, select() would return immediately
                // on every cycle because TCP send buffers are almost always writable, causing 100% CPU busy-wait.
                if (!cl.out_buf.empty()) {
                    FD_SET(fd, &write_fds);
                }
                max_fd = std::max(max_fd, fd);
            }

            // select() blocks until at least one descriptor is ready or a signal arrives
            int activity = ::select(max_fd + 1, &read_fds, &write_fds, nullptr, nullptr);

            if (activity < 0) {
                if (errno == EINTR) {
                    if (!g_running) break;
                    continue;
                }
                std::cerr << "[Server] select() error: " << std::strerror(errno) << "\n";
                break;
            }

            // 1. Check if listening socket is readable -> New connection ready to accept
            if (FD_ISSET(server_sock.get(), &read_fds)) {
                while (true) {
                    struct sockaddr_in client_addr;
                    socklen_t client_len = sizeof(client_addr);
                    int client_fd = ::accept(server_sock.get(),
                                             reinterpret_cast<struct sockaddr*>(&client_addr),
                                             &client_len);

                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            // Drained all queued connections in Accept Queue
                            break;
                        }
                        if (errno == EINTR) continue;
                        std::cerr << "[Server] accept() error: " << std::strerror(errno) << "\n";
                        break;
                    }

                    // select() limitation check: FD must be strictly less than FD_SETSIZE (1024)
                    if (client_fd >= FD_SETSIZE) {
                        std::cerr << "[Server] Rejecting FD " << client_fd
                                  << " (exceeds FD_SETSIZE limit of " << FD_SETSIZE << ")\n";
                        ::close(client_fd);
                        continue;
                    }

                    // Set newly accepted client socket to non-blocking
                    net::set_nonblocking(client_fd, true);

                    Client new_client;
                    new_client.fd = client_fd;
                    new_client.peer_addr = net::format_address(client_addr);
                    new_client.username = "user_" + std::to_string(next_user_id++);

                    std::cout << "[Server] Accepted new connection from " << new_client.peer_addr
                              << " (FD " << client_fd << ", username: " << new_client.username << ")\n";

                    std::string greeting = "Welcome to the select() Chat Server, " + new_client.username + "!\n"
                                           "Commands: /name <new_name>, /msg <user> <message>, /list, /quit\n";
                    queue_write(new_client, greeting);
                    broadcast("*** " + new_client.username + " has joined the chat ***\n", client_fd);

                    clients[client_fd] = std::move(new_client);
                }
            }

            // 2. Check read and write readiness for each connected client
            std::vector<int> disconnected_fds;

            for (auto& [fd, cl] : clients) {
                // Handle Write Readiness (Drain Output Buffer)
                if (FD_ISSET(fd, &write_fds) && !cl.out_buf.empty()) {
                    ssize_t bytes_sent = ::send(fd, cl.out_buf.data(), cl.out_buf.size(), MSG_NOSIGNAL);
                    if (bytes_sent > 0) {
                        cl.out_buf.erase(0, static_cast<size_t>(bytes_sent));
                    } else if (bytes_sent < 0) {
                        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                            std::cerr << "[Server] send() error on " << cl.username << ": " << std::strerror(errno) << "\n";
                            cl.disconnect = true;
                        }
                    }
                }

                // Handle Read Readiness (Drain Input Buffer & Parse Messages)
                if (FD_ISSET(fd, &read_fds) && !cl.disconnect) {
                    char buffer[4096];
                    while (true) {
                        ssize_t bytes_read = ::recv(fd, buffer, sizeof(buffer), 0);
                        if (bytes_read > 0) {
                            cl.in_buf.append(buffer, static_cast<size_t>(bytes_read));
                        } else if (bytes_read == 0) {
                            // Client closed connection cleanly
                            std::cout << "[Server] Client " << cl.username << " closed connection (EOF).\n";
                            cl.disconnect = true;
                            break;
                        } else {
                            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                // All currently available data read
                                break;
                            } else if (errno == EINTR) {
                                continue;
                            } else {
                                std::cerr << "[Server] recv() error on " << cl.username << ": " << std::strerror(errno) << "\n";
                                cl.disconnect = true;
                                break;
                            }
                        }
                    }

                    // Process complete newline-delimited messages from in_buf
                    size_t newline_pos;
                    while ((newline_pos = cl.in_buf.find('\n')) != std::string::npos) {
                        std::string raw_line = cl.in_buf.substr(0, newline_pos);
                        cl.in_buf.erase(0, newline_pos + 1);

                        std::string_view line = trim_line(raw_line);
                        if (line.empty()) continue;

                        if (line == "/quit" || line == "quit" || line == "/exit") {
                            queue_write(cl, "Goodbye!\n");
                            cl.disconnect = true;
                            break;
                        } else if (line == "/list") {
                            std::ostringstream oss;
                            oss << "=== Online Users (" << clients.size() << ") ===\n";
                            for (const auto& [other_fd, other_cl] : clients) {
                                oss << " - " << other_cl.username << " (" << other_cl.peer_addr << ")\n";
                            }
                            oss << "===========================\n";
                            queue_write(cl, oss.str());
                        } else if (line.starts_with("/name ")) {
                            std::string new_name = std::string(trim_line(line.substr(6)));
                            bool taken = false;
                            for (const auto& [o_fd, o_cl] : clients) {
                                if (o_cl.username == new_name) { taken = true; break; }
                            }
                            if (taken) {
                                queue_write(cl, "Error: Username already taken.\n");
                            } else if (new_name.empty() || new_name.find(' ') != std::string::npos) {
                                queue_write(cl, "Error: Invalid username format.\n");
                            } else {
                                std::string old_name = cl.username;
                                cl.username = new_name;
                                queue_write(cl, "Username changed to " + new_name + "\n");
                                broadcast("*** " + old_name + " is now known as " + new_name + " ***\n", fd);
                            }
                        } else if (line.starts_with("/msg ")) {
                            std::string_view rest = line.substr(5);
                            size_t space_idx = rest.find(' ');
                            if (space_idx != std::string_view::npos && space_idx > 0) {
                                std::string target(rest.substr(0, space_idx));
                                std::string content(trim_line(rest.substr(space_idx + 1)));
                                Client* target_cl = nullptr;
                                for (auto& [o_fd, o_cl] : clients) {
                                    if (o_cl.username == target) { target_cl = &o_cl; break; }
                                }
                                if (target_cl) {
                                    queue_write(*target_cl, "[Whisper from " + cl.username + "] " + content + "\n");
                                    queue_write(cl, "[Whisper to " + target + "] " + content + "\n");
                                } else {
                                    queue_write(cl, "Error: User '" + target + "' not found.\n");
                                }
                            }
                        } else {
                            std::string chat_msg = "[" + cl.username + "]: " + std::string(line) + "\n";
                            broadcast(chat_msg, fd);
                        }
                    }
                }

                if (cl.disconnect && cl.out_buf.empty()) {
                    disconnected_fds.push_back(fd);
                }
            }

            // Purge disconnected clients
            for (int disc_fd : disconnected_fds) {
                auto it = clients.find(disc_fd);
                if (it != clients.end()) {
                    std::string departing_user = it->second.username;
                    ::close(disc_fd);
                    clients.erase(it);
                    broadcast("*** " + departing_user + " has left the chat ***\n");
                }
            }
        }

        std::cout << "\n[Server] Shutting down. Closing all client sockets...\n";
        for (auto& [fd, cl] : clients) {
            ::close(fd);
        }

    } catch (const std::exception& ex) {
        std::cerr << "Fatal Exception: " << ex.what() << "\n";
        return 1;
    }

    return 0;
}

