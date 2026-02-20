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
| `gateway/` | `claw-gateway` | ✅ | HTTP control-plane (port 18789); POST /api/message, /api/event, GET /api/health, /api/status, hook registration, session endpoints |
| `channels/` | `claw-channel` | ✅ | Webhook normaliser for Telegram, Discord, Slack, LINE, WhatsApp, generic (port 18790) |
| `cron/` | `claw-cron` | ✅ | Native cron scheduler; reads crontab files; supports @reboot, @daily, @hourly, @weekly, @monthly |
| `daemon/` | `claw-daemon` | ✅ | Service lifecycle manager (start/stop/restart/status) using PID files under `/var/run/claw/` |
| `tui/` | `claw-tui` | ✅ | Interactive terminal chat UI; connects to the local gateway HTTP API |
| `logging/` | `claw-log` | ✅ | Centralised log viewer/streamer; tails `/var/log/claw/*.log` with per-service colour coding and follow mode |
| `memory/` | `claw-mem` | ✅ | Standalone memory read/write/search binary; JSON skill protocol; persistent key-value store at `/var/lib/claw/memory.json` |
| `plugins/` | `claw-plugin` | ✅ | Plugin loader/registry; discovers executables under `/opt/claw/plugins/`; JSON metadata sidecars; list/info/run/install/remove sub-commands |
| `terminal/` | `claw-term` | ✅ | PTY-backed terminal emulator; interactive bridge and capture mode; XFCE-compatible; uses POSIX pseudo-terminal APIs |
| `markdown/` | `claw-md` | ✅ | Markdown → ANSI terminal renderer; headings, bold, italic, code, blockquotes, lists, links, fenced code blocks |
| `link-understanding/` | `claw-link` | ✅ | Link preview / metadata extraction; fetches URL and extracts title, description, og:image, canonical via libcurl |
| `tts/` | `claw-tts` | ✅ | Text-to-speech via espeak-ng (`apk add espeak-ng`); voice/speed/pitch control; WAV file output; CLI and JSON skill modes |
| `browser/` | `claw-browser` | ✅ | Browser integration; uses xdg-open on XFCE desktop (DISPLAY/WAYLAND_DISPLAY); falls back to w3m/lynx/elinks headless; JSON skill mode with dump option |
| `media/` | `claw-media` | ✅ | Media playback via mpv/mplayer; metadata via ffprobe; play/info/stop sub-commands; GUI on XFCE desktop, audio-only on headless |
| `canvas-host/` | `claw-canvas` | ✅ | Canvas host for XFCE desktop; writes HTML/SVG/text to a temp file and opens it via xdg-open; headless mode prints path to stdout |
| `media-understanding/` | `claw-mu` | ✅ | Image/media understanding via Ollama vision (llava) or OpenAI GPT-4o; base64 image encoding; JSON skill protocol |
| `pairing/` | `claw-pair` | ✅ | Device pairing with challenge-response protocol; 6-char pairing code; per-device token registry at `/var/lib/claw/paired.json` |
| `hooks/` | `claw-gateway` | ✅ | Event hook registration and delivery; POST /api/hook/register, GET /api/hooks, DELETE /api/hook/<id>; fire-and-forget HTTP POST delivery |
| `sessions/` | `claw-gateway` | ✅ | Per-session persistence to disk (`/var/lib/claw/sessions/<id>.json`); GET /api/sessions, GET /api/session/<id> |
| `wizard/` | `scripts/claw-wizard.sh` | ✅ | Interactive setup wizard; configures LLM provider, API keys, channels, desktop; writes `/opt/claw/config/claw.env` |
| `cli/` | `claw-daemon` + `claw-tui` | 🔄 | The most critical CLI sub-commands (`daemon-cli`, `gateway-cli`, `tui-cli`) are covered; specialised sub-commands (`devices-cli`, `nodes-cli`, `sandbox-cli`, etc.) are not yet implemented |
| `agents/` | `agent/main.py` | 🔄 | ReAct agent loop implemented in Python; C-level agent runner not yet ported |
| `providers/` | `agent/core/agent.py` | 🔄 | Ollama, OpenAI, Anthropic backends implemented in Python |
| `config/` | `config/agent.yaml` | 🔄 | YAML config consumed by the Python agent; a C config reader library is not yet implemented |
| `routing/` | `claw-gateway` | 🔄 | HTTP request routing implemented inside claw-gateway |
| `security/` | `claw-shell`, `claw-fs` | 🔄 | Blocked-command policy (claw-shell) and path allowlists (claw-fs) cover core security rules |
| `acp/` | `claw-shell`, `claw-fs` | 🔄 | Access-control primitives are embedded in skill binaries; a separate ACP daemon is not yet implemented |
| `signal/` | All C binaries | 🔄 | SIGTERM/SIGINT/SIGCHLD/SIGHUP handling built into every daemon binary |
| ~~`node-host/`~~ | — | ~~❌~~ | ~~Node.js sub-process host not applicable — claw-linux has no Node.js runtime~~ |
| ~~`macos/`~~ | — | ~~❌~~ | ~~macOS-specific modules — not applicable to Alpine Linux~~ |
| ~~`imessage/`~~ | — | ~~❌~~ | ~~Apple iMessage — not applicable to Alpine Linux~~ |
| `discord/` | `claw-channel` | 🔄 | Discord inbound webhook normalised; outbound channel dispatch via Python agent |
| `slack/` | `claw-channel` | 🔄 | Slack inbound webhook normalised; outbound channel dispatch via Python agent |
| `telegram/` | `claw-channel` | 🔄 | Telegram inbound webhook normalised; outbound channel dispatch via Python agent |
| `line/` | `claw-channel` | ✅ | LINE Messaging API webhook normalised and forwarded to gateway |
| `whatsapp/` | `claw-channel` | ✅ | WhatsApp Business API webhook normalised and forwarded to gateway |
| `infra/` | Docker / OpenRC | 🔄 | Container and bare-metal infrastructure handled via Dockerfile + OpenRC init scripts |
| `process/` | `claw-daemon` | 🔄 | Process supervision implemented in claw-daemon |
| `auto-reply/` | `agent/core/agent.py` | 🔄 | Auto-reply logic part of the Python ReAct loop |
| `commands/` | `claw-shell` | 🔄 | Shell command execution skill |
| `compat/` | All C binaries | 🔄 | Alpine/musl Linux compatibility built directly into each binary; no separate shim needed |
| `shared/` | `common/claw_json.h` | 🔄 | Shared JSON utilities in the common header |
| `types/` | `common/claw_json.h` | 🔄 | Type definitions embedded in the common header |
| `utils/` | Embedded per-binary | 🔄 | Utility functions are inlined in each binary |
| `web/` | `claw-fetch` | 🔄 | HTTP fetch implemented; browser-level web features via `claw-browser` |
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
| `claw-log` | Centralised log viewer/streamer with follow mode |
| `claw-mem` | Persistent key-value memory store (JSON file-backed) |
| `claw-plugin` | Plugin loader/registry for `/opt/claw/plugins/` |
| `claw-term` | PTY-backed terminal emulator / subprocess runner |
| `claw-md` | Markdown → ANSI terminal renderer |
| `claw-link` | Link preview / metadata extractor (libcurl) |
| `claw-tts` | Text-to-speech via espeak-ng |
| `claw-browser` | Browser launcher (xdg-open on XFCE, w3m/lynx headless) |
| `claw-media` | Media playback (mpv) and metadata (ffprobe) |
| `claw-canvas` | Canvas host for XFCE desktop (HTML/SVG/text viewer) |
| `claw-mu` | Media/image understanding via Ollama vision or OpenAI |
| `claw-pair` | Device pairing with challenge-response token issuance |

---

## Implementation roadmap

### Completed ✅

- `claw-daemon` — Service lifecycle manager
- `claw-tui` — Interactive terminal chat UI
- `claw-log` — Centralised log viewer/streamer
- `claw-mem` — Standalone memory read/write/search binary
- `claw-plugin` — Plugin loader/registry
- `claw-term` — PTY-backed terminal emulator
- `claw-md` — Markdown → ANSI renderer
- `claw-link` — Link preview/understanding
- `claw-tts` — Text-to-speech via espeak-ng
- `claw-browser` — Browser integration (xdg-open / headless)
- `claw-media` — Media playback and metadata
- `claw-canvas` — Canvas host for XFCE desktop
- `claw-mu` — Media/image understanding
- `claw-pair` — Device pairing
- LINE + WhatsApp in `claw-channel`
- Hook registration in `claw-gateway`
- Session persistence in `claw-gateway`
- `claw-wizard.sh` — Interactive setup wizard

### Remaining 🔄

- Outbound channel dispatch (Telegram/Discord/Slack/LINE/WhatsApp send) — currently Python-only
- Separate ACP daemon
- Specialised CLI sub-commands (devices-cli, nodes-cli, sandbox-cli)
