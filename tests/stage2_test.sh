#!/usr/bin/env bash
set -euo pipefail

PORT=19877
SERVER_BIN="./build/stage2_server"
SERVER_LOG="/tmp/stage2_server.log"
ALICE_OUT="/tmp/stage2_alice.out"
BOB_OUT="/tmp/stage2_bob.out"

echo "=== Running Stage 2 Multi-Client Automated Test on port ${PORT} ==="

if [[ ! -x "${SERVER_BIN}" ]]; then
    echo "Error: Server binary not found. Build first."
    exit 1
fi

# 1. Start server
echo "[1] Starting stage2_server in background..."
${SERVER_BIN} -p ${PORT} > "${SERVER_LOG}" 2>&1 &
SERVER_PID=$!

cleanup() {
    echo "Cleaning up processes..."
    if kill -0 ${SERVER_PID} 2>/dev/null; then
        kill -INT ${SERVER_PID} 2>/dev/null || true
        wait ${SERVER_PID} 2>/dev/null || true
    fi
}
trap cleanup EXIT

sleep 0.2

# 2. Check server is listening
echo "[2] Checking listening socket with ss..."
ss -tln "( sport = :${PORT} )"

# 3. Create FIFOs for bidirectional nc interaction
ALICE_FIFO="/tmp/alice.fifo"
BOB_FIFO="/tmp/bob.fifo"
rm -f "${ALICE_FIFO}" "${BOB_FIFO}" "${ALICE_OUT}" "${BOB_OUT}"
mkfifo "${ALICE_FIFO}" "${BOB_FIFO}"

# Launch Alice
nc 127.0.0.1 ${PORT} < "${ALICE_FIFO}" > "${ALICE_OUT}" 2>&1 &
ALICE_PID=$!
exec 3> "${ALICE_FIFO}"

# Launch Bob
nc 127.0.0.1 ${PORT} < "${BOB_FIFO}" > "${BOB_OUT}" 2>&1 &
BOB_PID=$!
exec 4> "${BOB_FIFO}"

sleep 0.3

# 4. Perform chat sequence
echo "[3] Executing chat actions..."
# Alice renames herself to Alice
echo "/name Alice" >&3
sleep 0.2

# Bob renames himself to Bob
echo "/name Bob" >&4
sleep 0.2

# Alice sends a public message
echo "Hello from Alice!" >&3
sleep 0.2

# Bob sends a private whisper to Alice
echo "/msg Alice Secret message from Bob" >&4
sleep 0.2

# Alice requests the user list
echo "/list" >&3
sleep 0.2

# Alice quits
echo "/quit" >&3
sleep 0.3

# Bob quits
echo "/quit" >&4
sleep 0.3

# Close FIFOs
exec 3>&- || true
exec 4>&- || true
rm -f "${ALICE_FIFO}" "${BOB_FIFO}"

# Wait for client processes
wait ${ALICE_PID} 2>/dev/null || true
wait ${BOB_PID} 2>/dev/null || true

echo "[4] Inspecting Alice output:"
cat "${ALICE_OUT}"
echo ""

echo "[5] Inspecting Bob output:"
cat "${BOB_OUT}"
echo ""

# 5. Assertions
echo "[6] Verifying test assertions..."

# Alice received Bob's whisper
if grep -q "\[Whisper from Bob\] Secret message from Bob" "${ALICE_OUT}"; then
    echo "  -> Alice received whisper from Bob [PASS]"
else
    echo "  -> Alice did NOT receive whisper [FAIL]"
    exit 1
fi

# Bob received Alice's broadcast
if grep -q "\[Alice\]: Hello from Alice!" "${BOB_OUT}"; then
    echo "  -> Bob received broadcast from Alice [PASS]"
else
    echo "  -> Bob did NOT receive broadcast [FAIL]"
    exit 1
fi

# Bob received Alice's departure notification
if grep -q "\*\*\* Alice has left the chat \*\*\*" "${BOB_OUT}"; then
    echo "  -> Bob received Alice departure notice [PASS]"
else
    echo "  -> Bob did NOT receive departure notice [FAIL]"
    exit 1
fi

echo ""
echo "=== Stage 2 Multi-Client Automated Test PASSED successfully! ==="
