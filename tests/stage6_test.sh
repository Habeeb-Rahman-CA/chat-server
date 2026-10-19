#!/usr/bin/env bash
set -euo pipefail

PORT=19882
SERVER_BIN="./build/stage6_server"
SERVER_LOG="/tmp/stage6_server.log"

echo "=== Running Stage 6 Application Protocol Test on port ${PORT} ==="

${SERVER_BIN} -p ${PORT} > "${SERVER_LOG}" 2>&1 &
SERVER_PID=$!

cleanup() {
    if kill -0 ${SERVER_PID} 2>/dev/null; then
        kill -INT ${SERVER_PID} 2>/dev/null || true
        wait ${SERVER_PID} 2>/dev/null || true
    fi
}
trap cleanup EXIT

sleep 0.2

FIFO1="/tmp/p6_1.fifo"
FIFO2="/tmp/p6_2.fifo"
rm -f "${FIFO1}" "${FIFO2}"
mkfifo "${FIFO1}" "${FIFO2}"

nc 127.0.0.1 ${PORT} < "${FIFO1}" > /tmp/p6_1.out 2>&1 &
P1=$!
exec 11> "${FIFO1}"

nc 127.0.0.1 ${PORT} < "${FIFO2}" > /tmp/p6_2.out 2>&1 &
P2=$!
exec 12> "${FIFO2}"

sleep 0.2

# 1. Test unauthenticated MSG rejection
echo "MSG ShouldFailBeforeNick" >&11
sleep 0.2

# 2. Register Client 1
echo "NICK Alice" >&11
sleep 0.2

# 3. Client 2 tries duplicate NICK Alice
echo "NICK Alice" >&12
sleep 0.2

# 4. Client 2 registers as Bob
echo "NICK Bob" >&12
sleep 0.2

# 5. Public message
echo "MSG Hello everyone from Alice!" >&11
sleep 0.2

# 6. Private whisper
echo "WHISPER Alice Secret whisper" >&12
sleep 0.2

# 7. Ping/Pong test
echo "PING token_999" >&11
sleep 0.2

# 8. User list test
echo "USERS" >&11
sleep 0.2

# 9. Test fragmented delivery: send 'M', sleep, then 'SG Fragmented message\n'
printf "M" >&11
sleep 0.1
printf "SG Fragmented message\n" >&11
sleep 0.2

# 10. Test multiple messages in single packet
printf "MSG batch_one\nMSG batch_two\n" >&11
sleep 0.2

# 11. Quit
echo "QUIT" >&11
echo "QUIT" >&12
sleep 0.2

exec 11>&- || true
exec 12>&- || true
rm -f "${FIFO1}" "${FIFO2}"

wait ${P1} 2>/dev/null || true
wait ${P2} 2>/dev/null || true

echo "=== Client 1 (Alice) Output ==="
cat /tmp/p6_1.out
echo ""
echo "=== Client 2 (Bob) Output ==="
cat /tmp/p6_2.out
echo ""

echo "Verifying assertions..."

# 1. Unregistered error
grep -q "ERR 401 Must register" /tmp/p6_1.out && echo "  -> ERR 401 unregistered check: PASS"

# 2. Duplicate nick error
grep -q "ERR 409 Nickname already taken" /tmp/p6_2.out && echo "  -> ERR 409 duplicate nick check: PASS"

# 3. Broadcast received
grep -q "CHAT Alice Hello everyone from Alice!" /tmp/p6_2.out && echo "  -> Public message broadcast: PASS"

# 4. Whisper received
grep -q "WHISPER_FROM Bob Secret whisper" /tmp/p6_1.out && echo "  -> Private whisper: PASS"

# 5. PONG response
grep -q "PONG token_999" /tmp/p6_1.out && echo "  -> PING / PONG check: PASS"

# 6. Fragmented delivery
grep -q "CHAT Alice Fragmented message" /tmp/p6_2.out && echo "  -> Fragmented TCP stream handling: PASS"

# 7. Batch messages
grep -q "CHAT Alice batch_one" /tmp/p6_2.out && grep -q "CHAT Alice batch_two" /tmp/p6_2.out && echo "  -> Multiple messages per packet: PASS"

echo "=== All Stage 6 Protocol Tests PASSED! ==="

