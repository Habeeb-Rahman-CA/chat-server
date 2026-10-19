#include "net/ansi_colors.hpp"
#include "net/chat_protocol.hpp"
#include "net/socket.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace {

std::atomic<bool> g_running{true};
std::mutex g_cout_mutex;
std::mutex g_ping_mutex;
std::condition_variable g_ping_cv;
std::string g_current_input; // Tracks currently typed input so receiver doesn't scramble prompt
std::string g_current_channel = "#general";

void sigint_handler(int) {
    g_running = false;
    g_ping_cv.notify_all();
}

void print_banner() {
    std::cout << net::ui::BRIGHT_CYAN << net::ui::BOLD
              << R"(
  ██████╗██╗  ██╗ █████╗ ████████╗██╗   ██╗██╗
 ██╔════╝██║  ██║██╔══██╗╚══██╔══╝██║   ██║██║
 ██║     ███████║███████║   ██║   ██║   ██║██║
 ██║     ██╔══██║██╔══██║   ██║   ██║   ██║██║
 ╚██████╗██║  ██║██║  ██║   ██║   ╚██████╔╝██║
  ╚═════╝╚═╝  ╚═╝╚═╝  ╚═╝   ╚═╝    ╚═════╝ ╚═╝
)" << net::ui::RESET << "\n"
              << net::ui::DIM << " Terminal Multi-User TCP Chat Client v1.0.0" << net::ui::RESET << "\n"
              << net::ui::DIM << " Type " << net::ui::YELLOW << "/help" << net::ui::DIM << " for a list of available chat commands.\n"
              << "──────────────────────────────────────────────────────────\n" << net::ui::RESET;
}

void print_help() {
    std::cout << "\n" << net::ui::BOLD << "Available Chat Commands:" << net::ui::RESET << "\n"
              << "  " << net::ui::YELLOW << "<message>" << net::ui::RESET << "               Send a message to your current channel\n"
              << "  " << net::ui::YELLOW << "/join <#channel>" << net::ui::RESET << "       Switch/join a channel (e.g. /join #gaming)\n"
              << "  " << net::ui::YELLOW << "/rooms" << net::ui::RESET << "                 View all active channels\n"
              << "  " << net::ui::YELLOW << "/msg <user> <message>" << net::ui::RESET << "  Send a private whisper to a friend\n"
              << "  " << net::ui::YELLOW << "/users" << net::ui::RESET << "                 Show all online friends\n"
              << "  " << net::ui::YELLOW << "/nick <new_name>" << net::ui::RESET << "       Change your nickname\n"
              << "  " << net::ui::YELLOW << "/clear" << net::ui::RESET << "                 Clear terminal screen\n"
              << "  " << net::ui::YELLOW << "/quit" << net::ui::RESET << "                  Exit chat cleanly\n\n";
}

bool send_line(int fd, const std::string& line) {
    std::string to_send = line + "\n";
    size_t total = 0;
    while (total < to_send.size()) {
        ssize_t bytes = ::send(fd, to_send.data() + total, to_send.size() - total, MSG_NOSIGNAL);
        if (bytes <= 0) {
            if (errno == EINTR) continue;
            return false;
        }
        total += static_cast<size_t>(bytes);
    }
    return true;
}

