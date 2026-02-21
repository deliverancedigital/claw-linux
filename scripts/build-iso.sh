#!/bin/sh
# scripts/build-iso.sh — Build a bootable claw-linux ISO for bare metal hardware.
#
# Based on Alpine Linux mkimage (aports/scripts/mkimage.sh).
# Produces a hybrid ISO that can be written to USB or burned to disc and
# booted on any x86_64 machine.  After booting, the XFCE desktop launches
# automatically and the claw-linux agent starts as a background service.
#
# Prerequisites (run on an Alpine Linux host or inside the build container):
#   apk add alpine-sdk build-base apk-tools alpine-conf \
#           syslinux isolinux xorriso mtools squashfs-tools \
#           grub grub-efi
#
# Usage:
#   ./scripts/build-iso.sh [OUTPUT_DIR]
#
#   OUTPUT_DIR defaults to ./dist/
#   The finished ISO is written to OUTPUT_DIR/claw-linux-<date>.iso
#
# Environment variables:
#   ALPINE_RELEASE   Alpine release branch to base on  (default: v3.22)
#   ALPINE_ARCH      Target architecture               (default: x86_64)
#   ALPINE_MIRROR    APK mirror URL                    (default: https://dl-cdn.alpinelinux.org/alpine)
#   ISO_LABEL        ISO volume label                  (default: CLAW-LINUX)
#   CLAW_REPO        Repository root directory         (default: script directory/..)

set -e

# ── Defaults ────────────────────────────────────────────────────────────────
ALPINE_RELEASE="${ALPINE_RELEASE:-v3.22}"
ALPINE_ARCH="${ALPINE_ARCH:-x86_64}"
ALPINE_MIRROR="${ALPINE_MIRROR:-https://dl-cdn.alpinelinux.org/alpine}"
ISO_LABEL="${ISO_LABEL:-CLAW-LINUX}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CLAW_REPO="${CLAW_REPO:-$(cd "$SCRIPT_DIR/.." && pwd)}"
OUTPUT_DIR="${1:-$CLAW_REPO/dist}"
WORK_DIR="${TMPDIR:-/tmp}/claw-iso-work"
DATE="$(date +%Y%m%d)"
ISO_NAME="claw-linux-${DATE}.iso"

# ── Helpers ─────────────────────────────────────────────────────────────────
info()  { printf '\033[1;32m==> %s\033[0m\n' "$*"; }
error() { printf '\033[1;31mERROR: %s\033[0m\n' "$*" >&2; exit 1; }

require() {
    for cmd in "$@"; do
        command -v "$cmd" >/dev/null 2>&1 || \
            error "Required tool not found: $cmd  (run: apk add $cmd)"
    done
}

# ── Preflight checks ─────────────────────────────────────────────────────────
require apk mkfs.vfat xorriso mksquashfs

info "claw-linux ISO builder"
info "Alpine ${ALPINE_RELEASE} / ${ALPINE_ARCH}"
info "Output: ${OUTPUT_DIR}/${ISO_NAME}"

mkdir -p "$OUTPUT_DIR" "$WORK_DIR"

ROOTFS="$WORK_DIR/rootfs"
OVERLAY="$WORK_DIR/overlay"
ISO_STAGING="$WORK_DIR/iso"

rm -rf "$ROOTFS" "$OVERLAY" "$ISO_STAGING"
mkdir -p "$ROOTFS" "$OVERLAY" "$ISO_STAGING/boot/grub" "$ISO_STAGING/boot/syslinux"

# ── Stage 1: Bootstrap Alpine minimal rootfs ──────────────────────────────
info "Bootstrapping Alpine ${ALPINE_RELEASE} rootfs…"

apk --arch "$ALPINE_ARCH" \
    --root "$ROOTFS" \
    --initdb \
    --keys-dir /etc/apk/keys \
    --repository "${ALPINE_MIRROR}/${ALPINE_RELEASE}/main" \
    --repository "${ALPINE_MIRROR}/${ALPINE_RELEASE}/community" \
    add alpine-base

# Write APK repositories for in-ISO use
mkdir -p "$ROOTFS/etc/apk"
printf '%s/%s/main\n%s/%s/community\n' \
    "$ALPINE_MIRROR" "$ALPINE_RELEASE" \
    "$ALPINE_MIRROR" "$ALPINE_RELEASE" \
    > "$ROOTFS/etc/apk/repositories"

# ── Stage 2: Install core + XFCE desktop + claw packages ──────────────────
info "Installing packages (core + XFCE desktop)…"

CORE_PKGS="
    bash busybox-extras coreutils util-linux procps shadow sudo
    curl wget ca-certificates openssh-client
    git
    linux-lts
    python3 py3-pip
    jq htop
"

