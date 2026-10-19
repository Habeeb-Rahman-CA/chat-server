#!/usr/bin/env bash
set -euo pipefail

BENCH_BIN="./build/chat_benchmark"
STAGE6_BIN="./build/stage6_server"
STAGE7_BIN="./build/stage7_server"
PORT=19890

echo "=========================================================="
echo "    Running Automated Performance & Concurrency Benchmarks"
echo "=========================================================="

run_bench() {
    local SERVER_EXE="$1"
    local NAME="$2"
    local CLIENTS="$3"
    local MSGS="$4"
    PORT=$((PORT + 1))

    echo ""
    echo ">>> Benchmarking: ${NAME} (${CLIENTS} concurrent clients, ${MSGS} msgs/client on port ${PORT}) <<<"
    ${SERVER_EXE} -p ${PORT} > /tmp/bench_server.log 2>&1 &
    local PID=$!
    sleep 0.3

    ${BENCH_BIN} -p ${PORT} -c ${CLIENTS} -m ${MSGS}

    kill -INT ${PID} 2>/dev/null || true
    wait ${PID} 2>/dev/null || true
    sleep 0.2
}

run_bench "${STAGE6_BIN}" "Stage 6 Epoll Protocol Server" 50 30
run_bench "${STAGE7_BIN}" "Stage 7 Secure Epoll Server" 50 30
run_bench "${STAGE7_BIN}" "Stage 7 High-Concurrency Stress Test" 150 40

echo ""
echo "=========================================================="
echo "          All Benchmarks Completed Successfully!          "
echo "=========================================================="
