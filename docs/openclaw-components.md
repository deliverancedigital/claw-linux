# OpenClaw Component Map

This document maps the modules in the upstream
[openclaw/openclaw](https://github.com/openclaw/openclaw/tree/main/src) TypeScript source tree
to their native C binary equivalents in **claw-linux**.

## Status legend

| Symbol | Meaning |
|--------|---------|
| ✅ | Fully implemented as a native C binary |
| 🔄 | Partially implemented (functionality exists inside another binary or the Python agent) |
| ❌ | Not yet implemented |

---

## Core runtime modules

| openclaw/openclaw `src/` module | claw-linux binary | Status | Notes |
|---------------------------------|-------------------|--------|-------|
| `gateway/` | `claw-gateway` | ✅ | HTTP control-plane (port 18789); POST /api/message, /api/event, GET /api/health, /api/status |
| `channels/` | `claw-channel` | ✅ | Webhook normaliser for Telegram, Discord, Slack, generic (port 18790) |
| `cron/` | `claw-cron` | ✅ | Native cron scheduler; reads crontab files; supports @reboot, @daily, @hourly, @weekly, @monthly |
| `daemon/` | `claw-daemon` | ✅ | Service lifecycle manager (start/stop/restart/status) using PID files under `/var/run/claw/` |
| `tui/` | `claw-tui` | ✅ | Interactive terminal chat UI; connects to the local gateway HTTP API |
| `cli/` | `claw-daemon` + `claw-tui` | 🔄 | The most critical CLI sub-commands (`daemon-cli`, `gateway-cli`, `tui-cli`) are covered; specialised sub-commands (`browser-cli`, `devices-cli`, `nodes-cli`, `pairing-cli`, `sandbox-cli`, etc.) are not yet implemented |
| `agents/` | `agent/main.py` | 🔄 | ReAct agent loop implemented in Python; C-level agent runner not yet ported |
| `sessions/` | `claw-gateway` | 🔄 | Conversation sessions tracked in-process by the gateway; persistent session registry not yet implemented |
| `memory/` | `agent/memory/store.py` | 🔄 | Persistent memory implemented in Python; a standalone C binary (`claw-mem`) is not yet implemented |
| `providers/` | `agent/core/agent.py` | 🔄 | Ollama, OpenAI, Anthropic backends implemented in Python |
| `config/` | `config/agent.yaml` | 🔄 | YAML config consumed by the Python agent; a C config reader library is not yet implemented |
| `routing/` | `claw-gateway` | 🔄 | HTTP request routing implemented inside claw-gateway |
| `security/` | `claw-shell`, `claw-fs` | 🔄 | Blocked-command policy (claw-shell) and path allowlists (claw-fs) cover core security rules |
| `acp/` | `claw-shell`, `claw-fs` | 🔄 | Access-control primitives are embedded in skill binaries; a separate ACP daemon is not yet implemented |
| `signal/` | All C binaries | 🔄 | SIGTERM/SIGINT/SIGCHLD/SIGHUP handling built into every daemon binary |
| `hooks/` | — | ❌ | Event hook system not yet implemented |
| `plugins/` | — | ❌ | Plugin registry and SDK not yet implemented |
| `terminal/` | — | ❌ | PTY/terminal emulation layer not yet implemented |
| `browser/` | — | ❌ | Browser automation not yet implemented |
| `canvas-host/` | — | ❌ | Canvas host not applicable to headless/server deployments |
| `node-host/` | — | ❌ | Node.js sub-process host not applicable (claw-linux has no Node.js runtime) |
| `media/` | — | ❌ | Media handling not yet implemented |
| `media-understanding/` | — | ❌ | Vision / media understanding not yet implemented |
| `link-understanding/` | — | ❌ | Link preview / understanding not yet implemented |
| `tts/` | — | ❌ | Text-to-speech not yet implemented |
| `pairing/` | — | ❌ | Device pairing not yet implemented |
| `macos/` | — | ❌ | macOS-specific modules — not applicable to Alpine Linux |
| `discord/` | `claw-channel` | 🔄 | Discord inbound webhook normalised; outbound channel dispatch via Python agent |
| `slack/` | `claw-channel` | 🔄 | Slack inbound webhook normalised; outbound channel dispatch via Python agent |
| `telegram/` | `claw-channel` | 🔄 | Telegram inbound webhook normalised; outbound channel dispatch via Python agent |
| `imessage/` | — | ❌ | Apple iMessage not applicable to Alpine Linux |
| `line/` | — | ❌ | LINE messenger not yet implemented |
| `whatsapp/` | — | ❌ | WhatsApp not yet implemented |
| `infra/` | Docker / OpenRC | 🔄 | Container and bare-metal infrastructure handled via Dockerfile + OpenRC init scripts |
| `logging/` | All C binaries | 🔄 | Stderr/file logging built into each binary; a centralised log aggregator (`claw-log`) is not yet implemented |
| `process/` | `claw-daemon` | 🔄 | Process supervision implemented in claw-daemon |
| `auto-reply/` | `agent/core/agent.py` | 🔄 | Auto-reply logic part of the Python ReAct loop |
| `commands/` | `claw-shell` | 🔄 | Shell command execution skill |
| `compat/` | — | ❌ | Cross-platform compatibility shims not applicable |
| `shared/` | `common/claw_json.h` | 🔄 | Shared JSON utilities in the common header |
| `types/` | `common/claw_json.h` | 🔄 | Type definitions embedded in the common header |
| `utils/` | Embedded per-binary | 🔄 | Utility functions are inlined in each binary |
| `markdown/` | — | ❌ | Markdown renderer not yet implemented |
| `web/` | `claw-fetch` | 🔄 | HTTP fetch implemented; browser-level web features not yet implemented |
| `wizard/` | — | ❌ | Interactive setup wizard not yet implemented |
| `docs/` | `docs/` | 🔄 | Documentation is being maintained in this repo |
| `scripts/` | `scripts/` | 🔄 | Build and setup scripts maintained in this repo |

---

## Skill binaries (claw-linux additions, no direct openclaw/src counterpart)

These binaries were added to claw-linux as native implementations of OpenClaw's skill system,
which is invoked by the ReAct agent loop:

| Binary | Description |
|--------|-------------|
| `claw-shell` | Sandboxed shell command execution skill (blocked-command policy + timeout) |
| `claw-fs` | Allowlisted filesystem read/write/list skill |
| `claw-fetch` | HTTPS fetch skill (libcurl, TLS-verified, 32 MiB cap) |

---

## Implementation roadmap

The following chunks are suggested in rough priority order:

### Chunk 1 — Already done (this PR)
- `claw-daemon` — Service lifecycle manager
- `claw-tui` — Interactive terminal chat UI

### Chunk 2 — Logging & observability
- `claw-log` — Centralised log viewer / streamer (tails `/var/log/claw/*.log`)

### Chunk 3 — Session & memory persistence
- Extend `claw-gateway` to persist session state to disk
- `claw-mem` — Standalone memory read/write/search binary

### Chunk 4 — Extended channel support
- LINE messenger adapter in `claw-channel`
- WhatsApp adapter in `claw-channel`

### Chunk 5 — Plugin & hook system
- `claw-plugin` — Plugin loader / sandbox binary
- Hook registration in `claw-gateway`

### Chunk 6 — Terminal emulation
- `claw-term` — PTY-backed terminal emulator for the XFCE desktop environment
