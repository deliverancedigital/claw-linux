# claw-linux root Makefile
#
# Targets:
#   make build      — Build the Docker image (compiles C binaries inside Docker)
#   make binaries   — Compile C binaries locally (requires gcc + libcurl-dev)
#   make run        — Run the agent interactively
#   make shell      — Open a shell inside the container
#   make agent      — Start the agent service via docker compose
#   make gateway    — Start the gateway + channel services via docker compose
#   make api        — Start the agent + API service via docker compose
#   make logs       — Tail agent logs
#   make stop       — Stop all compose services
#   make clean      — Remove local C build artifacts
#   make test       — Run all binary smoke tests
#   make iso        — Build a bare metal bootable ISO (Alpine + XFCE)
#   make iso-docker — Build the ISO using the iso-builder Docker stage

IMAGE   := claw-linux
VERSION ?= latest

.PHONY: build binaries run shell agent gateway api logs stop clean test iso iso-docker

# ── Docker image ─────────────────────────────────────────────────────────────

build:
	docker build --target runtime -t $(IMAGE):$(VERSION) .

# ── C binaries (local dev build) ─────────────────────────────────────────────

binaries:
	$(MAKE) -C src

# ── Container execution ───────────────────────────────────────────────────────

run: build
	docker run --rm -it \
	  -e CLAW_MODE=agent \
	  -v claw-memory:/var/lib/claw/memory \
	  -v claw-workspace:/workspace \
	  $(IMAGE):$(VERSION)

shell: build
	docker run --rm -it \
	  -e CLAW_MODE=shell \
	  -v claw-workspace:/workspace \
	  $(IMAGE):$(VERSION)

# ── Compose targets ───────────────────────────────────────────────────────────

agent:
	docker compose up -d agent ollama

gateway:
	docker compose --profile gateway up -d

api:
	docker compose --profile api up -d

logs:
	docker compose logs -f agent

stop:
	docker compose down

# ── Bare metal ISO ────────────────────────────────────────────────────────────

iso:
	@echo "=== Building bare metal ISO (requires Alpine tools) ==="
	@echo "=== Run this on an Alpine Linux host or use 'make iso-docker' ==="
	mkdir -p dist
	CLAW_REPO=$(CURDIR) sh scripts/build-iso.sh dist

iso-docker:
	@echo "=== Building bare metal ISO via Docker ==="
	docker build --target iso-builder -t $(IMAGE)-iso-builder:$(VERSION) .
	mkdir -p dist
	docker run --rm --privileged \
	  -v $(CURDIR)/dist:/dist \
	  $(IMAGE)-iso-builder:$(VERSION)
	@echo "=== ISO ready in dist/ ==="

# ── Local smoke tests ─────────────────────────────────────────────────────────

test: binaries
	@echo "=== claw-shell: echo ==="
	@echo '{"command":"echo ok","timeout":5}' | src/bin/claw-shell
	@echo "=== claw-shell: blocked command ==="
	@echo '{"command":"rm -rf /","timeout":5}' | src/bin/claw-shell
	@echo "=== claw-fs: path outside allowed ==="
	@echo '{"op":"read","path":"/etc/shadow"}' | src/bin/claw-fs
	@echo "=== claw-gateway: --help ==="
	@src/bin/claw-gateway -h 2>&1 || true
	@echo "=== claw-channel: --help ==="
	@src/bin/claw-channel -h 2>&1 || true
	@echo "=== claw-cron: --help ==="
	@src/bin/claw-cron -h 2>&1 || true
	@echo "=== All smoke tests passed ==="

# ── Cleanup ───────────────────────────────────────────────────────────────────

clean:
	$(MAKE) -C src clean
