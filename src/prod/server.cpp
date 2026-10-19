#include "net/ansi_colors.hpp"
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
#include <sstream>
#include <string>
#include <string_view>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

volatile std::sig_atomic_t g_running = 1;

void sigint_handler(int) {
    g_running = 0;
}

constexpr size_t DEFAULT_MAX_CLIENTS = 2000;
constexpr size_t MAX_OUT_BUFFER_BYTES = 1024 * 1024; // 1 MB buffer limit per client
constexpr int DEFAULT_IDLE_TIMEOUT_SECONDS = 300;     // 5 minutes idle timeout
constexpr int RATE_LIMIT_MAX_PER_SEC = 10;            // Anti-spam limit

struct ClientSession {
    int fd = -1;
    std::string username;
    bool is_registered = false;
    std::string current_channel = "#general";
    std::string peer_addr;
    std::string in_buf;
    std::string out_buf;
    bool epollout_armed = false;
    bool closing = false;

    // Liveness & Rate limiting
    std::chrono::steady_clock::time_point last_activity;
    std::chrono::steady_clock::time_point rate_window_start;
    int messages_in_window = 0;
};

void log_server(std::string_view tag, std::string_view msg) {
    std::cout << net::ui::DIM << net::ui::current_timestamp() << net::ui::RESET << " "
              << net::ui::BOLD << "[" << tag << "]" << net::ui::RESET << " "
              << msg << "\n";
}

bool is_valid_username(std::string_view name) {
    if (name.size() < 2 || name.size() > 24) return false;
    for (char c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') {
            return false;
        }
    }
    return true;
}

