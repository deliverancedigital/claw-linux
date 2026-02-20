#!/bin/sh
# claw-linux agent setup script
# Installs Python dependencies for the autonomous agent.
# Called once at image build time (not at runtime).
set -e

echo "==> Installing agent Python dependencies…"
pip3 install --no-cache-dir -r /opt/claw/agent/requirements.txt

echo "==> Creating runtime directories…"
mkdir -p /var/lib/claw/memory
mkdir -p /workspace

echo "==> Agent setup complete."
