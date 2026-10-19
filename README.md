# Linux Systems Programming: TCP Chat Server from Scratch

An educational systems-programming project exploring TCP/IP networking, Linux POSIX sockets, kernel buffers, I/O multiplexing (`select`, `epoll`), non-blocking I/O, multi-threading models, and concurrency.

## Project Road Map

- [x] **Stage 1: Basic TCP Server & Client (Single Client, Blocking I/O, POSIX Sockets)**
- [x] **Stage 2: Multi-Client Blocking Server (Thread-per-Client & Mutex Synchronization)**
- [ ] **Stage 3: Thread Pool & Work Queues (Bounded Worker Pool & Task Scheduling)**
- [ ] **Stage 4: Non-Blocking Sockets & `select()` (I/O Multiplexing & Buffer Tracking)**
- [ ] **Stage 5: High-Concurrency Linux `epoll` (Level & Edge Triggered Event Loop)**
- [ ] **Stage 6: Custom Chat Protocol (Framing, Serialization, Room & User Management)**
- [ ] **Stage 7: Reliability & Security (Flow Control, Backpressure, Slow-Client Eviction)**
- [ ] **Stage 8: Benchmarking & Profiling (Latency, Throughput, `perf`, `strace`, `ss`)**

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

## Running Stage 1

### Terminal 1: Run Server
```bash
./build/stage1_server -a 0.0.0.0 -p 8080
```

### Terminal 2: Connect using Client or Netcat
```bash
# Using the Stage 1 C++ client:
./build/stage1_client -a 127.0.0.1 -p 8080

# Or using netcat:
nc 127.0.0.1 8080
```

### Running Automated Test
```bash
./tests/stage1_test.sh
```