XFCE_PKGS="
    xfce4 xfce4-terminal xfce4-panel xfce4-settings
    xfwm4 xfdesktop xfce4-session xfce4-taskmanager
    xfce4-screenshooter mousepad ristretto thunar
    lightdm lightdm-gtk-greeter
    dbus-x11 polkit polkit-elogind
    xorg-server xorg-server-common xinit
    xf86-video-vesa xf86-video-fbdev xf86-input-libinput
    mesa-dri-gallium
    font-dejavu adwaita-icon-theme
    networkmanager network-manager-applet
    gvfs gvfs-mtp
    alsa-utils pipewire wireplumber
"

apk --root "$ROOTFS" \
    --keys-dir /etc/apk/keys \
    --repository "${ALPINE_MIRROR}/${ALPINE_RELEASE}/main" \
    --repository "${ALPINE_MIRROR}/${ALPINE_RELEASE}/community" \
    add $CORE_PKGS $XFCE_PKGS

# ── Stage 3: Install compiled claw binaries ────────────────────────────────
info "Installing claw-linux binaries…"

# Build the C binaries if not already built
if [ ! -f "$CLAW_REPO/src/bin/claw-shell" ]; then
    info "Compiling C binaries…"
    make -C "$CLAW_REPO/src" CC=cc CFLAGS="-O2 -Wall -Wextra"
fi

install -Dm755 "$CLAW_REPO/src/bin/claw-shell"   "$ROOTFS/usr/local/bin/claw-shell"
install -Dm755 "$CLAW_REPO/src/bin/claw-fs"       "$ROOTFS/usr/local/bin/claw-fs"
install -Dm755 "$CLAW_REPO/src/bin/claw-fetch"    "$ROOTFS/usr/local/bin/claw-fetch"
install -Dm755 "$CLAW_REPO/src/bin/claw-gateway"  "$ROOTFS/usr/local/bin/claw-gateway"
install -Dm755 "$CLAW_REPO/src/bin/claw-channel"  "$ROOTFS/usr/local/bin/claw-channel"
install -Dm755 "$CLAW_REPO/src/bin/claw-cron"     "$ROOTFS/usr/local/bin/claw-cron"

# ── Stage 4: Install Python agent ─────────────────────────────────────────
info "Installing claw Python agent…"

mkdir -p "$ROOTFS/opt/claw"
cp -r "$CLAW_REPO/agent"  "$ROOTFS/opt/claw/agent"
cp -r "$CLAW_REPO/config" "$ROOTFS/opt/claw/config"
cp -r "$CLAW_REPO/scripts" "$ROOTFS/opt/claw/scripts"

# Install Python dependencies
apk --root "$ROOTFS" \
    --keys-dir /etc/apk/keys \
    --repository "${ALPINE_MIRROR}/${ALPINE_RELEASE}/main" \
    --repository "${ALPINE_MIRROR}/${ALPINE_RELEASE}/community" \
    add py3-yaml py3-requests

# ── Stage 5: Create claw user and directories ──────────────────────────────
info "Creating claw user account…"

chroot "$ROOTFS" sh -c "
    addgroup -S claw
    adduser -S -G claw -h /home/claw -s /bin/bash claw
    adduser claw audio
    adduser claw video
    adduser claw input
    adduser claw netdev
    mkdir -p /var/lib/claw/memory /var/log/claw /workspace /home/claw/.config
    chown -R claw:claw /var/lib/claw /var/log/claw /opt/claw /workspace /home/claw
"

# ── Stage 6: Install OpenRC init scripts ──────────────────────────────────
info "Installing OpenRC init scripts…"

for svc in claw-gateway claw-channel claw-cron claw-agent; do
    install -Dm755 "$CLAW_REPO/scripts/init/$svc" \
        "$ROOTFS/etc/init.d/$svc"
done

# Enable services at boot
for svc in lightdm claw-gateway claw-cron claw-agent networking; do
    chroot "$ROOTFS" rc-update add "$svc" default 2>/dev/null || true
done
chroot "$ROOTFS" rc-update add claw-channel default 2>/dev/null || true

# ── Stage 7: Configure XFCE auto-login and desktop ────────────────────────
info "Configuring XFCE desktop for claw user…"

# LightDM auto-login as claw
mkdir -p "$ROOTFS/etc/lightdm"
cat > "$ROOTFS/etc/lightdm/lightdm.conf" <<'EOF'
[Seat:*]
autologin-user=claw
autologin-user-timeout=0
user-session=xfce
greeter-session=lightdm-gtk-greeter
EOF

# XFCE session autostart — launch the agent terminal on desktop start
mkdir -p "$ROOTFS/home/claw/.config/autostart"
cat > "$ROOTFS/home/claw/.config/autostart/claw-agent-terminal.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Claw Agent Terminal
Comment=Open an interactive terminal to the claw AI agent
Exec=xfce4-terminal --title "Claw Agent" --command "python3 /opt/claw/agent/main.py"
Icon=utilities-terminal
StartupNotify=false
EOF

