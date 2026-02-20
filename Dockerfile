# claw-linux — Alpine Linux OS with OpenClaw autonomous agent
#
# Multi-stage build:
#   Stage 1 (builder): Compile the C skill binaries (claw-shell, claw-fs, claw-fetch)
#   Stage 2 (runtime): Minimal Alpine runtime with agent + compiled binaries
#
# Usage:
#   docker build -t claw-linux .
#   docker run -it claw-linux

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

# Compile all skill binaries
RUN cd src && make CC=cc CFLAGS="-O2 -Wall -Wextra -static-libgcc"

# ============================================================================
# Stage 2 — Runtime OS image
# ============================================================================
FROM alpine:3.21 AS runtime

LABEL org.opencontainers.image.title="claw-linux"
LABEL org.opencontainers.image.description="Alpine Linux OS with OpenClaw autonomous AI agent"
LABEL org.opencontainers.image.source="https://github.com/deliverancedigital/claw-linux"
LABEL org.opencontainers.image.licenses="MIT"

# Read the package list and install all uncommented, non-blank lines
COPY config/packages.txt /tmp/packages.txt
RUN grep -v '^\s*#' /tmp/packages.txt | grep -v '^\s*$' | \
    xargs apk add --no-cache && \
    rm /tmp/packages.txt

# Create non-root agent user
RUN addgroup -S claw && adduser -S -G claw -h /home/claw claw

# Install Python dependencies for the agent
COPY agent/requirements.txt /tmp/requirements.txt
RUN pip3 install --no-cache-dir -r /tmp/requirements.txt && \
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
RUN mkdir -p /workspace /var/lib/claw/memory && \
    chown -R claw:claw /workspace /var/lib/claw /opt/claw

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

EXPOSE 8080

ENTRYPOINT ["/opt/claw/scripts/entrypoint.sh"]
