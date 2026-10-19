#include "net/chat_protocol.hpp"
#include "net/socket.hpp"

#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unordered_map>
#include <vector>

namespace {

volatile std::sig_atomic_t g_running = 1;

void sigint_handler(int) {
    g_running = 0;
}

// Security and Reliability Constraints
constexpr size_t DEFAULT_MAX_CLIENTS = 1000;
constexpr size_t MAX_OUT_BUFFER_BYTES = 512 * 1024; // 512 KB slow-client buffer threshold
constexpr int DEFAULT_IDLE_TIMEOUT_SECONDS = 60;     // Disconnect clients inactive for 60s

struct ClientSession {
    int fd = -1;
    std::string username;
    bool is_registered = false;
    std::string peer_addr;
    std::string in_buf;
    std::string out_buf;
    bool epollout_armed = false;
    bool closing = false;
    std::chrono::steady_clock::time_point last_activity;
};

void log_event(std::string_view tag, std::string_view msg) {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf;
    ::localtime_r(&t, &tm_buf);
    std::cout << "[" << std::put_time(&tm_buf, "%H:%M:%S") << "] [" << tag << "] " << msg << "\n";
}

bool is_valid_username(std::string_view name) {
    if (name.size() < 3 || name.size() > 32) return false;
    for (char c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
            return false;
        }
    }
    return true;
}

void print_usage(const char* prog_name) {
    std::cout << "Usage: " << prog_name << " [options]\n"
              << "Options:\n"
              << "  -h, --help            Show this help message\n"
              << "  -a, --address <ip>    Bind IP address (default: 0.0.0.0)\n"
              << "  -p, --port <port>     Bind port number (default: 8080)\n"
              << "  -m, --max-clients <n> Max simultaneous connections (default: 1000)\n"
              << "  -t, --timeout <sec>   Idle client timeout in seconds (default: 60)\n";
}

} // namespace

