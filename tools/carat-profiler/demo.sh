#!/usr/bin/env bash
# End-to-end demo of the Carat live profiler.
#   ./demo.sh <model.btor2> [algorithm] [extra carat args...]
# Starts the daemon, runs Carat against the model with tracing on, and
# opens the dashboard. Requires: built carat (build/carat) and
# tools/carat-profiler/server/carat-profiler (go build).
set -euo pipefail
cd "$(dirname "$0")/../.."

MODEL="${1:?usage: demo.sh <model.btor2> [algorithm] [extra args...]}"
ALG="${2:-ic3}"
shift 2 || true

SOCK=/tmp/carat-prof.sock
HTTP=127.0.0.1:8765
DATA=build/profiler-data

echo "== building daemon if needed"
if [ ! -x tools/carat-profiler/server/carat-profiler ]; then
    GO="${GO:-$(command -v go || echo /opt/homebrew/opt/go/libexec/bin/go)}"
    (cd tools/carat-profiler/server && "$GO" build -o carat-profiler .)
fi

echo "== starting daemon (http://$HTTP)"
pkill -f "carat-profiler serve" 2>/dev/null || true
sleep 0.3
tools/carat-profiler/server/carat-profiler serve \
    --socket "$SOCK" --http "$HTTP" --data "$DATA" &
DAEMON_PID=$!
trap 'kill $DAEMON_PID 2>/dev/null || true' EXIT
sleep 0.5

echo "== opening dashboard"
(open "http://$HTTP" 2>/dev/null || xdg-open "http://$HTTP" 2>/dev/null || true)

echo "== running carat ($ALG) with live tracing (Ctrl-C stops the demo)"
./build/carat "$MODEL" -a "$ALG" "$@" --trace-socket "$SOCK"
echo "== run finished; dashboard keeps serving history at http://$HTTP"
wait
