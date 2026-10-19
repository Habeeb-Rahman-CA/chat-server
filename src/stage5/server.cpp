#include "net/socket.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/epoll.h>
#include <unordered_map>
#include <vector>

namespace {

volatile std::sig_atomic_t g_running = 1;

void sigint_handler(int) {
    g_running = 0;
}

struct ClientSession {
    int fd = -1;
    std::string username;
    std::string peer_addr;
    std::string in_buf;
    std::string out_buf;
    bool epollout_armed = false;
    bool closing = false;
};

std::string_view trim_line(std::string_view sv) {
    while (!sv.empty() && (sv.back() == '\r' || sv.back() == '\n' || sv.back() == ' ')) sv.remove_suffix(1);
    while (!sv.empty() && sv.front() == ' ') sv.remove_prefix(1);
    return sv;
}

void print_usage(const char* prog_name) {
    std::cout << "Usage: " << prog_name << " [options]\n"
              << "Options:\n"
              << "  -h, --help            Show this help message\n"
              << "  -a, --address <ip>    Bind IP address (default: 0.0.0.0)\n"
              << "  -p, --port <port>     Bind port number (default: 8080)\n"
              << "  --et                  Enable Edge-Triggered (EPOLLET) mode (default: Level-Triggered)\n";
}

} // namespace