int main(int argc, char* argv[]) {
    std::string ip_str = "0.0.0.0";
    uint16_t port = 8080;
    size_t max_clients = DEFAULT_MAX_CLIENTS;
    int idle_timeout_sec = DEFAULT_IDLE_TIMEOUT_SECONDS;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if ((arg == "-a" || arg == "--address") && i + 1 < argc) {
            ip_str = argv[++i];
        } else if ((arg == "-p" || arg == "--port") && i + 1 < argc) {
            port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if ((arg == "-m" || arg == "--max-clients") && i + 1 < argc) {
            max_clients = static_cast<size_t>(std::stoul(argv[++i]));
        } else if ((arg == "-t" || arg == "--timeout") && i + 1 < argc) {
            idle_timeout_sec = std::stoi(argv[++i]);
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
        std::cout << "   Stage 7: Production-Grade Reliability & Security    \n";
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
            std::cerr << "Error: epoll_create1 failed: " << std::strerror(errno) << "\n";
            return 1;
        }
        net::Socket epoll_raii(epoll_fd);

        // Register listening socket
        struct epoll_event ev_listen;
        std::memset(&ev_listen, 0, sizeof(ev_listen));
        ev_listen.data.fd = server_sock.get();
        ev_listen.events = EPOLLIN;
        ::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_sock.get(), &ev_listen);

        // Linux timerfd for periodic idle client reaping (every 1 second)
        int timer_fd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (timer_fd >= 0) {
            struct itimerspec ts;
            ts.it_interval.tv_sec = 1;
            ts.it_interval.tv_nsec = 0;
            ts.it_value.tv_sec = 1;
            ts.it_value.tv_nsec = 0;
            ::timerfd_settime(timer_fd, 0, &ts, nullptr);

            struct epoll_event ev_timer;
            std::memset(&ev_timer, 0, sizeof(ev_timer));
            ev_timer.data.fd = timer_fd;
            ev_timer.events = EPOLLIN;
            ::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, timer_fd, &ev_timer);
        }
        net::Socket timer_raii(timer_fd);

        log_event("INIT", "Server listening on " + ip_str + ":" + std::to_string(port));
        log_event("CONFIG", "Max Clients: " + std::to_string(max_clients) +
                          ", Idle Timeout: " + std::to_string(idle_timeout_sec) + "s" +
                          ", Max OutBuffer: " + std::to_string(MAX_OUT_BUFFER_BYTES / 1024) + " KB");

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

        auto queue_write = [&](ClientSession& client, const std::string& msg) -> bool {
            // SLOW-CLIENT PROTECTION (Backpressure):
            // If client's output buffer exceeds threshold, client is too slow or stalling.
            // Disconnect client to prevent memory exhaustion!
            if (client.out_buf.size() + msg.size() > MAX_OUT_BUFFER_BYTES) {
                log_event("PROTECT", "Slow client buffer overflow (" + std::to_string(client.out_buf.size()) +
                                     " bytes). Evicting FD " + std::to_string(client.fd) + " (" + client.username + ")");
                client.closing = true;
                return false;
            }

            client.out_buf.append(msg);
            if (!client.epollout_armed) {
                update_client_epoll(client.fd, true);
            }
            return true;
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
            int nfds = ::epoll_wait(epoll_fd, events, MAX_EVENTS, 1000);
            if (nfds < 0) {
                if (errno == EINTR) {
                    if (!g_running) break;
                    continue;
                }
                break;
            }

            auto now = std::chrono::steady_clock::now();
            std::vector<int> close_list;

            for (int i = 0; i < nfds; ++i) {
                int event_fd = events[i].data.fd;
                uint32_t revents = events[i].events;

                // Timer tick: Idle client inspection
                if (event_fd == timer_fd) {
                    uint64_t expirations = 0;
                    ::read(timer_fd, &expirations, sizeof(expirations));

                    for (auto& [fd, cl] : clients) {
                        if (!cl.closing) {
                            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - cl.last_activity).count();
                            if (elapsed >= idle_timeout_sec) {
                                log_event("TIMEOUT", "Client " + (cl.is_registered ? cl.username : "unregistered") +
                                                     " (" + cl.peer_addr + ") timed out after " + std::to_string(elapsed) + "s idle");
                                std::string err_msg = net::ChatProtocol::serialize_error(408, "Connection timed out due to inactivity");
                                ::send(fd, err_msg.data(), err_msg.size(), MSG_NOSIGNAL);
                                cl.closing = true;
                                close_list.push_back(fd);
                            }
                        }
                    }
                    continue;
                }

                // Listening socket event
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

                        // RELIABILITY: Enforce maximum concurrent connections limit
                        if (clients.size() >= max_clients) {
                            log_event("REJECT", "Server capacity reached (" + std::to_string(max_clients) + "). Dropping new connection.");
                            std::string err_resp = net::ChatProtocol::serialize_error(503, "Server is full. Try again later.");
                            ::send(client_fd, err_resp.data(), err_resp.size(), MSG_NOSIGNAL);
                            ::close(client_fd);
                            continue;
                        }

                        struct epoll_event ev_client;
                        std::memset(&ev_client, 0, sizeof(ev_client));
                        ev_client.data.fd = client_fd;
                        ev_client.events = EPOLLIN | EPOLLRDHUP;
                        ::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev_client);

                        ClientSession session;
                        session.fd = client_fd;
                        session.peer_addr = net::format_address(client_addr);
                        session.last_activity = now;

                        log_event("CONNECT", "Accepted from " + session.peer_addr + " (FD " + std::to_string(client_fd) + ")");
                        clients[client_fd] = std::move(session);
                        queue_write(clients[client_fd], net::ChatProtocol::serialize_ok("Connected to Secure Chat Server."));
                    }
                    continue;
                }

                auto it = clients.find(event_fd);
                if (it == clients.end()) continue;
                ClientSession& cl = it->second;

                // 1. Drain pending reads (EPOLLIN) first so data sent prior to client half-close is processed
                if ((revents & EPOLLIN) && !cl.closing) {
                    char buf[4096];
                    while (true) {
                        ssize_t bytes = ::recv(event_fd, buf, sizeof(buf), 0);
                        if (bytes > 0) {
                            cl.in_buf.append(buf, static_cast<size_t>(bytes));
                            cl.last_activity = now;
                        } else if (bytes == 0) {
                            // Client sent FIN (orderly shutdown)
                            cl.closing = true;
                            break;
                        } else {
                            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                            if (errno == EINTR) continue;
                            cl.closing = true;
                            break;
                        }
                    }

                    bool oversized = false;
                    while (auto frame_opt = net::ChatProtocol::extract_next_frame(cl.in_buf, oversized)) {
                        net::ClientCommand cmd = net::ChatProtocol::parse_command(*frame_opt);

                        std::visit([&](auto&& arg) {
                            using T = std::decay_t<decltype(arg)>;
                            if constexpr (std::is_same_v<T, net::NickCmd>) {
                                const std::string& desired = arg.username;
                                if (!is_valid_username(desired)) {
                                    queue_write(cl, net::ChatProtocol::serialize_error(400, "Username must be 3-32 chars alphanumeric and underscores only"));
                                    return;
                                }

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
                                        log_event("NICK", cl.peer_addr + " registered as " + desired);
                                    } else {
                                        broadcast_to_registered(net::ChatProtocol::serialize_chat("SERVER", old_name + " changed nick to " + desired), event_fd);
                                    }
                                }
                            } else if constexpr (std::is_same_v<T, net::MsgCmd>) {
                                if (!cl.is_registered) {
                                    queue_write(cl, net::ChatProtocol::serialize_error(401, "Must register nickname first with NICK <name>"));
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
                                        queue_write(cl, net::ChatProtocol::serialize_ok());
                                    } else {
                                        queue_write(cl, net::ChatProtocol::serialize_error(404, "Target user not found"));
                                    }
                                }
                            } else if constexpr (std::is_same_v<T, net::UsersCmd>) {
                                std::vector<std::string> user_list;
                                for (const auto& [o_fd, o_cl] : clients) {
                                    if (o_cl.is_registered) user_list.push_back(o_cl.username);
                                }
                                queue_write(cl, net::ChatProtocol::serialize_users_list(user_list));
                            } else if constexpr (std::is_same_v<T, net::PingCmd>) {
                                queue_write(cl, net::ChatProtocol::serialize_pong(arg.payload));
                            } else if constexpr (std::is_same_v<T, net::QuitCmd>) {
                                queue_write(cl, net::ChatProtocol::serialize_ok("Goodbye"));
                                cl.closing = true;
                            } else if constexpr (std::is_same_v<T, net::MalformedCmd>) {
                                queue_write(cl, net::ChatProtocol::serialize_error(400, arg.reason));
                            }
                        }, cmd);
                    }

                    if (oversized) {
                        queue_write(cl, net::ChatProtocol::serialize_error(413, "Frame exceeds MAX_FRAME_SIZE"));
                    }
                }

                // 2. Flush pending writes (EPOLLOUT or immediate flush)
                if (!cl.out_buf.empty()) {
                    while (!cl.out_buf.empty()) {
                        ssize_t sent = ::send(event_fd, cl.out_buf.data(), cl.out_buf.size(), MSG_NOSIGNAL);
                        if (sent > 0) {
                            cl.out_buf.erase(0, static_cast<size_t>(sent));
                        } else if (sent < 0) {
                            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                            if (errno == EINTR) continue;
                            cl.closing = true;
                            break;
                        }
                    }
                    if (cl.out_buf.empty()) {
                        update_client_epoll(event_fd, false);
                    }
                }

                // 3. Check for disconnect / errors
                if ((revents & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) || cl.closing) {
                    if (cl.out_buf.empty() || (revents & (EPOLLERR | EPOLLHUP))) {
                        close_list.push_back(event_fd);
                    }
                }
            }

            for (int disc_fd : close_list) {
                auto it = clients.find(disc_fd);
                if (it != clients.end()) {
                    if (it->second.is_registered) {
                        broadcast_to_registered(net::ChatProtocol::serialize_leave(it->second.username), disc_fd);
                        log_event("DISCONNECT", it->second.username + " (" + it->second.peer_addr + ") left");
                    } else {
                        log_event("DISCONNECT", "Unregistered socket FD " + std::to_string(disc_fd) + " closed");
                    }
                    ::epoll_ctl(epoll_fd, EPOLL_CTL_DEL, disc_fd, nullptr);
                    ::close(disc_fd);
                    clients.erase(it);
                }
            }
        }

        log_event("SHUTDOWN", "Stopping server and terminating client connections...");
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
