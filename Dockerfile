# claw-linux — Alpine Linux OS with OpenClaw autonomous agent
#
# Multi-stage build:
#   Stage 1 (builder): Compile all C binaries
#   Stage 2 (runtime): Minimal Alpine runtime with agent + compiled binaries
#   Stage 3 (iso-builder): Bare metal ISO construction environment (XFCE)
#
# Usage:
#   docker build -t claw-linux .
#   docker run -it claw-linux
#
# Modes (set CLAW_MODE env var):
#   agent    — interactive autonomous agent (default)
#   api      — REST API server
#   gateway  — native HTTP control-plane gateway
#   channel  — webhook channel adapter
#   cron     — cron automation scheduler
#   shell    — interactive bash shell

# ============================================================================
# Stage 1 — C binary builder
# ============================================================================
FROM alpine:3.21 AS builder

# Install build-time dependencies
RUN apk add --no-cache \
        gcc \
        musl-dev \
        make \
        pkgconf \
        curl-dev \
        ca-certificates

# Copy only the C source tree
WORKDIR /build
COPY src/ src/

# Compile all binaries (gateway, channel, cron don't need libcurl except channel)
WORKDIR /build/src
RUN make CC=cc CFLAGS="-O2 -Wall -Wextra -static-libgcc"

# ============================================================================
# Stage 2 — Runtime OS image (Docker / container deployments)
# ============================================================================
FROM alpine:3.21 AS runtime

LABEL org.opencontainers.image.title="claw-linux"
LABEL org.opencontainers.image.description="Alpine Linux OS with OpenClaw autonomous AI agent — native C runtime, no Node.js"
LABEL org.opencontainers.image.source="https://github.com/deliverancedigital/claw-linux"
LABEL org.opencontainers.image.licenses="MIT"

# Read the package list and install all uncommented, non-blank lines
COPY config/packages.txt /tmp/packages.txt
SHELL ["/bin/ash", "-eo", "pipefail", "-c"]
RUN grep -v '^\s*#' /tmp/packages.txt | grep -v '^\s*$' | \
    xargs apk add --no-cache && \
    rm /tmp/packages.txt
SHELL ["/bin/sh", "-c"]

# Create non-root agent user
RUN addgroup -S claw && adduser -S -G claw -h /home/claw claw

# Install Python dependencies for the agent
COPY agent/requirements.txt /tmp/requirements.txt
RUN pip3 install --no-cache-dir --break-system-packages -r /tmp/requirements.txt && \
    rm /tmp/requirements.txt

# Copy compiled C skill binaries from builder stage
COPY --from=builder /build/src/bin/ /usr/local/bin/

# Copy the agent source and configuration
COPY agent/  /opt/claw/agent/
COPY config/ /opt/claw/config/

# Copy scripts and make them executable
COPY scripts/ /opt/claw/scripts/
RUN chmod +x /opt/claw/scripts/*.sh

# Runtime directories
RUN mkdir -p /workspace /var/lib/claw/memory /var/log/claw && \
    chown -R claw:claw /workspace /var/lib/claw /var/log/claw /opt/claw

# Switch to non-root user
USER claw
WORKDIR /workspace

# Environment defaults (override with -e or docker-compose environment:)
ENV CLAW_MODE=agent \
    OPENCLAW_MODEL_PROVIDER=ollama \
    OPENCLAW_MODEL_NAME=llama3.2 \
    OPENCLAW_AGENT_NAME=Claw \
    OPENCLAW_LOG_LEVEL=INFO \
    PYTHONPATH=/opt/claw/agent

# Gateway port (18789 matches openclaw default) and channel adapter port
EXPOSE 8080 18789 18790

ENTRYPOINT ["/opt/claw/scripts/entrypoint.sh"]

# ============================================================================
# Stage 3 — ISO build environment (bare metal / XFCE)
# Produces a bootable Alpine + XFCE ISO when targeted explicitly:
#   docker build --target iso-builder -t claw-linux-iso-builder .
#   docker run --privileged -v $(pwd)/dist:/dist claw-linux-iso-builder
# ============================================================================
FROM alpine:3.21 AS iso-builder

# Install ISO build toolchain
RUN apk add --no-cache \
        alpine-sdk \
        build-base \
        apk-tools \
        alpine-conf \
        xorriso \
        mtools \
        squashfs-tools \
        grub \
        grub-efi \
        grub-bios \
        syslinux \
        gcc \
        musl-dev \
        make \
        pkgconf \
        curl-dev \
        python3 \
        py3-pip \
        bash \
        ca-certificates

# Copy compiled binaries from builder
COPY --from=builder /build/src/bin/ /usr/local/bin/

# Copy the full repo context needed by build-iso.sh
COPY . /opt/claw/

RUN chmod +x /opt/claw/scripts/build-iso.sh /opt/claw/scripts/setup-desktop.sh

WORKDIR /opt/claw

# Running this container produces an ISO at /dist/claw-linux-<date>.iso
CMD ["/bin/sh", "-c", \
     "mkdir -p /dist && CLAW_REPO=/opt/claw /opt/claw/scripts/build-iso.sh /dist"]