int main(int argc, char* argv[]) {
    std::string ip_str = "0.0.0.0";
    uint16_t port = 8080;
    bool edge_triggered = false;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if ((arg == "-a" || arg == "--address") && i + 1 < argc) {
            ip_str = argv[++i];
        } else if ((arg == "-p" || arg == "--port") && i + 1 < argc) {
            port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--et") {
            edge_triggered = true;
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
        std::cout << "        Stage 5: High-Performance Linux epoll Server    \n";
        std::cout << "========================================================\n";

        net::Socket server_sock = net::create_tcp_socket();
        net::set_reuse_addr(server_sock.get(), true);
        net::set_nonblocking(server_sock.get(), true);

        struct sockaddr_in bind_addr;
        net::parse_ipv4(ip_str, port, bind_addr);
        if (::bind(server_sock.get(), reinterpret_cast<struct sockaddr*>(&bind_addr), sizeof(bind_addr)) < 0) {
            std::cerr << "Error: bind() failed: " << std::strerror(errno) << "\n";
            return 1;
        }

        if (::listen(server_sock.get(), SOMAXCONN) < 0) {
            std::cerr << "Error: listen() failed: " << std::strerror(errno) << "\n";
            return 1;
        }

        // Step 1: Create epoll instance with epoll_create1(EPOLL_CLOEXEC)
        int epoll_fd = ::epoll_create1(EPOLL_CLOEXEC);
        if (epoll_fd < 0) {
            std::cerr << "Error: epoll_create1() failed: " << std::strerror(errno) << "\n";
            return 1;
        }
        net::Socket epoll_raii(epoll_fd);

        // Step 2: Register listening socket in epoll
        struct epoll_event ev_listen;
        std::memset(&ev_listen, 0, sizeof(ev_listen));
        ev_listen.data.fd = server_sock.get();
        ev_listen.events = EPOLLIN;
        if (edge_triggered) {
            ev_listen.events |= EPOLLET;
        }

        if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_sock.get(), &ev_listen) < 0) {
            std::cerr << "Error: epoll_ctl(EPOLL_CTL_ADD, listen_fd) failed: " << std::strerror(errno) << "\n";
            return 1;
        }

        std::cout << "[Server] Listening on " << ip_str << ":" << port << "\n";
        std::cout << "[Server] Event mode: " << (edge_triggered ? "Edge-Triggered (EPOLLET)" : "Level-Triggered (LT)") << "\n";
        std::cout << "[Server] epoll instance created (FD: " << epoll_fd << ")\n\n";

        std::unordered_map<int, ClientSession> clients;
        uint64_t next_user_id = 1;

        auto update_client_epoll = [&](int client_fd, bool want_write) {
            auto it = clients.find(client_fd);
            if (it == clients.end() || it->second.closing) return;

            struct epoll_event ev;
            std::memset(&ev, 0, sizeof(ev));
            ev.data.fd = client_fd;
            ev.events = EPOLLIN | EPOLLRDHUP;
            if (want_write) {
                ev.events |= EPOLLOUT;
            }
            if (edge_triggered) {
                ev.events |= EPOLLET;
            }

            it->second.epollout_armed = want_write;
            ::epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client_fd, &ev);
        };

        auto queue_write = [&](ClientSession& client, const std::string& msg) {
            client.out_buf.append(msg);
            // Arm EPOLLOUT if not already armed
            if (!client.epollout_armed) {
                update_client_epoll(client.fd, true);
            }
        };

        auto broadcast = [&](const std::string& msg, int exclude_fd = -1) {
            for (auto& [fd, cl] : clients) {
                if (fd != exclude_fd && !cl.closing) {
                    queue_write(cl, msg);
                }
            }
        };

        constexpr int MAX_EVENTS = 64;
        struct epoll_event events[MAX_EVENTS];

        // Main epoll_wait event loop
        while (g_running) {
            int nfds = ::epoll_wait(epoll_fd, events, MAX_EVENTS, -1);

            if (nfds < 0) {
                if (errno == EINTR) {
                    if (!g_running) break;
                    continue;
                }
                std::cerr << "[Server] epoll_wait error: " << std::strerror(errno) << "\n";
                break;
            }

            std::vector<int> close_list;

            for (int i = 0; i < nfds; ++i) {
                int event_fd = events[i].data.fd;
                uint32_t revents = events[i].events;

                // Case A: Event on Listening Socket -> Accept new connections
                if (event_fd == server_sock.get()) {
                    while (true) {
                        struct sockaddr_in client_addr;
                        socklen_t client_len = sizeof(client_addr);
                        int client_fd = ::accept4(server_sock.get(),
                                                  reinterpret_cast<struct sockaddr*>(&client_addr),
                                                  &client_len,
                                                  SOCK_NONBLOCK | SOCK_CLOEXEC);

                        if (client_fd < 0) {
                            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                // In both LT and ET, all ready incoming connections have been drained
                                break;
                            }
                            if (errno == EINTR) continue;
                            std::cerr << "[Server] accept4 error: " << std::strerror(errno) << "\n";
                            break;
                        }

                        // Register new client in epoll
                        struct epoll_event ev_client;
                        std::memset(&ev_client, 0, sizeof(ev_client));
                        ev_client.data.fd = client_fd;
                        ev_client.events = EPOLLIN | EPOLLRDHUP;
                        if (edge_triggered) {
                            ev_client.events |= EPOLLET;
                        }

                        if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev_client) < 0) {
                            std::cerr << "[Server] epoll_ctl ADD failed for client: " << std::strerror(errno) << "\n";
                            ::close(client_fd);
                            continue;
                        }

                        ClientSession session;
                        session.fd = client_fd;
                        session.peer_addr = net::format_address(client_addr);
                        session.username = "user_" + std::to_string(next_user_id++);

                        std::cout << "[Server] Accepted client from " << session.peer_addr
                                  << " (FD " << client_fd << ", " << session.username << ")\n";

                        std::string greeting = "Welcome to the epoll Chat Server, " + session.username + "!\n"
                                               "Commands: /name <new_name>, /msg <user> <message>, /list, /quit\n";
                        clients[client_fd] = std::move(session);
                        queue_write(clients[client_fd], greeting);
                        broadcast("*** " + clients[client_fd].username + " has joined the chat ***\n", client_fd);
                    }
                    continue;
                }

                // Case B: Event on Client Socket
                auto it = clients.find(event_fd);
                if (it == clients.end()) {
                    // Stale event or already closed descriptor
                    continue;
                }
                ClientSession& cl = it->second;

                // 1. Check for errors or peer half-close (EPOLLRDHUP / EPOLLHUP / EPOLLERR)
                if (revents & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                    cl.closing = true;
                    close_list.push_back(event_fd);
                    continue;
                }

                // 2. Handle Write Readiness (EPOLLOUT)
                if ((revents & EPOLLOUT) && !cl.out_buf.empty()) {
                    while (!cl.out_buf.empty()) {
                        ssize_t sent = ::send(event_fd, cl.out_buf.data(), cl.out_buf.size(), MSG_NOSIGNAL);
                        if (sent > 0) {
                            cl.out_buf.erase(0, static_cast<size_t>(sent));
                        } else if (sent < 0) {
                            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                // Kernel socket send buffer full, wait for next EPOLLOUT
                                break;
                            }
                            if (errno == EINTR) continue;
                            cl.closing = true;
                            close_list.push_back(event_fd);
                            break;
                        }
                    }

                    // If output buffer is completely flushed, disarm EPOLLOUT to prevent busy-looping!
                    if (cl.out_buf.empty()) {
                        update_client_epoll(event_fd, false);
                    }
                }

                // 3. Handle Read Readiness (EPOLLIN)
                if ((revents & EPOLLIN) && !cl.closing) {
                    char buf[4096];
                    while (true) {
                        ssize_t bytes = ::recv(event_fd, buf, sizeof(buf), 0);
                        if (bytes > 0) {
                            cl.in_buf.append(buf, static_cast<size_t>(bytes));
                        } else if (bytes == 0) {
                            // Client initiated graceful shutdown (FIN)
                            cl.closing = true;
                            close_list.push_back(event_fd);
                            break;
                        } else {
                            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                // All currently available data drained (crucial in Edge-Triggered mode!)
                                break;
                            }
                            if (errno == EINTR) continue;
                            cl.closing = true;
                            close_list.push_back(event_fd);
                            break;
                        }
                    }

                    // Parse complete lines from in_buf
                    size_t newline_pos;
                    while ((newline_pos = cl.in_buf.find('\n')) != std::string::npos) {
                        std::string raw_line = cl.in_buf.substr(0, newline_pos);
                        cl.in_buf.erase(0, newline_pos + 1);

                        std::string_view line = trim_line(raw_line);
                        if (line.empty()) continue;

                        if (line == "/quit" || line == "quit" || line == "/exit") {
                            queue_write(cl, "Goodbye!\n");
                            cl.closing = true;
                            close_list.push_back(event_fd);
                            break;
                        } else if (line == "/list") {
                            std::ostringstream oss;
                            oss << "=== Online Users (" << clients.size() << ") ===\n";
                            for (const auto& [o_fd, o_cl] : clients) {
                                oss << " - " << o_cl.username << " (" << o_cl.peer_addr << ")\n";
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
                                broadcast("*** " + old_name + " is now known as " + new_name + " ***\n", event_fd);
                            }
                        } else if (line.starts_with("/msg ")) {
                            std::string_view rest = line.substr(5);
                            size_t space_idx = rest.find(' ');
                            if (space_idx != std::string_view::npos && space_idx > 0) {
                                std::string target(rest.substr(0, space_idx));
                                std::string content(trim_line(rest.substr(space_idx + 1)));
                                ClientSession* target_cl = nullptr;
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
                            broadcast(chat_msg, event_fd);
                        }
                    }
                }
            }

            // Purge disconnected clients cleanly
            for (int disc_fd : close_list) {
                auto it = clients.find(disc_fd);
                if (it != clients.end()) {
                    std::string departing_user = it->second.username;
                    // Deregister from epoll before closing FD to prevent stale event delivery
                    ::epoll_ctl(epoll_fd, EPOLL_CTL_DEL, disc_fd, nullptr);
                    ::close(disc_fd);
                    clients.erase(it);
                    broadcast("*** " + departing_user + " has left the chat ***\n");
                    std::cout << "[Server] Cleaned up client " << departing_user << " (FD " << disc_fd << ")\n";
                }
            }
        }

        std::cout << "\n[Server] Shutting down epoll server...\n";
        for (auto& [fd, cl] : clients) {
            ::epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
            ::close(fd);
        }

    } catch (const std::exception& ex) {
        std::cerr << "Fatal Exception: " << ex.what() << "\n";
        return 1;
    }

    return 0;
}

