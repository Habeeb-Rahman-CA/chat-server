#!/usr/bin/env bash
set -euo pipefail

PORT=19879
SERVER_BIN="./build/stage4_server"
SERVER_LOG="/tmp/stage4_server.log"

echo "=== Running Stage 4 select() Multi-Client Automated Test on port ${PORT} ==="

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

FIFO1="/tmp/s4_c1.fifo"
FIFO2="/tmp/s4_c2.fifo"
rm -f "${FIFO1}" "${FIFO2}"
mkfifo "${FIFO1}" "${FIFO2}"

nc 127.0.0.1 ${PORT} < "${FIFO1}" > /tmp/s4_c1.out 2>&1 &
PID1=$!
exec 7> "${FIFO1}"

nc 127.0.0.1 ${PORT} < "${FIFO2}" > /tmp/s4_c2.out 2>&1 &
PID2=$!
exec 8> "${FIFO2}"

sleep 0.2

echo "/name Grace" >&7
echo "/name Ada" >&8
sleep 0.2

echo "Ada, are you there?" >&7
sleep 0.2

echo "/msg Grace Yes, non-blocking select works!" >&8
sleep 0.2

echo "/list" >&7
sleep 0.2

echo "/quit" >&7
echo "/quit" >&8
sleep 0.2

exec 7>&- || true
exec 8>&- || true
rm -f "${FIFO1}" "${FIFO2}"

wait ${PID1} 2>/dev/null || true
wait ${PID2} 2>/dev/null || true

cat /tmp/s4_c1.out
cat /tmp/s4_c2.out

if grep -q "Ada, are you there?" /tmp/s4_c2.out && grep -q "Yes, non-blocking select works!" /tmp/s4_c1.out; then
    echo "=== Stage 4 select() Test PASSED! ==="
else
    echo "=== Stage 4 select() Test FAILED! ==="
    exit 1
fi