bool is_valid_channel(std::string_view chan) {
    if (chan.size() < 2 || chan.size() > 24 || chan.front() != '#') return false;
    for (size_t i = 1; i < chan.size(); ++i) {
        char c = chan[i];
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') {
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
              << "  -m, --max-clients <n> Maximum concurrent clients (default: 2000)\n"
              << "  -t, --timeout <sec>   Idle timeout in seconds (default: 300)\n";
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
        std::cout << net::ui::BRIGHT_CYAN << net::ui::BOLD
                  << R"(
  ╔══════════════════════════════════════════════════════════════════╗
  ║            Production Linux Terminal TCP Chat Server             ║
  ╚══════════════════════════════════════════════════════════════════╝
)" << net::ui::RESET << "\n";

        net::Socket server_sock = net::create_tcp_socket();
        net::set_reuse_addr(server_sock.get(), true);
        net::set_nonblocking(server_sock.get(), true);

        struct sockaddr_in bind_addr;
        net::parse_ipv4(ip_str, port, bind_addr);
        if (::bind(server_sock.get(), reinterpret_cast<struct sockaddr*>(&bind_addr), sizeof(bind_addr)) < 0) {
            std::cerr << net::ui::RED << "Fatal: Failed to bind to " << ip_str << ":" << port
                      << " - " << std::strerror(errno) << net::ui::RESET << "\n";
            return 1;
        }

        if (::listen(server_sock.get(), SOMAXCONN) < 0) {
            std::cerr << net::ui::RED << "Fatal: listen() failed: " << std::strerror(errno) << net::ui::RESET << "\n";
            return 1;
        }

        int epoll_fd = ::epoll_create1(EPOLL_CLOEXEC);
        if (epoll_fd < 0) {
            std::cerr << "Fatal: epoll_create1 failed: " << std::strerror(errno) << "\n";
            return 1;
        }
        net::Socket epoll_raii(epoll_fd);

        struct epoll_event ev_listen;
        std::memset(&ev_listen, 0, sizeof(ev_listen));
        ev_listen.data.fd = server_sock.get();
        ev_listen.events = EPOLLIN;
        ::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_sock.get(), &ev_listen);

        // Timerfd for periodic idle client checks and keepalive
        int timer_fd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (timer_fd >= 0) {
            struct itimerspec ts;
            ts.it_interval.tv_sec = 2;
            ts.it_interval.tv_nsec = 0;
            ts.it_value.tv_sec = 2;
            ts.it_value.tv_nsec = 0;
            ::timerfd_settime(timer_fd, 0, &ts, nullptr);

            struct epoll_event ev_timer;
            std::memset(&ev_timer, 0, sizeof(ev_timer));
            ev_timer.data.fd = timer_fd;
            ev_timer.events = EPOLLIN;
            ::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, timer_fd, &ev_timer);
        }
        net::Socket timer_raii(timer_fd);

        log_server("ONLINE", "Chat server ready on " + ip_str + ":" + std::to_string(port));
        log_server("INFO", "Tell friends to connect with: ./chat_client -a <YOUR_IP> -p " + std::to_string(port));

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
            if (client.out_buf.size() + msg.size() > MAX_OUT_BUFFER_BYTES) {
                log_server("EVICT", "Client " + client.username + " out_buf exceeded 1MB. Disconnecting.");
                client.closing = true;
                return false;
            }
            client.out_buf.append(msg);
            if (!client.epollout_armed) {
                update_client_epoll(client.fd, true);
            }
            return true;
        };

        auto broadcast_channel = [&](std::string_view channel, const std::string& msg, int exclude_fd = -1) {
            for (auto& [fd, cl] : clients) {
                if (fd != exclude_fd && cl.is_registered && cl.current_channel == channel && !cl.closing) {
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

                if (event_fd == timer_fd) {
                    uint64_t expirations = 0;
                    ::read(timer_fd, &expirations, sizeof(expirations));

                    for (auto& [fd, cl] : clients) {
                        if (!cl.closing) {
                            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - cl.last_activity).count();
                            if (elapsed >= idle_timeout_sec) {
                                log_server("TIMEOUT", cl.username + " timed out after " + std::to_string(elapsed) + "s idle");
                                std::string err = net::ChatProtocol::serialize_error(408, "Disconnected due to inactivity");
                                ::send(fd, err.data(), err.size(), MSG_NOSIGNAL);
                                cl.closing = true;
                                close_list.push_back(fd);
                            }
                        }
                    }
                    continue;
                }

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

                        if (clients.size() >= max_clients) {
                            std::string err_resp = net::ChatProtocol::serialize_error(503, "Server full");
                            ::send(client_fd, err_resp.data(), err_resp.size(), MSG_NOSIGNAL);
                            ::close(client_fd);
                            continue;
                        }

                        // Enable TCP keepalive so firewalls don't kill idle connections
                        net::set_tcp_keepalive(client_fd, 30, 5, 3);

                        struct epoll_event ev_client;
                        std::memset(&ev_client, 0, sizeof(ev_client));
                        ev_client.data.fd = client_fd;
                        ev_client.events = EPOLLIN | EPOLLRDHUP;
                        ::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev_client);

                        ClientSession session;
                        session.fd = client_fd;
                        session.peer_addr = net::format_address(client_addr);
                        session.last_activity = now;
                        session.rate_window_start = now;

                        log_server("CONNECT", "Friend connected from " + session.peer_addr);
                        clients[client_fd] = std::move(session);
                        queue_write(clients[client_fd], net::ChatProtocol::serialize_ok("Connected to Modern Chat Server. Choose a username via NICK <name>"));
                    }
                    continue;
                }

                auto it = clients.find(event_fd);
                if (it == clients.end()) continue;
                ClientSession& cl = it->second;

                // 1. Drain input
                if ((revents & EPOLLIN) && !cl.closing) {
                    char buf[4096];
                    while (true) {
                        ssize_t bytes = ::recv(event_fd, buf, sizeof(buf), 0);
                        if (bytes > 0) {
                            cl.in_buf.append(buf, static_cast<size_t>(bytes));
                            cl.last_activity = now;
                        } else if (bytes == 0) {
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
                        std::string clean_frame = net::ui::sanitize_message(*frame_opt);
                        if (clean_frame.empty()) continue;

                        // Rate limiting check
                        auto window_elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - cl.rate_window_start).count();
                        if (window_elapsed >= 1) {
                            cl.rate_window_start = now;
                            cl.messages_in_window = 1;
                        } else {
                            cl.messages_in_window++;
                            if (cl.messages_in_window > RATE_LIMIT_MAX_PER_SEC) {
                                queue_write(cl, net::ChatProtocol::serialize_error(429, "Rate limit exceeded (slow down!)"));
                                continue;
                            }
                        }

                        // Parse command
                        net::ClientCommand cmd = net::ChatProtocol::parse_command(clean_frame);

                        std::visit([&](auto&& arg) {
                            using T = std::decay_t<decltype(arg)>;
                            if constexpr (std::is_same_v<T, net::NickCmd>) {
                                const std::string& desired = arg.username;
                                if (!is_valid_username(desired)) {
                                    queue_write(cl, net::ChatProtocol::serialize_error(400, "Username must be 2-24 alphanumeric characters, underscores or dashes"));
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
                                    queue_write(cl, net::ChatProtocol::serialize_error(409, "Nickname '" + desired + "' already taken"));
                                } else {
                                    bool was_reg = cl.is_registered;
                                    std::string old_nick = cl.username;
                                    cl.username = desired;
                                    cl.is_registered = true;
                                    queue_write(cl, net::ChatProtocol::serialize_ok("Registered as " + desired));

                                    if (!was_reg) {
                                        broadcast_channel(cl.current_channel, net::ChatProtocol::serialize_join(desired), event_fd);
                                        log_server("NICK", cl.peer_addr + " set nickname to " + desired);
                                    } else {
                                        broadcast_channel(cl.current_channel, net::ChatProtocol::serialize_chat("SERVER", old_nick + " is now known as " + desired), event_fd);
                                    }
                                }
                            } else if constexpr (std::is_same_v<T, net::MsgCmd>) {
                                if (!cl.is_registered) {
                                    queue_write(cl, net::ChatProtocol::serialize_error(401, "Please choose a nickname first: NICK <your_name>"));
                                } else {
                                    std::string_view msg_text = arg.text;
                                    // Custom commands support
                                    if (msg_text.starts_with("/join ")) {
                                        std::string target_chan = std::string(msg_text.substr(6));
                                        if (!is_valid_channel(target_chan)) {
                                            queue_write(cl, net::ChatProtocol::serialize_error(400, "Channel format must start with # and be 2-24 chars"));
                                        } else {
                                            std::string old_chan = cl.current_channel;
                                            broadcast_channel(old_chan, net::ChatProtocol::serialize_chat("SERVER", cl.username + " left " + old_chan), event_fd);
                                            cl.current_channel = target_chan;
                                            broadcast_channel(target_chan, net::ChatProtocol::serialize_chat("SERVER", cl.username + " joined " + target_chan), event_fd);
                                            queue_write(cl, net::ChatProtocol::serialize_ok("Joined channel " + target_chan));
                                            log_server("CHANNEL", cl.username + " switched to " + target_chan);
                                        }
                                    } else if (msg_text == "/rooms" || msg_text == "/channels") {
                                        std::unordered_map<std::string, int> channel_counts;
                                        for (const auto& [o_fd, o_cl] : clients) {
                                            if (o_cl.is_registered) channel_counts[o_cl.current_channel]++;
                                        }
                                        std::ostringstream oss;
                                        oss << "=== Active Channels ===\n";
                                        for (const auto& [ch, count] : channel_counts) {
                                            oss << " " << ch << " (" << count << " users)\n";
                                        }
                                        queue_write(cl, net::ChatProtocol::serialize_chat("SERVER", oss.str()));
                                    } else {
                                        // Broadcast message to users in the same channel
                                        std::string formatted = "[" + cl.current_channel + "] " + arg.text;
                                        broadcast_channel(cl.current_channel, net::ChatProtocol::serialize_chat(cl.username, formatted), event_fd);
                                        queue_write(cl, net::ChatProtocol::serialize_ok());
                                    }
                                }
                            } else if constexpr (std::is_same_v<T, net::WhisperCmd>) {
                                if (!cl.is_registered) {
                                    queue_write(cl, net::ChatProtocol::serialize_error(401, "Choose a nickname first"));
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
                                        queue_write(cl, net::ChatProtocol::serialize_error(404, "User '" + arg.target + "' not found"));
                                    }
                                }
                            } else if constexpr (std::is_same_v<T, net::UsersCmd>) {
                                std::vector<std::string> user_list;
                                for (const auto& [o_fd, o_cl] : clients) {
                                    if (o_cl.is_registered) {
                                        user_list.push_back(o_cl.username + "(" + o_cl.current_channel + ")");
                                    }
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
                        queue_write(cl, net::ChatProtocol::serialize_error(413, "Message exceeded maximum allowable frame size"));
                    }
                }

                // 2. Flush output
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

                // 3. Disconnect handling
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
                        broadcast_channel(it->second.current_channel, net::ChatProtocol::serialize_leave(it->second.username), disc_fd);
                        log_server("DISCONNECT", it->second.username + " disconnected");
                    }
                    ::epoll_ctl(epoll_fd, EPOLL_CTL_DEL, disc_fd, nullptr);
                    ::close(disc_fd);
                    clients.erase(it);
                }
            }
        }

        log_server("SHUTDOWN", "Broadcasting server shutdown notice to friends...");
        std::string bye_msg = net::ChatProtocol::serialize_chat("SERVER", "Server is shutting down for maintenance. Goodbye!");
        for (auto& [fd, cl] : clients) {
            ::send(fd, bye_msg.data(), bye_msg.size(), MSG_NOSIGNAL);
            ::epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
            ::close(fd);
        }

    } catch (const std::exception& ex) {
        std::cerr << "Fatal Exception: " << ex.what() << "\n";
        return 1;
    }

    return 0;
}
