#!/bin/bash
# Qubic testnet entrypoint
# Starts the node, waits for the HTTP API, then sends F12 to enable MAIN mode.

set -e

QUBIC_DATA="${QUBIC_DATA_DIR:-/qubic}"
TICKING_DELAY="${TICKING_DELAY:-1000}"
API_URL="http://127.0.0.1:41841"
API_TIMEOUT=180  # seconds to wait for API to come up

mkdir -p "$QUBIC_DATA"

echo "=== Qubic Testnet Node ==="
echo "Data dir : $QUBIC_DATA"
echo "Tick delay: ${TICKING_DELAY}ms"

# Named pipe used to inject key presses into the Qubic process via stdin.
# fd 3 is kept open on the write side so the process never sees EOF.
STDIN_PIPE="$QUBIC_DATA/.qubic_stdin"
rm -f "$STDIN_PIPE"
mkfifo "$STDIN_PIPE"

# Keep the write end of the pipe open in fd 3 for the lifetime of this script.
exec 3<>"$STDIN_PIPE"

echo "Starting Qubic binary..."
/usr/local/bin/Qubic --ticking-delay "$TICKING_DELAY" <"$STDIN_PIPE" &
QUBIC_PID=$!
echo "Qubic started (PID $QUBIC_PID)"

# Wait for the HTTP RPC API to become available.
echo "Waiting for HTTP API at $API_URL ..."
WAITED=0
while ! curl -sf "$API_URL/live/v1/tick-info" > /dev/null 2>&1; do
    if ! kill -0 "$QUBIC_PID" 2>/dev/null; then
        echo "ERROR: Qubic process exited before API came up."
        exit 1
    fi
    sleep 2
    WAITED=$((WAITED + 2))
    if [ "$WAITED" -ge "$API_TIMEOUT" ]; then
        echo "ERROR: HTTP API did not become available within ${API_TIMEOUT}s."
        kill "$QUBIC_PID" 2>/dev/null || true
        exit 1
    fi
done

echo "HTTP API is up (waited ${WAITED}s)."

# Send F12 (ESC [ 2 4 ~) to switch the node to MAIN mode and start ticking.
# This is equivalent to pressing F12 at the console.
echo "Sending F12 → switching to MAIN mode..."
printf '\033[24~' >&3

echo "Node is ticking. RPC API: $API_URL/live/v1/tick-info"

# Forward SIGTERM / SIGINT to the Qubic process for clean shutdown.
trap 'echo "Shutting down..."; kill "$QUBIC_PID" 2>/dev/null; wait "$QUBIC_PID"' SIGTERM SIGINT

wait "$QUBIC_PID"
echo "Qubic process exited."
