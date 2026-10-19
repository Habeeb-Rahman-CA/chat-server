#!/usr/bin/env bash
set -euo pipefail

PORT=19878
SERVER_BIN="./build/stage3_server"
SERVER_LOG="/tmp/stage3_server.log"

echo "=== Running Stage 3 Thread-Pool Automated Test on port ${PORT} ==="

${SERVER_BIN} -p ${PORT} -t 4 > "${SERVER_LOG}" 2>&1 &
SERVER_PID=$!

cleanup() {
    if kill -0 ${SERVER_PID} 2>/dev/null; then
        kill -INT ${SERVER_PID} 2>/dev/null || true
        wait ${SERVER_PID} 2>/dev/null || true
    fi
}
trap cleanup EXIT

sleep 0.2

# Run two clients via stage2_client (full duplex) or nc
FIFO1="/tmp/c1.fifo"
FIFO2="/tmp/c2.fifo"
rm -f "${FIFO1}" "${FIFO2}"
mkfifo "${FIFO1}" "${FIFO2}"

nc 127.0.0.1 ${PORT} < "${FIFO1}" > /tmp/c1.out 2>&1 &
PID1=$!
exec 5> "${FIFO1}"

nc 127.0.0.1 ${PORT} < "${FIFO2}" > /tmp/c2.out 2>&1 &
PID2=$!
exec 6> "${FIFO2}"

sleep 0.2

echo "/name Charlie" >&5
echo "/name Dave" >&6
sleep 0.2

echo "Greetings from Charlie!" >&5
sleep 0.2

echo "/msg Charlie Whisper from Dave" >&6
sleep 0.2

echo "/quit" >&5
echo "/quit" >&6
sleep 0.2

exec 5>&- || true
exec 6>&- || true
rm -f "${FIFO1}" "${FIFO2}"

wait ${PID1} 2>/dev/null || true
wait ${PID2} 2>/dev/null || true

cat /tmp/c1.out
cat /tmp/c2.out

if grep -q "Greetings from Charlie!" /tmp/c2.out && grep -q "Whisper from Dave" /tmp/c1.out; then
    echo "=== Stage 3 Thread-Pool Test PASSED! ==="
else
    echo "=== Stage 3 Thread-Pool Test FAILED! ==="
    exit 1
fi
