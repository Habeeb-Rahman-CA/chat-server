#include "net/socket.hpp"
#include "net/thread_pool.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

volatile std::sig_atomic_t g_running = 1;
int g_server_fd = -1;

void sigint_handler(int) {
    g_running = 0;
    if (g_server_fd >= 0) {
        ::close(g_server_fd);
        g_server_fd = -1;
    }
}

struct ClientSession {
    int fd;
    std::string username;
    std::string peer_addr;
    std::mutex send_mutex;

    ClientSession(int f, std::string user, std::string addr)
        : fd(f), username(std::move(user)), peer_addr(std::move(addr)) {}

    bool send_message(const std::string& msg) {
        std::lock_guard<std::mutex> lock(send_mutex);
        size_t total_sent = 0;
        while (total_sent < msg.size()) {
            ssize_t bytes = ::send(fd, msg.data() + total_sent, msg.size() - total_sent, MSG_NOSIGNAL);
            if (bytes < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            total_sent += static_cast<size_t>(bytes);
        }
        return true;
    }
};

class ChatRoom {
public:
    bool add_client(int fd, const std::string& peer_addr, const std::string& desired_username,
                    std::shared_ptr<ClientSession>& out_session) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (is_username_taken_locked(desired_username)) {
            return false;
        }
        auto session = std::make_shared<ClientSession>(fd, desired_username, peer_addr);
        clients_[fd] = session;
        out_session = session;
        return true;
    }

    std::string generate_unique_name() {
        std::lock_guard<std::mutex> lock(mutex_);
        while (true) {
            std::string candidate = "user_" + std::to_string(next_user_id_++);
            if (!is_username_taken_locked(candidate)) {
                return candidate;
            }
        }
    }

    bool rename_client(int fd, const std::string& new_name, std::string& old_name) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (is_username_taken_locked(new_name)) {
            return false;
        }
        auto it = clients_.find(fd);
        if (it != clients_.end()) {
            old_name = it->second->username;
            it->second->username = new_name;
            return true;
        }
        return false;
    }

    std::shared_ptr<ClientSession> remove_client(int fd) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = clients_.find(fd);
        if (it != clients_.end()) {
            auto session = it->second;
            clients_.erase(it);
            return session;
        }
        return nullptr;
    }

    void broadcast(const std::string& msg, int exclude_fd = -1) {
        std::vector<std::shared_ptr<ClientSession>> recipients;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            recipients.reserve(clients_.size());
            for (const auto& [fd, session] : clients_) {
                if (fd != exclude_fd) {
                    recipients.push_back(session);
                }
            }
        }
        for (const auto& client : recipients) {
            client->send_message(msg);
        }
    }

    bool send_private(const std::string& sender_name, const std::string& target_name,
                      const std::string& content, int sender_fd) {
        std::shared_ptr<ClientSession> target;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& [fd, session] : clients_) {
                if (session->username == target_name) {
                    target = session;
                    break;
                }
            }
        }
        if (!target) return false;

        std::string priv_msg = "[Whisper from " + sender_name + "] " + content + "\n";
        target->send_message(priv_msg);

        std::string confirm = "[Whisper to " + target_name + "] " + content + "\n";
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = clients_.find(sender_fd);
            if (it != clients_.end()) {
                it->second->send_message(confirm);
            }
        }
        return true;
    }

    std::string get_user_list() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::ostringstream oss;
        oss << "=== Online Users (" << clients_.size() << ") ===\n";
        for (const auto& [fd, session] : clients_) {
            oss << " - " << session->username << " (" << session->peer_addr << ")\n";
        }
        oss << "===========================\n";
        return oss.str();
    }

    size_t client_count() {
        std::lock_guard<std::mutex> lock(mutex_);
        return clients_.size();
    }

private:
    bool is_username_taken_locked(const std::string& name) const {
        for (const auto& [fd, session] : clients_) {
            if (session->username == name) return true;
        }
        return false;
    }

    std::mutex mutex_;
    std::unordered_map<int, std::shared_ptr<ClientSession>> clients_;
    uint64_t next_user_id_ = 1;
};

std::string_view trim_line(std::string_view sv) {
    while (!sv.empty() && (sv.back() == '\r' || sv.back() == '\n' || sv.back() == ' ')) sv.remove_suffix(1);
    while (!sv.empty() && sv.front() == ' ') sv.remove_prefix(1);
    return sv;
}

