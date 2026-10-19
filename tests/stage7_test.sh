#!/usr/bin/env bash
set -euo pipefail

PORT=19883
SERVER_BIN="./build/stage7_server"
SERVER_LOG="/tmp/stage7_server.log"

echo "=== Running Stage 7 Reliability & Security Test on port ${PORT} ==="

# 1. Test Max Client Capacity & Username Validation
${SERVER_BIN} -p ${PORT} -m 2 -t 10 > "${SERVER_LOG}" 2>&1 &
SERVER_PID=$!

cleanup() {
    if kill -0 ${SERVER_PID} 2>/dev/null; then
        kill -INT ${SERVER_PID} 2>/dev/null || true
        wait ${SERVER_PID} 2>/dev/null || true
    fi
}
trap cleanup EXIT

sleep 0.2

echo "[1] Testing Invalid Username Rejection..."
RESP1=$(printf "NICK bad;name!\nQUIT\n" | nc 127.0.0.1 ${PORT})
echo "${RESP1}"
if echo "${RESP1}" | grep -q "ERR 400 Username must be 3-32"; then
    echo "  -> Invalid username properly rejected [PASS]"
else
    echo "  -> Invalid username NOT rejected [FAIL]"
    exit 1
fi

echo "[2] Testing Max Clients Limit (Capacity = 2)..."
FIFO1="/tmp/s7_1.fifo"
FIFO2="/tmp/s7_2.fifo"
rm -f "${FIFO1}" "${FIFO2}"
mkfifo "${FIFO1}" "${FIFO2}"

nc 127.0.0.1 ${PORT} < "${FIFO1}" > /tmp/s7_1.out 2>&1 &
P1=$!
exec 13> "${FIFO1}"

nc 127.0.0.1 ${PORT} < "${FIFO2}" > /tmp/s7_2.out 2>&1 &
P2=$!
exec 14> "${FIFO2}"

sleep 0.2
echo "NICK UserOne" >&13
echo "NICK UserTwo" >&14
sleep 0.2

# Try to connect 3rd client (should be rejected with 503)
RESP_OVERFLOW=$(printf "NICK UserThree\n" | nc 127.0.0.1 ${PORT} || true)
echo "Overflow client response: ${RESP_OVERFLOW}"
if echo "${RESP_OVERFLOW}" | grep -q "ERR 503 Server is full"; then
    echo "  -> Max client capacity enforcement: [PASS]"
else
    echo "  -> Max client capacity enforcement: [FAIL]"
    exit 1
fi

echo "QUIT" >&13
echo "QUIT" >&14
sleep 0.2
exec 13>&- || true
exec 14>&- || true
rm -f "${FIFO1}" "${FIFO2}"
wait ${P1} 2>/dev/null || true
wait ${P2} 2>/dev/null || true

kill -INT ${SERVER_PID} 2>/dev/null || true
wait ${SERVER_PID} 2>/dev/null || true
trap - EXIT

# 2. Test Idle Timeout
PORT=19884
echo "[3] Testing Idle Client Timeout (2 seconds)..."
${SERVER_BIN} -p ${PORT} -t 2 > /tmp/stage7_timeout.log 2>&1 &
SERVER_PID2=$!

sleep 0.2
# Connect client and sleep 3 seconds without sending data
TIMEOUT_RESP=$(timeout 4 nc 127.0.0.1 ${PORT} || true)
echo "Timeout response: ${TIMEOUT_RESP}"

kill -INT ${SERVER_PID2} 2>/dev/null || true
wait ${SERVER_PID2} 2>/dev/null || true

if echo "${TIMEOUT_RESP}" | grep -q "ERR 408 Connection timed out"; then
    echo "  -> Idle timeout enforcement: [PASS]"
else
    echo "  -> Idle timeout enforcement: [FAIL]"
    exit 1
fi

echo "=== All Stage 7 Reliability Tests PASSED! ==="
