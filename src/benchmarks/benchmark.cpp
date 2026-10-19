#include "net/socket.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

struct BenchResult {
    size_t total_sent = 0;
    size_t total_received = 0;
    size_t failed_connections = 0;
    std::vector<double> latencies_ms; // Round-trip latencies
};

void run_client_worker(const std::string& ip, uint16_t port, size_t client_id,
                        size_t messages_to_send, BenchResult& result) {
    try {
        net::Socket sock = net::create_tcp_socket();
        struct sockaddr_in serv_addr;
        if (!net::parse_ipv4(ip, port, serv_addr)) {
            result.failed_connections++;
            return;
        }

        if (::connect(sock.get(), reinterpret_cast<struct sockaddr*>(&serv_addr), sizeof(serv_addr)) < 0) {
            result.failed_connections++;
            return;
        }

        // Read initial server welcome banner
        char buffer[2048];
        ssize_t n = ::recv(sock.get(), buffer, sizeof(buffer) - 1, 0);
        if (n <= 0) {
            result.failed_connections++;
            return;
        }

        // Register username
        std::string nick = "bench_" + std::to_string(client_id);
        std::string reg_msg = "NICK " + nick + "\n";
        ::send(sock.get(), reg_msg.data(), reg_msg.size(), MSG_NOSIGNAL);
        ::recv(sock.get(), buffer, sizeof(buffer) - 1, 0);

        // Ping-Pong latency measurement loop
        for (size_t m = 0; m < messages_to_send; ++m) {
            auto start = std::chrono::steady_clock::now();
            std::string ping = "PING " + std::to_string(m) + "\n";

            ssize_t sent = ::send(sock.get(), ping.data(), ping.size(), MSG_NOSIGNAL);
            if (sent <= 0) break;
            result.total_sent++;

            ssize_t recvd = ::recv(sock.get(), buffer, sizeof(buffer) - 1, 0);
            if (recvd <= 0) break;
            result.total_received++;

            auto end = std::chrono::steady_clock::now();
            double latency_ms = std::chrono::duration<double, std::milli>(end - start).count();
            result.latencies_ms.push_back(latency_ms);
        }

        std::string quit_msg = "QUIT\n";
        ::send(sock.get(), quit_msg.data(), quit_msg.size(), MSG_NOSIGNAL);

    } catch (...) {
        result.failed_connections++;
    }
}

void print_usage(const char* prog_name) {
    std::cout << "Usage: " << prog_name << " [options]\n"
              << "Options:\n"
              << "  -a, --address <ip>     Server IP (default: 127.0.0.1)\n"
              << "  -p, --port <port>      Server Port (default: 8080)\n"
              << "  -c, --clients <n>      Number of concurrent clients (default: 50)\n"
              << "  -m, --messages <n>     Messages per client (default: 20)\n";
}

} // namespace

int main(int argc, char* argv[]) {
    std::string ip = "127.0.0.1";
    uint16_t port = 8080;
    size_t num_clients = 50;
    size_t messages_per_client = 20;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if ((arg == "-a" || arg == "--address") && i + 1 < argc) {
            ip = argv[++i];
        } else if ((arg == "-p" || arg == "--port") && i + 1 < argc) {
            port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if ((arg == "-c" || arg == "--clients") && i + 1 < argc) {
            num_clients = static_cast<size_t>(std::stoul(argv[++i]));
        } else if ((arg == "-m" || arg == "--messages") && i + 1 < argc) {
            messages_per_client = static_cast<size_t>(std::stoul(argv[++i]));
        }
    }

    std::cout << "========================================================\n";
    std::cout << "       TCP Chat Server Load Generator & Benchmark       \n";
    std::cout << "========================================================\n";
    std::cout << "Target: " << ip << ":" << port << "\n";
    std::cout << "Concurrent Clients: " << num_clients << "\n";
    std::cout << "Messages per Client: " << messages_per_client << "\n";
    std::cout << "Total Requests Planned: " << (num_clients * messages_per_client) << "\n\n";

    std::vector<BenchResult> results(num_clients);
    std::vector<std::thread> threads;
    threads.reserve(num_clients);

    auto bench_start = std::chrono::steady_clock::now();

    for (size_t i = 0; i < num_clients; ++i) {
        threads.emplace_back(run_client_worker, ip, port, i + 1, messages_per_client, std::ref(results[i]));
    }

    for (auto& t : threads) {
        if (t.joinable()) t.join();
    }

    auto bench_end = std::chrono::steady_clock::now();
    double total_time_sec = std::chrono::duration<double>(bench_end - bench_start).count();

    // Aggregate statistics
    size_t total_sent = 0;
    size_t total_received = 0;
    size_t failed_conns = 0;
    std::vector<double> all_latencies;

    for (const auto& r : results) {
        total_sent += r.total_sent;
        total_received += r.total_received;
        failed_conns += r.failed_connections;
        all_latencies.insert(all_latencies.end(), r.latencies_ms.begin(), r.latencies_ms.end());
    }

    std::sort(all_latencies.begin(), all_latencies.end());

    double min_lat = all_latencies.empty() ? 0 : all_latencies.front();
    double max_lat = all_latencies.empty() ? 0 : all_latencies.back();
    double p50 = all_latencies.empty() ? 0 : all_latencies[all_latencies.size() * 50 / 100];
    double p95 = all_latencies.empty() ? 0 : all_latencies[all_latencies.size() * 95 / 100];
    double p99 = all_latencies.empty() ? 0 : all_latencies[all_latencies.size() * 99 / 100];
    double avg_lat = all_latencies.empty() ? 0 : std::accumulate(all_latencies.begin(), all_latencies.end(), 0.0) / static_cast<double>(all_latencies.size());

    double throughput = (total_time_sec > 0) ? (static_cast<double>(total_received) / total_time_sec) : 0;

    std::cout << "---------------- Benchmark Summary ----------------\n";
    std::cout << "Wall Clock Time:       " << std::fixed << std::setprecision(3) << total_time_sec << " seconds\n";
    std::cout << "Successful Responses:  " << total_received << " / " << (num_clients * messages_per_client) << "\n";
    std::cout << "Failed Connections:    " << failed_conns << "\n";
    std::cout << "Throughput:            " << std::fixed << std::setprecision(1) << throughput << " req/sec (msgs/sec)\n";
    std::cout << "\nLatency Distribution (Round-Trip):\n";
    std::cout << "  Min:                 " << std::fixed << std::setprecision(3) << min_lat << " ms\n";
    std::cout << "  Mean:                " << avg_lat << " ms\n";
    std::cout << "  p50 (Median):        " << p50 << " ms\n";
    std::cout << "  p95:                 " << p95 << " ms\n";
    std::cout << "  p99:                 " << p99 << " ms\n";
    std::cout << "  Max:                 " << max_lat << " ms\n";
    std::cout << "===================================================\n";

    return (total_received > 0 && failed_conns == 0) ? 0 : 1;
}