// Dedicated connection monitor thread that reads lines and offloads processing to the ThreadPool
void client_connection_handler(int client_fd, std::string peer_addr, ChatRoom& room, net::ThreadPool& pool) {
    net::Socket client_sock(client_fd);

    std::string username = room.generate_unique_name();
    std::shared_ptr<ClientSession> session;
    room.add_client(client_fd, peer_addr, username, session);

    std::string greeting = "Welcome to the Thread-Pool Chat Server, " + username + "!\n"
                           "Commands: /name <new_name>, /msg <user> <message>, /list, /quit\n";
    session->send_message(greeting);

    // Offload join broadcast to the thread pool worker queue!
    pool.submit([&room, username, peer_addr, client_fd]() {
        room.broadcast("*** " + username + " has joined the chat (" + peer_addr + ") ***\n", client_fd);
    });

    constexpr size_t BUFFER_SIZE = 4096;
    char buffer[BUFFER_SIZE];
    std::string accumulator;

    while (g_running) {
        ssize_t bytes_read = ::recv(client_fd, buffer, sizeof(buffer), 0);

        if (bytes_read > 0) {
            accumulator.append(buffer, static_cast<size_t>(bytes_read));
            size_t newline_pos;
            while ((newline_pos = accumulator.find('\n')) != std::string::npos) {
                std::string raw_line = accumulator.substr(0, newline_pos);
                accumulator.erase(0, newline_pos + 1);

                std::string_view line = trim_line(raw_line);
                if (line.empty()) continue;

                if (line == "/quit" || line == "quit" || line == "/exit") {
                    session->send_message("Goodbye!\n");
                    goto client_disconnect;
                }

                // Offload message processing and dispatch to the ThreadPool!
                // This keeps worker queue size bounded and decouples CPU-bound work
                // (e.g. message parsing, formatting, filtering) from the connection.
                std::string line_copy(line);
                bool submitted = pool.submit([&room, session, client_fd, line_copy, &username]() {
                    std::string_view l = line_copy;
                    if (l == "/list") {
                        session->send_message(room.get_user_list());
                    } else if (l.starts_with("/name ")) {
                        std::string new_name = std::string(trim_line(l.substr(6)));
                        if (new_name.empty() || new_name.find(' ') != std::string::npos) {
                            session->send_message("Error: Invalid username format.\n");
                        } else {
                            std::string old_name;
                            if (room.rename_client(client_fd, new_name, old_name)) {
                                username = new_name;
                                session->send_message("Username changed to " + username + "\n");
                                room.broadcast("*** " + old_name + " is now known as " + new_name + " ***\n", client_fd);
                            } else {
                                session->send_message("Error: Username '" + new_name + "' already taken.\n");
                            }
                        }
                    } else if (l.starts_with("/msg ")) {
                        std::string_view rest = l.substr(5);
                        size_t space_idx = rest.find(' ');
                        if (space_idx != std::string_view::npos && space_idx > 0) {
                            std::string target(rest.substr(0, space_idx));
                            std::string content(trim_line(rest.substr(space_idx + 1)));
                            if (!room.send_private(username, target, content, client_fd)) {
                                session->send_message("Error: User '" + target + "' not found.\n");
                            }
                        }
                    } else {
                        std::string msg = "[" + username + "]: " + line_copy + "\n";
                        room.broadcast(msg, client_fd);
                    }
                });

                if (!submitted) {
                    session->send_message("Server Error: Task queue full, message dropped.\n");
                }
            }
        } else {
            // Disconnect or error
            break;
        }
    }

client_disconnect:
    room.remove_client(client_fd);
    pool.submit([&room, username]() {
        room.broadcast("*** " + username + " has left the chat ***\n");
    });
}

} // namespace

int main(int argc, char* argv[]) {
    std::string ip_str = "0.0.0.0";
    uint16_t port = 8080;
    size_t pool_threads = 4;
    size_t queue_capacity = 256;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if ((arg == "-a" || arg == "--address") && i + 1 < argc) {
            ip_str = argv[++i];
        } else if ((arg == "-p" || arg == "--port") && i + 1 < argc) {
            port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if ((arg == "-t" || arg == "--threads") && i + 1 < argc) {
            pool_threads = static_cast<size_t>(std::stoi(argv[++i]));
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
        std::cout << "     Stage 3: Thread-Pool & Work-Queue TCP Server       \n";
        std::cout << "========================================================\n";

        net::Socket server_sock = net::create_tcp_socket();
        g_server_fd = server_sock.get();
        net::set_reuse_addr(server_sock.get(), true);

        struct sockaddr_in bind_addr;
        net::parse_ipv4(ip_str, port, bind_addr);
        ::bind(server_sock.get(), reinterpret_cast<struct sockaddr*>(&bind_addr), sizeof(bind_addr));
        ::listen(server_sock.get(), 128);

        std::cout << "[Server] Listening on " << ip_str << ":" << port << "\n";
        std::cout << "[Server] Worker Thread Pool: " << pool_threads
                  << " threads, Queue Capacity: " << queue_capacity << "\n\n";

        net::ThreadPool pool(pool_threads, queue_capacity, net::ThreadPool::OverflowPolicy::Reject);
        ChatRoom room;
        std::vector<std::thread> conn_threads;

        while (g_running) {
            struct sockaddr_in client_addr;
            socklen_t len = sizeof(client_addr);
            int client_fd = ::accept(server_sock.get(), reinterpret_cast<struct sockaddr*>(&client_addr), &len);
            if (client_fd < 0) {
                if (errno == EINTR || !g_running) break;
                if (errno == EBADF) break;
                continue;
            }

            std::string peer = net::format_address(client_addr);
            std::cout << "[Server:Main] New connection from " << peer << " (FD " << client_fd << ")\n";

            conn_threads.emplace_back(client_connection_handler, client_fd, peer, std::ref(room), std::ref(pool));
        }

        std::cout << "[Server] Shutting down. Joining threads...\n";
        for (auto& t : conn_threads) {
            if (t.joinable()) t.join();
        }
        pool.shutdown();
        std::cout << "[Server] Shutdown complete.\n";

    } catch (const std::exception& ex) {
        std::cerr << "Fatal Exception: " << ex.what() << "\n";
        return 1;
    }
    return 0;
}
