#include "net/socket.hpp"

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
    // Closing the listening socket unblocks any thread blocked in accept()
    if (g_server_fd >= 0) {
        ::close(g_server_fd);
        g_server_fd = -1;
    }
}

// Represents a connected client session.
struct ClientSession {
    int fd;
    std::string username;
    std::string peer_addr;
    std::mutex send_mutex; // Prevents interleaved concurrent writes to the same client

    ClientSession(int f, std::string user, std::string addr)
        : fd(f), username(std::move(user)), peer_addr(std::move(addr)) {}

    // Safely send a message to this specific client
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

// Thread-safe Chat Room manager
class ChatRoom {
public:
    // Adds a new client. Returns false if username is taken.
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

    // Generates a unique default username like "user_1", "user_2"
    std::string generate_unique_name() {
        std::lock_guard<std::mutex> lock(mutex_);
        while (true) {
            std::string candidate = "user_" + std::to_string(next_user_id_++);
            if (!is_username_taken_locked(candidate)) {
                return candidate;
            }
        }
    }

    // Renames a client. Returns false if the new name is already in use.
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

    // Removes a client from the registry and returns the session
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

    // Broadcasts a message to all connected clients (except optionally the sender)
    void broadcast(const std::string& msg, int exclude_fd = -1) {
        std::vector<std::shared_ptr<ClientSession>> recipients;
        {
            // Keep critical section as short as possible: copy shared_ptrs, then send outside lock
            std::lock_guard<std::mutex> lock(mutex_);
            recipients.reserve(clients_.size());
            for (const auto& [fd, session] : clients_) {
                if (fd != exclude_fd) {
                    recipients.push_back(session);
                }
            }
        }

        // Send to each client with its own per-client send_mutex lock
        for (const auto& client : recipients) {
            client->send_message(msg);
        }
    }

    // Sends a private message to a specific user by name
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

        if (!target) {
            return false;
        }

        std::string priv_msg = "[Whisper from " + sender_name + "] " + content + "\n";
        target->send_message(priv_msg);

        // Echo confirmation to sender
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

    // Returns a list of all currently connected usernames
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
            if (session->username == name) {
                return true;
            }
        }
        return false;
    }

    std::mutex mutex_;
    std::unordered_map<int, std::shared_ptr<ClientSession>> clients_;
    uint64_t next_user_id_ = 1;
};

// Thread tracking for safe joining and cleanup on shutdown
class ThreadManager {
public:
    void track(std::thread&& t) {
        std::lock_guard<std::mutex> lock(mutex_);
        cleanup_finished_locked();
        threads_.push_back(std::move(t));
    }

    void join_all() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& t : threads_) {
            if (t.joinable()) {
                t.join();
            }
        }
        threads_.clear();
    }

private:
    void cleanup_finished_locked() {
        // Clean up joinable threads where possible
        // (For simplicity in this stage, we join all during join_all())
    }

    std::mutex mutex_;
    std::vector<std::thread> threads_;
};

// Trims trailing carriage returns and newlines
std::string_view trim_line(std::string_view sv) {
    while (!sv.empty() && (sv.back() == '\r' || sv.back() == '\n' || sv.back() == ' ')) {
        sv.remove_suffix(1);
    }
    while (!sv.empty() && sv.front() == ' ') {
        sv.remove_prefix(1);
    }
    return sv;
}

// Client worker thread function
void client_worker_thread(int client_fd, std::string peer_addr, ChatRoom& room) {
    // Wrap client fd in RAII socket so it closes when this worker exits
    net::Socket client_sock(client_fd);

    std::cout << "[Server:Thread " << std::this_thread::get_id() << "] Handling client "
              << peer_addr << " on FD " << client_fd << "\n";

    // Assign an initial unique username
    std::string username = room.generate_unique_name();
    std::shared_ptr<ClientSession> session;
    room.add_client(client_fd, peer_addr, username, session);

    // Send greeting and instructions
    std::string greeting = "Welcome to the Multi-Client Chat Server, " + username + "!\n"
                           "Commands: /name <new_name>, /msg <user> <message>, /list, /quit\n";
    session->send_message(greeting);

    // Broadcast join notification to all other clients
    room.broadcast("*** " + username + " has joined the chat (" + peer_addr + ") ***\n", client_fd);

    constexpr size_t BUFFER_SIZE = 4096;
    char buffer[BUFFER_SIZE];
    std::string accumulator;

    while (g_running) {
        // recv() blocks this specific worker thread waiting for data from this client
        ssize_t bytes_read = ::recv(client_fd, buffer, sizeof(buffer), 0);

        if (bytes_read > 0) {
            accumulator.append(buffer, static_cast<size_t>(bytes_read));

            // TCP Byte-Stream Processing:
            // Sockets deliver arbitrary chunks of bytes, NOT discrete application messages!
            // We search for newline delimiters ('\n') to process complete messages.
            size_t newline_pos;
            while ((newline_pos = accumulator.find('\n')) != std::string::npos) {
                std::string raw_line = accumulator.substr(0, newline_pos);
                accumulator.erase(0, newline_pos + 1);

                std::string_view line = trim_line(raw_line);
                if (line.empty()) continue;

                // Process application commands
                if (line == "/quit" || line == "quit" || line == "/exit") {
                    session->send_message("Goodbye!\n");
                    goto client_disconnect;
                } else if (line == "/list") {
                    session->send_message(room.get_user_list());
                } else if (line.starts_with("/name ")) {
                    std::string new_name = std::string(trim_line(line.substr(6)));
                    if (new_name.empty() || new_name.find(' ') != std::string::npos) {
                        session->send_message("Error: Username must be a non-empty word without spaces.\n");
                    } else {
                        std::string old_name;
                        if (room.rename_client(client_fd, new_name, old_name)) {
                            username = new_name;
                            session->send_message("Username changed to " + username + "\n");
                            room.broadcast("*** " + old_name + " is now known as " + new_name + " ***\n", client_fd);
                        } else {
                            session->send_message("Error: Username '" + new_name + "' is already taken.\n");
                        }
                    }
                } else if (line.starts_with("/msg ")) {
                    // Private message: /msg <target> <content>
                    std::string_view rest = line.substr(5);
                    size_t space_idx = rest.find(' ');
                    if (space_idx == std::string_view::npos || space_idx == 0) {
                        session->send_message("Usage: /msg <username> <message>\n");
                    } else {
                        std::string target(rest.substr(0, space_idx));
                        std::string content(trim_line(rest.substr(space_idx + 1)));
                        if (!room.send_private(username, target, content, client_fd)) {
                            session->send_message("Error: User '" + target + "' not found.\n");
                        }
                    }
                } else {
                    // Regular chat message: broadcast to all other users
                    std::string broadcast_msg = "[" + username + "]: " + std::string(line) + "\n";
                    std::cout << "[Chat] " << broadcast_msg;
                    room.broadcast(broadcast_msg, client_fd);
                }
            }
        } else if (bytes_read == 0) {
            // TCP FIN packet received: peer initiated graceful shutdown
            std::cout << "[Server] Client " << username << " (" << peer_addr << ") disconnected (EOF).\n";
            break;
        } else {
            // Error occurred
            if (errno == EINTR) {
                if (g_running) continue;
                break;
            } else if (errno == ECONNRESET) {
                std::cout << "[Server] Client " << username << " abruptly reset connection (ECONNRESET).\n";
                break;
            } else {
                std::cerr << "[Server] recv() error on " << username << ": " << std::strerror(errno) << "\n";
                break;
            }
        }
    }

client_disconnect:
    // Remove client from active registry
    room.remove_client(client_fd);
    room.broadcast("*** " + username + " has left the chat ***\n");

    std::cout << "[Server:Thread " << std::this_thread::get_id() << "] Finished handling "
              << username << " (FD " << client_fd << "). Active clients: " << room.client_count() << "\n";
    // client_sock destructor automatically calls ::close(client_fd) here
}

