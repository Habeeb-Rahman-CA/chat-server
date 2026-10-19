#!/usr/bin/env bash
set -euo pipefail

PORT=19880
SERVER_BIN="./build/stage5_server"
SERVER_LOG="/tmp/stage5_server.log"

run_test() {
    local MODE_FLAG="$1"
    local DESC="$2"
    echo "=== Running Stage 5 epoll test (${DESC}) on port ${PORT} ==="

    ${SERVER_BIN} -p ${PORT} ${MODE_FLAG} > "${SERVER_LOG}" 2>&1 &
    SERVER_PID=$!

    cleanup() {
        if kill -0 ${SERVER_PID} 2>/dev/null; then
            kill -INT ${SERVER_PID} 2>/dev/null || true
            wait ${SERVER_PID} 2>/dev/null || true
        fi
    }
    trap cleanup EXIT

    sleep 0.2

    FIFO1="/tmp/ep1.fifo"
    FIFO2="/tmp/ep2.fifo"
    rm -f "${FIFO1}" "${FIFO2}"
    mkfifo "${FIFO1}" "${FIFO2}"

    nc 127.0.0.1 ${PORT} < "${FIFO1}" > /tmp/ep1.out 2>&1 &
    P1=$!
    exec 9> "${FIFO1}"

    nc 127.0.0.1 ${PORT} < "${FIFO2}" > /tmp/ep2.out 2>&1 &
    P2=$!
    exec 10> "${FIFO2}"

    sleep 0.2

    echo "/name Linus" >&9
    echo "/name Ken" >&10
    sleep 0.2

    echo "epoll is blazing fast!" >&9
    sleep 0.2

    echo "/msg Linus Unix rocks" >&10
    sleep 0.2

    echo "/list" >&9
    sleep 0.2

    echo "/quit" >&9
    echo "/quit" >&10
    sleep 0.2

    exec 9>&- || true
    exec 10>&- || true
    rm -f "${FIFO1}" "${FIFO2}"

    wait ${P1} 2>/dev/null || true
    wait ${P2} 2>/dev/null || true

    kill -INT ${SERVER_PID} 2>/dev/null || true
    wait ${SERVER_PID} 2>/dev/null || true
    trap - EXIT

    cat /tmp/ep1.out
    cat /tmp/ep2.out

    if grep -q "epoll is blazing fast!" /tmp/ep2.out && grep -q "Unix rocks" /tmp/ep1.out; then
        echo "=== epoll ${DESC} mode: PASSED! ==="
    else
        echo "=== epoll ${DESC} mode: FAILED! ==="
        exit 1
    fi
}

run_test "" "Level-Triggered (LT)"
PORT=19881
run_test "--et" "Edge-Triggered (ET)"

echo "=== All Stage 5 epoll tests PASSED! ==="

