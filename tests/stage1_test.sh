#!/usr/bin/env bash
set -euo pipefail

PORT=19876
SERVER_BIN="./build/stage1_server"
CLIENT_BIN="./build/stage1_client"

echo "=== Running Stage 1 Automated Test on port ${PORT} ==="

if [[ ! -x "${SERVER_BIN}" || ! -x "${CLIENT_BIN}" ]]; then
    echo "Error: Binaries not found. Run 'cmake --build build' first."
    exit 1
fi

# 1. Start server in background with --once
echo "[1] Starting stage1_server..."
${SERVER_BIN} -p ${PORT} --once > /tmp/stage1_server.log 2>&1 &
SERVER_PID=$!

cleanup() {
    if kill -0 ${SERVER_PID} 2>/dev/null; then
        kill -9 ${SERVER_PID} 2>/dev/null || true
    fi
}
trap cleanup EXIT

# Wait briefly for server to bind and listen
sleep 0.2

# 2. Check that socket is in LISTEN state using ss
echo "[2] Inspecting socket state via 'ss'..."
ss -tln "( sport = :${PORT} )"

# 3. Run client and send messages
echo "[3] Connecting client and sending test messages..."
OUTPUT=$(printf "Hello Kernel!\nSystems Programming\nquit\n" | ${CLIENT_BIN} -p ${PORT})

echo "[4] Client output:"
echo "${OUTPUT}"

# 4. Verify expected responses
echo "[5] Verifying echoes..."
if echo "${OUTPUT}" | grep -q "\[Echo\] Hello Kernel!"; then
    echo "  -> Found [Echo] Hello Kernel! [PASS]"
else
    echo "  -> Missing [Echo] Hello Kernel! [FAIL]"
    exit 1
fi

if echo "${OUTPUT}" | grep -q "\[Echo\] Systems Programming"; then
    echo "  -> Found [Echo] Systems Programming [PASS]"
else
    echo "  -> Missing [Echo] Systems Programming [FAIL]"
    exit 1
fi

# Wait for server to exit
wait ${SERVER_PID} || true

echo ""
echo "[6] Server log output:"
cat /tmp/stage1_server.log

echo ""
echo "=== Stage 1 Automated Test PASSED successfully! ==="

