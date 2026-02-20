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
  shell)
    echo "Dropping to interactive shell…"
    exec /bin/bash
    ;;
  *)
    echo "Unknown CLAW_MODE: $CLAW_MODE" >&2
    echo "Valid modes: agent | api | shell" >&2
    exit 1
    ;;
esac
