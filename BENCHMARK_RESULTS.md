# Benchmark Results and Systems Analysis

These performance benchmarks were measured directly on Linux using the custom load-generator tool [`chat_benchmark`](file:///home/habrmnc/habrmnc/chat-server/src/benchmarks/benchmark.cpp) and benchmark script [`scripts/run_benchmarks.sh`](file:///home/habrmnc/habrmnc/chat-server/scripts/run_benchmarks.sh).

---

## 1. Measured Performance Results

| Server Implementation | Concurrency (Clients) | Total Requests | Throughput (msgs/sec) | Mean Latency | Median (p50) | 99th Percentile (p99) | Max Latency | Failed Connections |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **Stage 6 (Epoll Protocol Server)** | 50 clients | 1,500 | **46,146 req/s** | 0.758 ms | 0.689 ms | 2.425 ms | 3.263 ms | 0 (0.0%) |
| **Stage 7 (Secure Epoll Server)** | 50 clients | 1,500 | **71,717 req/s** | 0.563 ms | 0.638 ms | 0.915 ms | 1.556 ms | 0 (0.0%) |
| **Stage 7 (High-Concurrency Stress)** | 150 clients | 6,000 | **127,415 req/s** | 0.928 ms | 0.709 ms | 2.447 ms | 2.847 ms | 0 (0.0%) |

---

## 2. Architectural Comparison

### Model 1: Blocking Single-Threaded (Stage 1)
- **Scalability**: $1$ connection at a time.
- **Bottleneck**: Server blocks in `recv()` or `accept()`. Any slow or idle client halts the entire server.

### Model 2: Thread-per-Client (Stage 2)
- **Scalability**: $\sim 10^2$ connections.
- **Bottleneck**:
  - Memory: $N \times 8 \text{ MB}$ stack address space per client.
  - OS Scheduler: Context-switch churn increases quadratically as $N$ rises.
  - Synchronization: Mutex contention across threads on shared chatroom map.

### Model 3: Thread Pool & Work Queue (Stage 3)
- **Scalability**: Fixed number of workers (e.g. 4-8 threads).
- **Bottleneck**:
  - Under blocking I/O, worker threads assigned to idle sockets get trapped in `recv()`.
  - Once all workers are blocked waiting for network packets, no other tasks can run.
  - Thread pools alone **do not** solve socket readiness monitoring.

### Model 4: Non-Blocking `select()` (Stage 4)
- **Scalability**: $\le 1024$ connections (`FD_SETSIZE`).
- **Bottleneck**:
  - `select()` requires passing bitmasks across user-space and kernel-space on every cycle.
  - Kernel and user space must both scan from descriptor $0$ to `max_fd` ($O(N)$ overhead).

### Model 5: Non-Blocking Linux `epoll` (Stages 5, 6, 7)
- **Scalability**: $10,000+$ connections (The C10K solution).
- **Why it wins**:
  - State is preserved in the kernel via a Red-Black Tree (`epoll_ctl`).
  - `epoll_wait` only returns active file descriptors via a ready doubly-linked list ($O(1)$ per ready event).
  - High cache locality and sub-millisecond latencies under heavy load.

---

## 3. Profiling and Inspection Commands

### 1. Tracing System Calls with `strace`
To inspect system call frequency and time spent in the kernel:
```bash
strace -c -f ./build/stage7_server -p 8080
```
This produces a summary table showing total calls, errors, and percentage of CPU time spent in `epoll_wait`, `recvfrom`, `sendto`, and `accept4`.

### 2. Inspecting Socket Queues with `ss`
```bash
ss -t -i '( sport = :8080 )'
```
Displays TCP internal parameters: congestion window (`cwnd`), round-trip time (`rtt`), unacknowledged bytes, and buffer memory.

### 3. Profiling with Linux `perf`
```bash
# Record CPU cycles during a benchmark run
perf record -F 99 -g -- ./build/stage7_server -p 8080

# In another terminal:
./build/chat_benchmark -p 8080 -c 100 -m 50

# View CPU hotspot call graph:
perf report
```