void print_usage(const char* prog_name) {
    std::cout << "Usage: " << prog_name << " [options]\n"
              << "Options:\n"
              << "  -h, --help            Show this help message\n"
              << "  -a, --address <ip>    Bind IP address (default: 0.0.0.0)\n"
              << "  -p, --port <port>     Bind port number (default: 8080)\n"
              << "  -b, --backlog <n>     Listen backlog size (default: 128)\n";
}

} // namespace

int main(int argc, char* argv[]) {
    std::string ip_str = "0.0.0.0";
    uint16_t port = 8080;
    int backlog = 128;

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
    ::sigaction(SIGTERM, &sa, nullptr);

    ::signal(SIGPIPE, SIG_IGN);

    try {
        std::cout << "========================================================\n";
        std::cout << "   Stage 2: Multi-Client Blocking Server (Thread-Per-Client) \n";
        std::cout << "========================================================\n";

        net::Socket server_sock = net::create_tcp_socket();
        g_server_fd = server_sock.get();
        net::set_reuse_addr(server_sock.get(), true);

        struct sockaddr_in bind_addr;
        if (!net::parse_ipv4(ip_str, port, bind_addr)) {
            std::cerr << "Error: Invalid IP: " << ip_str << "\n";
            return 1;
        }

        if (::bind(server_sock.get(), reinterpret_cast<struct sockaddr*>(&bind_addr), sizeof(bind_addr)) < 0) {
            std::cerr << "Error: bind() failed: " << std::strerror(errno) << "\n";
            return 1;
        }

        if (::listen(server_sock.get(), backlog) < 0) {
            std::cerr << "Error: listen() failed: " << std::strerror(errno) << "\n";
            return 1;
        }

        std::cout << "[Server] Listening on " << ip_str << ":" << port << " (Backlog: " << backlog << ")\n";
        std::cout << "[Server] Thread-per-client model active. Waiting for clients...\n\n";

        ChatRoom chat_room;
        ThreadManager thread_mgr;

        // Main thread accept loop
        while (g_running) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            std::memset(&client_addr, 0, sizeof(client_addr));

            int client_fd = ::accept(server_sock.get(),
                                     reinterpret_cast<struct sockaddr*>(&client_addr),
                                     &client_len);

            if (client_fd < 0) {
                if (errno == EINTR || !g_running) {
                    break;
                }
                if (errno == EBADF) {
                    // Socket was closed in signal handler
                    break;
                }
                std::cerr << "[Server] accept() error: " << std::strerror(errno) << "\n";
                continue;
            }

            std::string peer_addr = net::format_address(client_addr);
            std::cout << "[Server:Main] Accepted connection from " << peer_addr
                      << " (FD " << client_fd << "). Spawning worker thread...\n";

            // Spawn a new OS thread dedicated to this client connection
            std::thread worker(client_worker_thread, client_fd, peer_addr, std::ref(chat_room));
            thread_mgr.track(std::move(worker));
        }

        std::cout << "\n[Server] Shutdown signal received. Waiting for worker threads to terminate...\n";
        // Disconnect remaining clients by broadcasting shutdown
        chat_room.broadcast("*** Server is shutting down now ***\n");

        thread_mgr.join_all();
        std::cout << "[Server] All worker threads joined. Server stopped cleanly.\n";

    } catch (const std::exception& ex) {
        std::cerr << "Fatal Exception: " << ex.what() << "\n";
        return 1;
    }

    return 0;
}
