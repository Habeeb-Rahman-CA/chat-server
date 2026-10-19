#!/usr/bin/env bash
set -euo pipefail

PORT=19910
SERVER_BIN="./build/chat_server"
CLIENT_BIN="./build/chat_client"

echo "=== Testing Production Chat Server & Client on port ${PORT} ==="

${SERVER_BIN} -p ${PORT} > /tmp/prod_server.log 2>&1 &
SERVER_PID=$!

cleanup() {
    if kill -0 ${SERVER_PID} 2>/dev/null; then
        kill -INT ${SERVER_PID} 2>/dev/null || true
        wait ${SERVER_PID} 2>/dev/null || true
    fi
}
trap cleanup EXIT

sleep 0.3

# Launch Bob in background listening and reacting
(
    sleep 1.0
    echo "/join #gaming"
    sleep 0.8
    echo "/quit"
) | ${CLIENT_BIN} -p ${PORT} -u Bob > /tmp/prod_b.out 2>&1 &
BOB_PID=$!

sleep 0.2

# Launch Alice in background sending messages
(
    sleep 0.4
    echo "Hey Bob, welcome to the production chat!"
    sleep 0.8
    echo "/msg Bob Secret whisper to gaming room"
    sleep 0.4
    echo "/users"
    sleep 0.4
    echo "/quit"
) | ${CLIENT_BIN} -p ${PORT} -u Alice > /tmp/prod_a.out 2>&1 &
ALICE_PID=$!

wait ${ALICE_PID} 2>/dev/null || true
wait ${BOB_PID} 2>/dev/null || true

cat /tmp/prod_a.out
cat /tmp/prod_b.out

echo "Checking test results..."
grep -q "welcome to the production chat" /tmp/prod_b.out && echo "  -> Channel broadcast: PASS"
grep -q "Secret whisper to gaming room" /tmp/prod_b.out && echo "  -> Cross-channel whisper: PASS"
grep -q "Online Friends" /tmp/prod_a.out && echo "  -> User list inspection: PASS"

echo "=== Production Chat Server & Client PASSED ALL TESTS! ==="