// Background receiver loop
void receiver_thread_loop(int sock_fd) {
    constexpr size_t BUF_SIZE = 4096;
    char buffer[BUF_SIZE];
    std::string accumulator;

    while (g_running) {
        ssize_t bytes = ::recv(sock_fd, buffer, sizeof(buffer), 0);
        if (bytes > 0) {
            accumulator.append(buffer, static_cast<size_t>(bytes));

            size_t newline_pos;
            while ((newline_pos = accumulator.find('\n')) != std::string::npos) {
                std::string raw_line = accumulator.substr(0, newline_pos);
                accumulator.erase(0, newline_pos + 1);

                if (!raw_line.empty() && raw_line.back() == '\r') {
                    raw_line.pop_back();
                }
                if (raw_line.empty()) continue;

                // Format incoming protocol line with colors
                std::lock_guard<std::mutex> lock(g_cout_mutex);

                // Clear current prompt line before printing incoming message
                std::cout << net::ui::CLEAR_LINE;

                std::string_view sv = raw_line;
                std::string time_str = net::ui::current_timestamp();

                if (sv.starts_with("CHAT ")) {
                    // CHAT <sender> <text>
                    std::string_view rest = sv.substr(5);
                    size_t sp = rest.find(' ');
                    if (sp != std::string_view::npos) {
                        std::string_view sender = rest.substr(0, sp);
                        std::string_view text = rest.substr(sp + 1);

                        if (sender == "SERVER") {
                            std::cout << net::ui::DIM << time_str << " " << net::ui::BRIGHT_CYAN
                                      << "[SERVER] " << net::ui::RESET << text << "\n";
                        } else {
                            std::string_view color = net::ui::color_for_name(sender);
                            std::cout << net::ui::DIM << time_str << " " << color << net::ui::BOLD
                                      << "<" << sender << "> " << net::ui::RESET << text << "\n";
                        }
                    }
                } else if (sv.starts_with("WHISPER_FROM ")) {
                    // WHISPER_FROM <sender> <text>
                    std::string_view rest = sv.substr(13);
                    size_t sp = rest.find(' ');
                    if (sp != std::string_view::npos) {
                        std::string_view sender = rest.substr(0, sp);
                        std::string_view text = rest.substr(sp + 1);
                        std::cout << net::ui::DIM << time_str << " " << net::ui::BRIGHT_MAGENTA << net::ui::BOLD
                                  << "[WHISPER] <" << sender << " -> You>: " << net::ui::RESET << text << "\n";
                    }
                } else if (sv.starts_with("JOIN ")) {
                    std::string_view joined_user = sv.substr(5);
                    std::cout << net::ui::DIM << time_str << " " << net::ui::BRIGHT_GREEN
                              << "→ " << joined_user << " joined the chat" << net::ui::RESET << "\n";
                } else if (sv.starts_with("LEAVE ")) {
                    std::string_view left_user = sv.substr(6);
                    std::cout << net::ui::DIM << time_str << " " << net::ui::BRIGHT_RED
                              << "← " << left_user << " left the chat" << net::ui::RESET << "\n";
                } else if (sv.starts_with("USERS ")) {
                    // USERS <count> <user1> ...
                    std::string_view rest = sv.substr(6);
                    size_t sp = rest.find(' ');
                    std::string_view count_str = (sp != std::string_view::npos) ? rest.substr(0, sp) : rest;
                    std::string_view users = (sp != std::string_view::npos) ? rest.substr(sp + 1) : "";
                    std::cout << "\n" << net::ui::BOLD << net::ui::BRIGHT_CYAN
                              << "=== Online Friends (" << count_str << ") ===" << net::ui::RESET << "\n";
                    std::stringstream ss(std::string{users});
                    std::string user_token;
                    while (ss >> user_token) {
                        std::cout << "  • " << net::ui::color_for_name(user_token) << user_token << net::ui::RESET << "\n";
                    }
                    std::cout << net::ui::BRIGHT_CYAN << "===========================" << net::ui::RESET << "\n\n";
                } else if (sv.starts_with("ERR ")) {
                    std::cout << net::ui::BRIGHT_RED << net::ui::BOLD << "[ERROR] "
                              << net::ui::RESET << sv.substr(4) << "\n";
                } else if (sv.starts_with("OK")) {
                    if (sv.size() > 3) {
                        std::cout << net::ui::BRIGHT_GREEN << "[OK] " << net::ui::RESET
                                  << sv.substr(3) << "\n";
                    }
                }

                // Restore input prompt and current buffer
                std::cout << net::ui::BRIGHT_BLUE << "chat [" << g_current_channel << "]> "
                          << net::ui::RESET << g_current_input << std::flush;
            }
        } else if (bytes == 0) {
            std::lock_guard<std::mutex> lock(g_cout_mutex);
            std::cout << "\n" << net::ui::BRIGHT_YELLOW << "[Disconnected] Server closed the connection."
                      << net::ui::RESET << "\n";
            g_running = false;
            g_ping_cv.notify_all();
            break;
        } else {
            if (errno == EINTR) continue;
            g_running = false;
            g_ping_cv.notify_all();
            break;
        }
    }
}

void print_usage(const char* prog_name) {
    std::cout << "Usage: " << prog_name << " [options]\n"
              << "Options:\n"
              << "  -a, --address <ip>    Server IP address (default: 127.0.0.1)\n"
              << "  -p, --port <port>     Server port number (default: 8080)\n"
              << "  -u, --username <name> Pre-set chat username\n";
}

} // namespace

