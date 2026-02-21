#!/bin/sh
# scripts/setup-desktop.sh — Install and configure the XFCE desktop
# for claw-linux bare metal deployments.
#
# Run this once on a fresh Alpine Linux installation to set up:
#   • XFCE desktop environment
#   • LightDM display manager with auto-login as the claw user
#   • claw-linux OpenRC services (gateway, channel, cron, agent)
#   • Desktop autostart entries
#
# Usage (as root on an installed Alpine system):
#   sh /opt/claw/scripts/setup-desktop.sh

set -e

info()  { printf '\033[1;32m==> %s\033[0m\n' "$*"; }
error() { printf '\033[1;31mERROR: %s\033[0m\n' "$*" >&2; exit 1; }

[ "$(id -u)" = "0" ] || error "This script must be run as root"

CLAW_HOME="${CLAW_HOME:-/opt/claw}"
CLAW_USER="${CLAW_USER:-claw}"

# ── 1. Add community repository ───────────────────────────────────────────
info "Enabling Alpine community repository…"
REPO_FILE=/etc/apk/repositories
# Uncomment any commented-out community line first
sed -i 's|^#\(.*community\)|\1|' "$REPO_FILE" 2>/dev/null || true
# Add edge repos only if they are not already present
grep -qF 'edge/main'      "$REPO_FILE" 2>/dev/null || \
    printf 'http://dl-cdn.alpinelinux.org/alpine/edge/main\n'      >> "$REPO_FILE"
grep -qF 'edge/community' "$REPO_FILE" 2>/dev/null || \
    printf 'http://dl-cdn.alpinelinux.org/alpine/edge/community\n' >> "$REPO_FILE"
apk update

# ── 2. Install XFCE and display manager ───────────────────────────────────
info "Installing XFCE desktop environment…"
apk add --no-cache \
    xfce4 xfce4-terminal xfce4-panel xfce4-settings \
    xfwm4 xfdesktop xfce4-session xfce4-taskmanager \
    xfce4-screenshooter mousepad ristretto thunar \
    lightdm lightdm-gtk-greeter \
    dbus-x11 polkit polkit-elogind \
    xorg-server xorg-server-common xinit \
    xf86-video-vesa xf86-video-fbdev xf86-input-libinput \
    mesa-dri-gallium \
    font-dejavu adwaita-icon-theme \
    network-manager network-manager-applet \
    alsa-utils

# ── 3. Create claw user if absent ─────────────────────────────────────────
if ! id "$CLAW_USER" >/dev/null 2>&1; then
    info "Creating $CLAW_USER user…"
    addgroup -S "$CLAW_USER"
    adduser -S -G "$CLAW_USER" -h "/home/$CLAW_USER" -s /bin/bash "$CLAW_USER"
fi
adduser "$CLAW_USER" audio   2>/dev/null || true
adduser "$CLAW_USER" video   2>/dev/null || true
adduser "$CLAW_USER" input   2>/dev/null || true
adduser "$CLAW_USER" netdev  2>/dev/null || true
adduser "$CLAW_USER" wheel   2>/dev/null || true

# ── 4. Runtime directories ────────────────────────────────────────────────
info "Creating runtime directories…"
mkdir -p /var/lib/claw/memory /var/log/claw /workspace
chown -R "$CLAW_USER:$CLAW_USER" /var/lib/claw /var/log/claw /workspace

# ── 5. Configure LightDM auto-login ──────────────────────────────────────
info "Configuring LightDM auto-login for $CLAW_USER…"
cat > /etc/lightdm/lightdm.conf <<EOF
[Seat:*]
autologin-user=${CLAW_USER}
autologin-user-timeout=0
user-session=xfce
greeter-session=lightdm-gtk-greeter
EOF

# ── 6. XFCE desktop autostart — claw agent terminal ──────────────────────
info "Adding claw agent to XFCE autostart…"
AUTOSTART_DIR="/home/$CLAW_USER/.config/autostart"
mkdir -p "$AUTOSTART_DIR"
cat > "$AUTOSTART_DIR/claw-agent-terminal.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Claw Agent Terminal
Comment=Interactive claw AI agent session
Exec=xfce4-terminal --title "Claw Agent" --command "python3 /opt/claw/agent/main.py"
Icon=utilities-terminal
StartupNotify=false
EOF
chown -R "$CLAW_USER:$CLAW_USER" "/home/$CLAW_USER/.config"

# ── 7. Install OpenRC init scripts ────────────────────────────────────────
info "Installing claw OpenRC services…"
for svc in claw-gateway claw-channel claw-cron claw-agent; do
    SRC="$CLAW_HOME/scripts/init/$svc"
    if [ -f "$SRC" ]; then
        install -Dm755 "$SRC" "/etc/init.d/$svc"
        rc-update add "$svc" default 2>/dev/null || true
        info "  Enabled $svc"
    else
        printf '  WARN: %s not found, skipping\n' "$SRC"
    fi
done

# Enable display manager and networking
rc-update add lightdm   default 2>/dev/null || true
rc-update add networking default 2>/dev/null || true

# ── 8. Install Python dependencies ────────────────────────────────────────
if [ -f "$CLAW_HOME/agent/requirements.txt" ]; then
    info "Installing Python agent dependencies…"
    pip3 install --no-cache-dir --break-system-packages -r "$CLAW_HOME/agent/requirements.txt"
fi

# ── 9. Summary ────────────────────────────────────────────────────────────
info "Desktop setup complete!"
printf '\nReboot to start the XFCE desktop with the claw agent:\n'
printf '  reboot\n\n'
printf 'Or start services immediately:\n'
printf '  rc-service lightdm start\n'
printf '  rc-service claw-gateway start\n'
printf '  rc-service claw-agent start\n\n'
