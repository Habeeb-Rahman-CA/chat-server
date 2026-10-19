# Linux Systems Programming: TCP Chat Server from Scratch

An educational systems-programming project exploring TCP/IP networking, Linux POSIX sockets, kernel buffers, I/O multiplexing (`select`, `epoll`), non-blocking I/O, multi-threading models, and concurrency.

## Project Road Map

- [x] **Stage 1: Basic TCP Server & Client (Single Client, Blocking I/O, POSIX Sockets)**
- [x] **Stage 2: Multi-Client Blocking Server (Thread-per-Client & Mutex Synchronization)**
- [x] **Stage 3: Thread Pool & Work Queues (Bounded Worker Pool & Task Scheduling)**
- [x] **Stage 4: Non-Blocking Sockets & `select()` (I/O Multiplexing & Buffer Tracking)**
- [x] **Stage 5: High-Concurrency Linux `epoll` (Level & Edge Triggered Event Loop)**
- [x] **Stage 6: Custom Chat Protocol (Framing, Serialization, Room & User Management)**
- [x] **Stage 7: Reliability & Security (Flow Control, Backpressure, Slow-Client Eviction)**
- [x] **Stage 8: Benchmarking & Profiling (Latency, Throughput, `perf`, `strace`, `ss`)**

---

## Directory Structure

```
chat-server/
├── CMakeLists.txt          # Root build configuration (C++20, strict warnings)
├── README.md               # Architecture documentation & guides
├── include/
│   └── net/
│       └── socket.hpp      # RAII Socket wrapper & POSIX networking utilities
├── src/
│   ├── common/
│   │   └── socket.cpp      # Implementation of address parsing and socket options
│   └── stage1/
│       ├── server.cpp      # Stage 1: Single-client blocking TCP echo server
│       └── client.cpp      # Stage 1: Interactive terminal TCP client
└── tests/
    └── stage1_test.sh      # Automated integration test script for Stage 1
```

---

## Building

```bash
# Configure build directory
cmake -B build -S .

# Compile all targets
cmake --build build
```

---

## Running the Different Architectures
 
### Stage 1: Basic TCP Server & Client
```bash
./build/stage1_server -a 0.0.0.0 -p 8080
./build/stage1_client -a 127.0.0.1 -p 8080
```

### Stage 2: Thread-per-Client Server & Full-Duplex Client
```bash
./build/stage2_server -p 8080
./build/stage2_client -p 8080
```

### Stage 3: Thread-Pool & Work-Queue Server
```bash
./build/stage3_server -p 8080 -t 4
```

### Stage 4: Non-Blocking select() Server
```bash
./build/stage4_server -p 8080
```

### Stage 5: Linux epoll Server (Level & Edge Triggered)
```bash
# Level-Triggered:
./build/stage5_server -p 8080

# Edge-Triggered:
./build/stage5_server -p 8080 --et
```

### Stage 6: Application Protocol Server
```bash
./build/stage6_server -p 8080
```

### Stage 7: Secure, Reliable Production-Grade Server
```bash
./build/stage7_server -p 8080 -m 1000 -t 60
```

### Stage 8: Load Generator & Benchmark Suite
```bash
# Run benchmark against any running server
./build/chat_benchmark -p 8080 -c 50 -m 30

# Or execute full automated comparison script:
./scripts/run_benchmarks.sh
```

---

## Running Automated Tests

```bash
./tests/stage1_test.sh
./tests/stage2_test.sh
./tests/stage3_test.sh
./tests/stage4_test.sh
./tests/stage5_test.sh
./tests/stage6_test.sh
./tests/stage7_test.sh
```