# Desktop background and panel defaults
mkdir -p "$ROOTFS/home/claw/.config/xfce4/xfconf/xfce-perchannel-xml"
cat > "$ROOTFS/home/claw/.config/xfce4/xfconf/xfce-perchannel-xml/xfce4-desktop.xml" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<channel name="xfce4-desktop" version="1.0">
  <property name="backdrop" type="empty">
    <property name="screen0" type="empty">
      <property name="monitor0" type="empty">
        <property name="workspace0" type="empty">
          <property name="color-style"   type="int"    value="0"/>
          <property name="rgba1"         type="array">
            <value type="double" value="0.047059"/>
            <value type="double" value="0.188235"/>
            <value type="double" value="0.278431"/>
            <value type="double" value="1.000000"/>
          </property>
          <property name="image-style"   type="int"    value="0"/>
        </property>
      </property>
    </property>
  </property>
</channel>
EOF

chroot "$ROOTFS" chown -R claw:claw /home/claw

# ── Stage 8: Networking defaults (DHCP on eth0) ────────────────────────────
info "Configuring default networking…"

mkdir -p "$ROOTFS/etc/network"
cat > "$ROOTFS/etc/network/interfaces" <<'EOF'
auto lo
iface lo inet loopback

auto eth0
iface eth0 inet dhcp
EOF

# hostname
printf 'claw-linux\n' > "$ROOTFS/etc/hostname"

# ── Stage 9: Build squashfs rootfs image ──────────────────────────────────
info "Creating squashfs filesystem image…"

mksquashfs "$ROOTFS" "$ISO_STAGING/rootfs.squashfs" \
    -comp xz -Xdict-size 100% -no-progress -quiet

# ── Stage 10: Copy kernel and initramfs from rootfs ───────────────────────
info "Copying kernel and initramfs…"

VMLINUZ=$(find "$ROOTFS/boot" -name 'vmlinuz*' | sort -V | tail -n1)
INITRAMFS=$(find "$ROOTFS/boot" \( -name 'initramfs*' -o -name 'initrd*' \) \
            | sort -V | tail -n1)

[ -f "$VMLINUZ" ]   || error "Kernel not found in $ROOTFS/boot"

cp "$VMLINUZ"  "$ISO_STAGING/boot/vmlinuz"
[ -f "$INITRAMFS" ] && cp "$INITRAMFS" "$ISO_STAGING/boot/initramfs"

# ── Stage 11: GRUB EFI + BIOS bootloader ──────────────────────────────────
info "Installing GRUB bootloader…"

cat > "$ISO_STAGING/boot/grub/grub.cfg" <<EOF
set default=0
set timeout=5
set gfxpayload=keep

insmod all_video

menuentry "claw-linux (XFCE Desktop)" {
    linux  /boot/vmlinuz quiet modules=loop,squashfs,sd-mod,usb-storage \
           root=/dev/ram0 initrd=/boot/initramfs \
           claw.mode=desktop
    initrd /boot/initramfs
}

menuentry "claw-linux (Agent-only, headless)" {
    linux  /boot/vmlinuz quiet modules=loop,squashfs,sd-mod,usb-storage \
           root=/dev/ram0 initrd=/boot/initramfs \
           claw.mode=agent console=tty0 console=ttyS0,115200n8
    initrd /boot/initramfs
}

menuentry "claw-linux (Shell only)" {
    linux  /boot/vmlinuz quiet modules=loop,squashfs,sd-mod,usb-storage \
           root=/dev/ram0 initrd=/boot/initramfs \
           claw.mode=shell console=tty0
    initrd /boot/initramfs
}
EOF

# ── Stage 12: Assemble ISO ─────────────────────────────────────────────────
info "Assembling ISO image…"

xorriso -as mkisofs \
    -o "${OUTPUT_DIR}/${ISO_NAME}" \
    -V "$ISO_LABEL" \
    -J -R \
    -b boot/syslinux/isolinux.bin \
    -c boot/syslinux/boot.cat \
    -no-emul-boot \
    -boot-load-size 4 \
    -boot-info-table \
    --grub2-mbr /usr/lib/grub/i386-pc/boot_hybrid.img \
    --efi-boot boot/grub/efi.img \
    -efi-boot-part \
    --efi-boot-image \
    "$ISO_STAGING" 2>/dev/null || \
xorriso -as mkisofs \
    -o "${OUTPUT_DIR}/${ISO_NAME}" \
    -V "$ISO_LABEL" \
    -J -R \
    "$ISO_STAGING"

info "ISO build complete!"
info "Output: ${OUTPUT_DIR}/${ISO_NAME}"
printf '\nTo write to USB (replace /dev/sdX with your device):\n'
printf '  dd if=%s/%s of=/dev/sdX bs=4M status=progress\n\n' \
    "$OUTPUT_DIR" "$ISO_NAME"
