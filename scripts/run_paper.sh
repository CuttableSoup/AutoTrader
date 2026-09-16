#!/usr/bin/env bash
# Start the paper stack on Linux/macOS under one process group. Ctrl-C stops everything.
# Prerequisites: .venv (uv pip install -e python -e watchdog), tools/nats/nats-server, C++ build in $BUILD_DIR,
# config/paper.json, config/watchdog.json.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
BUILD_DIR="${BUILD_DIR:-cpp/build/linux-release/bin}"
CONFIG="${CONFIG:-config/paper.json}"
PY="$ROOT/.venv/bin/python"
mkdir -p var/jetstream var/log var/state
pids=()
start() { echo "starting $1"; shift; "$@" >> "var/log/$1.stdout.log" 2>&1 & pids+=($!); }
trap 'echo "stopping"; kill "${pids[@]}" 2>/dev/null || true; wait' INT TERM

start nats tools/nats/nats-server -c config/nats/nats-server.conf
sleep 2
"$PY" scripts/bootstrap_streams.py
start logger     "$PY" -m autotrader.logger_service --config "$CONFIG"
start pager      "$PY" -m autotrader.pager --config "$CONFIG"
[ "${NO_WATCHDOG:-0}" = "1" ] || start watchdog "$PY" -m watchdog.main config/watchdog.json
start portfolio  "$BUILD_DIR/at_portfolio_svc" --config "$CONFIG"
start reconciler "$BUILD_DIR/at_reconciler_svc" --config "$CONFIG"
start ingestor   "$BUILD_DIR/at_ingestor_svc" --config "$CONFIG"
start events     "$PY" -m autotrader.events_feed.feed --config "$CONFIG"
start strategy   "$BUILD_DIR/at_strategy_svc" --config "$CONFIG"
start validator  "$PY" -m autotrader.validator.sidecar --config "$CONFIG"
start risk       "$BUILD_DIR/at_risk_svc" --config "$CONFIG"
start execution  "$BUILD_DIR/at_execution_svc" --config "$CONFIG"
echo "paper stack started (pids: ${pids[*]}). Nothing trades until broker.reconcile reports CLEAN."
wait
