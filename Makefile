# claw-linux root Makefile
#
# Targets:
#   make build      — Build the Docker image (compiles C binaries inside Docker)
#   make binaries   — Compile C skill binaries locally (requires gcc + libcurl-dev)
#   make run        — Run the agent interactively
#   make shell      — Open a shell inside the container
#   make agent      — Start the agent service via docker compose
#   make api        — Start the agent + API service via docker compose
#   make logs       — Tail agent logs
#   make stop       — Stop all compose services
#   make clean      — Remove local C build artifacts
#   make test       — Run skill binary smoke tests

IMAGE   := claw-linux
VERSION ?= latest

.PHONY: build binaries run shell agent api logs stop clean test

# ── Docker image ─────────────────────────────────────────────────────────────

build:
	docker build -t $(IMAGE):$(VERSION) .

# ── C skill binaries (local dev build) ───────────────────────────────────────

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

api:
	docker compose --profile api up -d

logs:
	docker compose logs -f agent

stop:
	docker compose down

# ── Local smoke tests ─────────────────────────────────────────────────────────

test: binaries
	@echo "=== claw-shell: echo ==="
	@echo '{"command":"echo ok","timeout":5}' | src/bin/claw-shell
	@echo "=== claw-shell: blocked ==="
	@echo '{"command":"rm -rf /","timeout":5}' | src/bin/claw-shell
	@echo "=== claw-fs: path outside allowed ==="
	@echo '{"op":"read","path":"/etc/shadow"}' | src/bin/claw-fs
	@echo "=== All smoke tests passed ==="

# ── Cleanup ───────────────────────────────────────────────────────────────────

clean:
	$(MAKE) -C src clean