int main(int argc, char* argv[]) {
    std::string server_ip = "127.0.0.1";
    uint16_t port = 8080;
    std::string initial_user;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if ((arg == "-a" || arg == "--address") && i + 1 < argc) {
            server_ip = argv[++i];
        } else if ((arg == "-p" || arg == "--port") && i + 1 < argc) {
            port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if ((arg == "-u" || arg == "--username") && i + 1 < argc) {
            initial_user = argv[++i];
        }
    }

    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    ::sigaction(SIGINT, &sa, nullptr);
    ::signal(SIGPIPE, SIG_IGN);

    print_banner();

    try {
        std::cout << net::ui::DIM << "Connecting to " << server_ip << ":" << port << "..." << net::ui::RESET << "\n";
        net::Socket sock = net::create_tcp_socket();
        net::set_tcp_keepalive(sock.get(), 30, 5, 3);

        struct sockaddr_in serv_addr;
        if (!net::parse_ipv4(server_ip, port, serv_addr)) {
            std::cerr << net::ui::RED << "Error: Invalid server IP: " << server_ip << net::ui::RESET << "\n";
            return 1;
        }

        if (::connect(sock.get(), reinterpret_cast<struct sockaddr*>(&serv_addr), sizeof(serv_addr)) < 0) {
            std::cerr << net::ui::RED << "Could not connect to " << server_ip << ":" << port
                      << " (" << std::strerror(errno) << ")\n"
                      << "Make sure the chat server is running!" << net::ui::RESET << "\n";
            return 1;
        }

        std::cout << net::ui::GREEN << "✓ Connected successfully!" << net::ui::RESET << "\n\n";

        // Prompt for nickname if not passed on CLI
        std::string nickname = initial_user;
        while (nickname.empty() && g_running) {
            std::cout << net::ui::BOLD << "Enter your chat nickname: " << net::ui::RESET;
            if (!std::getline(std::cin, nickname)) return 0;
            while (!nickname.empty() && (nickname.back() == ' ' || nickname.back() == '\r')) nickname.pop_back();
            while (!nickname.empty() && nickname.front() == ' ') nickname.erase(0, 1);
        }

        // Register nickname
        send_line(sock.get(), "NICK " + nickname);

        // Start background receiver
        std::thread receiver(receiver_thread_loop, sock.get());

        // Background Keepalive Ping thread (sends PING every 25 seconds)
        std::thread pinger([sock_fd = sock.get()]() {
            std::unique_lock<std::mutex> lock(g_ping_mutex);
            while (g_running) {
                if (g_ping_cv.wait_for(lock, std::chrono::seconds(25), [] { return !g_running.load(); })) {
                    break;
                }
                send_line(sock_fd, "PING keepalive");
            }
        });

        // Foreground command / input loop
        std::string line;
        while (g_running) {
            {
                std::lock_guard<std::mutex> lock(g_cout_mutex);
                std::cout << net::ui::BRIGHT_BLUE << "chat [" << g_current_channel << "]> "
                          << net::ui::RESET << std::flush;
            }

            if (!std::getline(std::cin, line)) {
                // EOF (Ctrl+D)
                break;
            }

            if (line.empty()) continue;

            // Handle client-side commands
            if (line == "/help") {
                std::lock_guard<std::mutex> lock(g_cout_mutex);
                print_help();
                continue;
            } else if (line == "/clear") {
                std::cout << "\033[2J\033[1;1H" << std::flush;
                continue;
            } else if (line == "/quit" || line == "/exit") {
                send_line(sock.get(), "QUIT");
                break;
            } else if (line.starts_with("/nick ")) {
                std::string new_nick = line.substr(6);
                send_line(sock.get(), "NICK " + new_nick);
            } else if (line.starts_with("/msg ")) {
                // /msg <user> <message>
                std::string rest = line.substr(5);
                size_t sp = rest.find(' ');
                if (sp != std::string::npos && sp > 0) {
                    std::string target = rest.substr(0, sp);
                    std::string body = rest.substr(sp + 1);
                    send_line(sock.get(), "WHISPER " + target + " " + body);
                } else {
                    std::cout << net::ui::YELLOW << "Usage: /msg <username> <message>\n" << net::ui::RESET;
                }
            } else if (line == "/users" || line == "/list" || line == "/who") {
                send_line(sock.get(), "USERS");
            } else if (line.starts_with("/join ")) {
                std::string chan = line.substr(6);
                if (!chan.empty() && chan.front() != '#') chan = "#" + chan;
                g_current_channel = chan;
                send_line(sock.get(), "MSG /join " + chan);
            } else if (line == "/rooms" || line == "/channels") {
                send_line(sock.get(), "MSG /rooms");
            } else {
                // Regular chat message to current channel
                send_line(sock.get(), "MSG " + line);
            }
        }

        g_running = false;
        g_ping_cv.notify_all();
        ::shutdown(sock.get(), SHUT_RDWR);
        sock.close();

        if (receiver.joinable()) receiver.join();
        if (pinger.joinable()) pinger.join();

        std::cout << "\n" << net::ui::CYAN << "Thank you for chatting! Goodbye." << net::ui::RESET << "\n";

    } catch (const std::exception& ex) {
        std::cerr << "Fatal Exception: " << ex.what() << "\n";
        return 1;
    }

    return 0;
}
