#include "net/chat_protocol.hpp"
#include "net/socket.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <iostream>
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
    bool is_registered = false;
    std::string peer_addr;
    std::string in_buf;
    std::string out_buf;
    bool epollout_armed = false;
    bool closing = false;
};

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
        std::cout << "      Stage 6: Custom Application Protocol Server       \n";
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

        int epoll_fd = ::epoll_create1(EPOLL_CLOEXEC);
        if (epoll_fd < 0) {
            std::cerr << "Error: epoll_create1() failed: " << std::strerror(errno) << "\n";
            return 1;
        }
        net::Socket epoll_raii(epoll_fd);

        struct epoll_event ev_listen;
        std::memset(&ev_listen, 0, sizeof(ev_listen));
        ev_listen.data.fd = server_sock.get();
        ev_listen.events = EPOLLIN;
        ::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_sock.get(), &ev_listen);

        std::cout << "[Server] Listening on " << ip_str << ":" << port << " with Protocol Parser\n\n";

        std::unordered_map<int, ClientSession> clients;

        auto update_client_epoll = [&](int client_fd, bool want_write) {
            auto it = clients.find(client_fd);
            if (it == clients.end() || it->second.closing) return;

            struct epoll_event ev;
            std::memset(&ev, 0, sizeof(ev));
            ev.data.fd = client_fd;
            ev.events = EPOLLIN | EPOLLRDHUP;
            if (want_write) ev.events |= EPOLLOUT;

            it->second.epollout_armed = want_write;
            ::epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client_fd, &ev);
        };

        auto queue_write = [&](ClientSession& client, const std::string& msg) {
            client.out_buf.append(msg);
            if (!client.epollout_armed) {
                update_client_epoll(client.fd, true);
            }
        };

        auto broadcast_to_registered = [&](const std::string& msg, int exclude_fd = -1) {
            for (auto& [fd, cl] : clients) {
                if (fd != exclude_fd && cl.is_registered && !cl.closing) {
                    queue_write(cl, msg);
                }
            }
        };

        constexpr int MAX_EVENTS = 64;
        struct epoll_event events[MAX_EVENTS];

        while (g_running) {
            int nfds = ::epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
            if (nfds < 0) {
                if (errno == EINTR) {
                    if (!g_running) break;
                    continue;
                }
                break;
            }

            std::vector<int> close_list;

            for (int i = 0; i < nfds; ++i) {
                int event_fd = events[i].data.fd;
                uint32_t revents = events[i].events;

                if (event_fd == server_sock.get()) {
                    while (true) {
                        struct sockaddr_in client_addr;
                        socklen_t client_len = sizeof(client_addr);
                        int client_fd = ::accept4(server_sock.get(),
                                                  reinterpret_cast<struct sockaddr*>(&client_addr),
                                                  &client_len,
                                                  SOCK_NONBLOCK | SOCK_CLOEXEC);
                        if (client_fd < 0) {
                            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                            if (errno == EINTR) continue;
                            break;
                        }

                        struct epoll_event ev_client;
                        std::memset(&ev_client, 0, sizeof(ev_client));
                        ev_client.data.fd = client_fd;
                        ev_client.events = EPOLLIN | EPOLLRDHUP;
                        ::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev_client);

                        ClientSession session;
                        session.fd = client_fd;
                        session.peer_addr = net::format_address(client_addr);

                        clients[client_fd] = std::move(session);
                        // Send welcome protocol message
                        queue_write(clients[client_fd], net::ChatProtocol::serialize_ok("Welcome. Please register with NICK <username>"));
                    }
                    continue;
                }

                auto it = clients.find(event_fd);
                if (it == clients.end()) continue;
                ClientSession& cl = it->second;

                if (revents & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                    cl.closing = true;
                    close_list.push_back(event_fd);
                    continue;
                }

                // Handle flush writes
                if ((revents & EPOLLOUT) && !cl.out_buf.empty()) {
                    while (!cl.out_buf.empty()) {
                        ssize_t sent = ::send(event_fd, cl.out_buf.data(), cl.out_buf.size(), MSG_NOSIGNAL);
                        if (sent > 0) {
                            cl.out_buf.erase(0, static_cast<size_t>(sent));
                        } else if (sent < 0) {
                            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                            if (errno == EINTR) continue;
                            cl.closing = true;
                            close_list.push_back(event_fd);
                            break;
                        }
                    }
                    if (cl.out_buf.empty()) {
                        update_client_epoll(event_fd, false);
                    }
                }

                // Handle reads and protocol parsing
                if ((revents & EPOLLIN) && !cl.closing) {
                    char buf[4096];
                    while (true) {
                        ssize_t bytes = ::recv(event_fd, buf, sizeof(buf), 0);
                        if (bytes > 0) {
                            cl.in_buf.append(buf, static_cast<size_t>(bytes));
                        } else if (bytes == 0) {
                            cl.closing = true;
                            close_list.push_back(event_fd);
                            break;
                        } else {
                            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                            if (errno == EINTR) continue;
                            cl.closing = true;
                            close_list.push_back(event_fd);
                            break;
                        }
                    }

                    // Extract and dispatch protocol frames
                    bool oversized = false;
                    while (auto frame_opt = net::ChatProtocol::extract_next_frame(cl.in_buf, oversized)) {
                        net::ClientCommand cmd = net::ChatProtocol::parse_command(*frame_opt);

                        std::visit([&](auto&& arg) {
                            using T = std::decay_t<decltype(arg)>;
                            if constexpr (std::is_same_v<T, net::NickCmd>) {
                                const std::string& desired = arg.username;
                                bool taken = false;
                                for (const auto& [o_fd, o_cl] : clients) {
                                    if (o_cl.is_registered && o_cl.username == desired && o_fd != event_fd) {
                                        taken = true;
                                        break;
                                    }
                                }
                                if (taken) {
                                    queue_write(cl, net::ChatProtocol::serialize_error(409, "Nickname already taken"));
                                } else {
                                    bool was_registered = cl.is_registered;
                                    std::string old_name = cl.username;
                                    cl.username = desired;
                                    cl.is_registered = true;
                                    queue_write(cl, net::ChatProtocol::serialize_ok("Registered as " + desired));
                                    if (!was_registered) {
                                        broadcast_to_registered(net::ChatProtocol::serialize_join(desired), event_fd);
                                    } else {
                                        broadcast_to_registered(net::ChatProtocol::serialize_chat("SERVER", old_name + " is now known as " + desired), event_fd);
                                    }
                                }
                            } else if constexpr (std::is_same_v<T, net::MsgCmd>) {
                                if (!cl.is_registered) {
                                    queue_write(cl, net::ChatProtocol::serialize_error(401, "Must register nickname first via NICK <name>"));
                                } else {
                                    broadcast_to_registered(net::ChatProtocol::serialize_chat(cl.username, arg.text), event_fd);
                                    queue_write(cl, net::ChatProtocol::serialize_ok());
                                }
                            } else if constexpr (std::is_same_v<T, net::WhisperCmd>) {
                                if (!cl.is_registered) {
                                    queue_write(cl, net::ChatProtocol::serialize_error(401, "Must register nickname first"));
                                } else {
                                    ClientSession* target_session = nullptr;
                                    for (auto& [o_fd, o_cl] : clients) {
                                        if (o_cl.is_registered && o_cl.username == arg.target) {
                                            target_session = &o_cl;
                                            break;
                                        }
                                    }
                                    if (target_session) {
                                        queue_write(*target_session, net::ChatProtocol::serialize_whisper(cl.username, arg.text));
                                        queue_write(cl, net::ChatProtocol::serialize_ok("Whisper sent to " + arg.target));
                                    } else {
                                        queue_write(cl, net::ChatProtocol::serialize_error(404, "User not found"));
                                    }
                                }
                            } else if constexpr (std::is_same_v<T, net::UsersCmd>) {
                                std::vector<std::string> user_list;
                                for (const auto& [o_fd, o_cl] : clients) {
                                    if (o_cl.is_registered) {
                                        user_list.push_back(o_cl.username);
                                    }
                                }
                                queue_write(cl, net::ChatProtocol::serialize_users_list(user_list));
                            } else if constexpr (std::is_same_v<T, net::PingCmd>) {
                                queue_write(cl, net::ChatProtocol::serialize_pong(arg.payload));
                            } else if constexpr (std::is_same_v<T, net::QuitCmd>) {
                                queue_write(cl, net::ChatProtocol::serialize_ok("Goodbye"));
                                cl.closing = true;
                                close_list.push_back(event_fd);
                            } else if constexpr (std::is_same_v<T, net::MalformedCmd>) {
                                queue_write(cl, net::ChatProtocol::serialize_error(400, arg.reason));
                            }
                        }, cmd);
                    }

                    if (oversized) {
                        queue_write(cl, net::ChatProtocol::serialize_error(413, "Frame exceeds MAX_FRAME_SIZE (4096 bytes)"));
                    }
                }
            }

            for (int disc_fd : close_list) {
                auto it = clients.find(disc_fd);
                if (it != clients.end()) {
                    if (it->second.is_registered) {
                        broadcast_to_registered(net::ChatProtocol::serialize_leave(it->second.username), disc_fd);
                    }
                    ::epoll_ctl(epoll_fd, EPOLL_CTL_DEL, disc_fd, nullptr);
                    ::close(disc_fd);
                    clients.erase(it);
                }
            }
        }

        std::cout << "[Server] Shutting down protocol server...\n";
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
