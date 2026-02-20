#!/bin/sh
# claw-linux container entrypoint
# Runs as the last instruction in the Dockerfile CMD.
# Starts the autonomous agent or drops to a shell depending on CLAW_MODE.
set -e

CLAW_MODE="${CLAW_MODE:-agent}"
WORKSPACE="${WORKSPACE:-/workspace}"
MEMORY_DIR="${OPENCLAW_MEMORY_DIR:-/var/lib/claw/memory}"

# Ensure required directories exist with correct permissions
mkdir -p "$WORKSPACE" "$MEMORY_DIR"

echo "┌────────────────────────────────────────────────┐"
echo "│            claw-linux  (Alpine Linux)          │"
echo "│    OpenClaw-compatible autonomous AI agent     │"
echo "└────────────────────────────────────────────────┘"
echo ""
echo "Mode:    $CLAW_MODE"
echo "Workspace: $WORKSPACE"

case "$CLAW_MODE" in
  agent)
    echo "Starting autonomous agent…"
    exec python3 /opt/claw/agent/main.py
    ;;
  api)
    echo "Starting agent REST API…"
    exec python3 /opt/claw/agent/main.py --api
    ;;
  gateway)
    echo "Starting native gateway…"
    exec /usr/local/bin/claw-gateway \
        -p "${CLAW_GATEWAY_PORT:-18789}" \
        -b "${CLAW_GATEWAY_BIND:-0.0.0.0}"
    ;;
  channel)
    echo "Starting channel adapter…"
    exec /usr/local/bin/claw-channel \
        -p "${CLAW_CHANNEL_PORT:-18790}" \
        -b "${CLAW_CHANNEL_BIND:-0.0.0.0}" \
        -g "${CLAW_GATEWAY_URL:-http://127.0.0.1:18789/api/event}"
    ;;
  cron)
    echo "Starting cron scheduler…"
    exec /usr/local/bin/claw-cron \
        -f "${CLAW_CRONTAB:-/opt/claw/config/crontab}" \
        -l "${CLAW_CRON_LOG:--}"
    ;;
  desktop)
    echo "Desktop mode requires bare metal or a display server."
    echo "See scripts/setup-desktop.sh for bare metal XFCE setup."
    echo "Dropping to interactive shell…"
    exec /bin/bash
    ;;
  shell)
    echo "Dropping to interactive shell…"
    exec /bin/bash
    ;;
  *)
    echo "Unknown CLAW_MODE: $CLAW_MODE" >&2
    echo "Valid modes: agent | api | gateway | channel | cron | desktop | shell" >&2
    exit 1
    ;;
esac
