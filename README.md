# claw-linux

**claw-linux** is an Alpine Linux-based operating system built to run a fully native, zero-Node.js implementation of the [OpenClaw](https://github.com/openclaw/openclaw) autonomous AI agent stack. All core OpenClaw components — gateway, channels, skills, automation, and runtime — are re-implemented as native C binaries with direct kernel access. No Node.js runtime is required.

## What's inside

| Component | Binary | Description |
|---|---|---|
| **Gateway** | `claw-gateway` | Native HTTP control plane (port 18789); message, event, hook, and session endpoints |
| **Channel adapter** | `claw-channel` | Webhook receiver for Telegram, Discord, Slack, LINE, WhatsApp, and generic webhooks (port 18790) |
| **Shell skill** | `claw-shell` | Sandboxed shell command execution with blocked-command policy |
| **Filesystem skill** | `claw-fs` | Allowlisted file read/write/list with path safety |
| **Web fetch skill** | `claw-fetch` | libcurl-backed HTTPS fetch with TLS verification |
| **Cron scheduler** | `claw-cron` | Native cron daemon for automation tasks |
| **Service manager** | `claw-daemon` | Start/stop/restart/status for all claw services using PID files |
| **Terminal UI** | `claw-tui` | Interactive readline-style chat interface connecting to the local gateway |
| **Log viewer** | `claw-log` | Centralised log viewer/streamer with per-service colour coding and follow mode |
| **Memory store** | `claw-mem` | Persistent key-value memory store (JSON skill protocol) |
| **Plugin loader** | `claw-plugin` | Plugin registry; discover, install, and run plugins from `/opt/claw/plugins/` |
| **Terminal emulator** | `claw-term` | PTY-backed terminal emulator with interactive bridge and capture modes |
| **Markdown renderer** | `claw-md` | Markdown → ANSI terminal renderer |
| **Link preview** | `claw-link` | Link metadata extractor (title, description, og:image) via libcurl |
| **Text-to-speech** | `claw-tts` | Speech synthesis via espeak-ng; JSON skill + CLI |
| **Browser** | `claw-browser` | Browser launcher (xdg-open on XFCE; w3m/lynx headless fallback) |
| **Media** | `claw-media` | Media playback via mpv; metadata via ffprobe; XFCE GUI or headless audio |
| **Canvas host** | `claw-canvas` | Visual canvas for XFCE desktop; renders HTML/SVG/text in the browser |
| **Media understanding** | `claw-mu` | Image/media analysis via Ollama vision (llava) or OpenAI GPT-4o |
| **Device pairing** | `claw-pair` | Challenge-response device pairing with per-device token registry |
| **Setup wizard** | `claw-wizard.sh` | Interactive setup wizard for LLM provider, API keys, and channels |
| **Python agent** | `agent/main.py` | ReAct-loop agent with Ollama/OpenAI/Anthropic backends |

## Features

- **Minimal footprint** — Built on Alpine Linux for a lean, fast, secure base.
- **No Node.js required** — All OpenClaw components re-implemented in native C.
- **Native kernel access** — C binaries run with full OS-level capabilities.
- **Channels** — Telegram, Discord, Slack, LINE, WhatsApp, generic webhooks (inbound + outbound).
- **Automation** — Native cron scheduler (`claw-cron`) + Python automation API.
- **Persistent memory** — Agent remembers context across sessions via `claw-mem`.
- **Plugin system** — Extend the agent with plugins via `claw-plugin`.
- **Model agnostic** — Ollama, OpenAI, or Anthropic Claude.
- **Private by default** — No data leaves your machine unless you allow it.
- **Docker & bare metal** — Run as a container or build an ISO for dedicated hardware.
- **XFCE desktop** — Full graphical desktop environment for bare metal installs; canvas, browser, media, TTS all work on XFCE.
- **Hook system** — Register HTTP webhooks for any event via the gateway.
- **Device pairing** — Securely pair remote devices with `claw-pair`.

## Quick Start (Docker)

```bash
# Clone the repository
git clone https://github.com/deliverancedigital/claw-linux.git
cd claw-linux

# Build and start the agent
make build
make run

# Or use Docker Compose for the full stack (agent + Ollama)
docker compose up -d
```

## Running Individual Services

```bash
# Start the native gateway control plane
docker run --rm -it -e CLAW_MODE=gateway -p 18789:18789 claw-linux

# Start the channel adapter (Telegram/Discord/Slack webhooks)
docker run --rm -it -e CLAW_MODE=channel -p 18790:18790 claw-linux

# Start the cron automation scheduler
docker run --rm -it -e CLAW_MODE=cron claw-linux

# Interactive shell
make shell

# Full stack via Compose profiles
docker compose --profile gateway up -d    # gateway + channel adapter
docker compose --profile automation up -d # cron scheduler
docker compose --profile api up -d        # REST API
```

## Service Manager (claw-daemon)

`claw-daemon` manages the lifecycle of all claw-linux services using PID files.  It is the
recommended way to run services on bare metal without a full init system.

```bash
# Start all services
claw-daemon start gateway
claw-daemon start channel
claw-daemon start cron
claw-daemon start agent

# Check status of all services
claw-daemon status

# Stop a service
claw-daemon stop cron

# Restart a service
claw-daemon restart gateway

# Reload claw-cron crontab without restarting
claw-daemon reload cron
```

PID files are written to `/var/run/claw/` and logs go to `/var/log/claw/`.

## Terminal Chat UI (claw-tui)

`claw-tui` is an interactive terminal interface for chatting with the agent via the gateway.

```bash
# Connect to the local gateway and chat
claw-tui

# Connect to a remote gateway
claw-tui -g http://my-server:18789

# Use a named session
claw-tui -s my-project
```

Inside `claw-tui`:
- Type a message and press Enter to send it to the agent
- `/status` — check gateway health
- `/session <ID>` — switch to a different conversation session
- `/clear` — clear the screen
- `/help` — show all commands
- `/quit` or Ctrl-D — exit

## Building from Source

```bash
# Build all 8 C binaries locally (requires gcc + libcurl-dev)
make binaries

# Run all smoke tests
make test

# Build the Docker image
make build

# Open a shell inside the container
make shell
```

## Bare Metal Installation (Alpine Linux + XFCE)

### Option A: Build a bootable ISO

```bash
# Build using Docker (recommended — no host dependencies)
make iso-docker

# Or build directly on an Alpine host
make iso

# The ISO is written to dist/claw-linux-<date>.iso
# Write to USB (replace /dev/sdX):
dd if=dist/claw-linux-*.iso of=/dev/sdX bs=4M status=progress
```

The ISO boots into a choice of:
- **XFCE Desktop** — full graphical environment with the claw agent terminal auto-started
- **Agent-only (headless)** — claw agent running as a service, accessible via SSH/serial
- **Shell only** — minimal Alpine shell

### Option B: Install onto an existing Alpine system

```bash
# On a fresh Alpine Linux installation (as root):
sh /opt/claw/scripts/setup-desktop.sh
```

This installs XFCE, LightDM (with auto-login as `claw`), all OpenRC services, and the claw agent desktop autostart entry.

### OpenRC Services (Alpine bare metal)

After installation, services are managed with OpenRC:

```bash
# Start all services
rc-service claw-gateway start
rc-service claw-channel start
rc-service claw-cron    start
rc-service claw-agent   start

# Enable on boot
rc-update add claw-gateway default
rc-update add claw-channel default
rc-update add claw-cron    default
rc-update add claw-agent   default
```

OpenRC init scripts are in `scripts/init/`:

| Script | Service | Description |
|---|---|---|
| `claw-gateway` | claw-gateway | HTTP gateway daemon |
| `claw-channel` | claw-channel | Channel webhook adapter |
| `claw-cron` | claw-cron | Cron automation scheduler |
| `claw-agent` | claw-agent | Python AI agent |

## Configuration

Copy the example configuration and edit to your needs:

```bash
cp config/agent.yaml config/agent.local.yaml
# Edit config/agent.local.yaml with your API keys and preferences
```

Key settings in `config/agent.yaml`:

| Setting | Description |
|---|---|
| `model.provider` | LLM provider: `openai`, `anthropic`, or `ollama` |
| `model.name` | Model name (e.g. `gpt-4o`, `claude-3-5-sonnet`, `llama3.2`) |
| `agent.name` | Name of your agent |
| `gateway.port` | Port for the native HTTP gateway (default: 18789) |
| `channel_adapter.port` | Port for the webhook channel adapter (default: 18790) |
| `automation.crontab` | Path to the crontab file for `claw-cron` |
| `skills.channel.channels.telegram.token` | Telegram bot token |
| `skills.channel.channels.discord.webhook_url` | Discord incoming webhook URL |
| `skills.channel.channels.slack.webhook_url` | Slack incoming webhook URL |

## Channel Integration

### Inbound (receiving messages)

Point your platform's webhook at the `claw-channel` adapter:

| Platform | Webhook URL |
|---|---|
| Telegram | `http://your-host:18790/channel/telegram` |
| Discord | `http://your-host:18790/channel/discord` |
| Slack | `http://your-host:18790/channel/slack` |
| Generic | `http://your-host:18790/channel/webhook` |

### Outbound (agent sending messages)

The agent uses the `channel` skill:

```
SKILL_CALL: {"skill": "channel", "channel": "telegram", "message": "Hello!", "recipient": "@username"}
SKILL_CALL: {"skill": "channel", "channel": "discord",  "message": "Task complete."}
SKILL_CALL: {"skill": "channel", "channel": "slack",    "message": "Done."}
```

## Automation (Crontab)

Edit `config/crontab` to schedule automated tasks:

```cron
# Run a health check every 5 minutes
*/5 * * * * echo '{"command":"echo ping","timeout":5}' | /usr/local/bin/claw-shell

# Daily memory summarisation at 02:00
0 2 * * * python3 /opt/claw/agent/main.py "Summarise today's memory"

# Run once at startup
@reboot python3 /opt/claw/agent/main.py "System started. Report status."
```

Send `SIGHUP` to `claw-cron` to reload without restarting.

## Project Structure

```
claw-linux/
├── Dockerfile              # Multi-stage: builder + runtime + iso-builder
├── docker-compose.yml      # Full stack: agent, gateway, channel, cron, Ollama
├── Makefile                # Build, test, ISO, and compose targets
├── config/
│   ├── packages.txt        # Alpine apk packages
│   ├── agent.yaml          # Agent + gateway + channel + automation config
│   └── crontab             # Default automation schedule
├── scripts/
│   ├── entrypoint.sh       # Container entrypoint (all CLAW_MODE values)
│   ├── setup-agent.sh      # Python dependency installer
│   ├── setup-desktop.sh    # Bare metal XFCE desktop installer
│   ├── build-iso.sh        # Alpine + XFCE bootable ISO builder
│   └── init/               # OpenRC init scripts for bare metal
│       ├── claw-gateway    # Gateway service
│       ├── claw-channel    # Channel adapter service
│       ├── claw-cron       # Cron scheduler service
│       └── claw-agent      # Python agent service
├── src/                    # Native C binaries (no Node.js)
│   ├── Makefile
│   ├── common/
│   │   └── claw_json.h     # Minimal JSON helper (shared by all binaries)
│   ├── claw-gateway/       # HTTP control plane gateway
│   ├── claw-channel/       # Webhook channel adapter
│   ├── claw-shell/         # Shell execution skill
│   ├── claw-fs/            # Filesystem skill
│   ├── claw-fetch/         # Web fetch skill
│   ├── claw-cron/          # Cron automation scheduler
│   ├── claw-daemon/        # Service lifecycle manager
│   └── claw-tui/           # Interactive terminal chat UI
└── agent/
    ├── main.py             # Agent entry point
    ├── requirements.txt    # Python dependencies
    ├── core/
    │   ├── agent.py        # ReAct agent loop
    │   └── config.py       # Configuration loader
    ├── skills/
    │   ├── shell.py        # Shell skill (wraps claw-shell)
    │   ├── filesystem.py   # Filesystem skill (wraps claw-fs)
    │   ├── web.py          # Web skill (wraps claw-fetch)
    │   └── channel.py      # Channel dispatch skill (outbound)
    ├── automation/
    │   └── cron.py         # Crontab read/write API
    └── memory/
        └── store.py        # Persistent memory store
```

## Environment Variables

| Variable | Description | Default |
|---|---|---|
| `CLAW_MODE` | Service mode: `agent\|api\|gateway\|channel\|cron\|desktop\|shell` | `agent` |
| `OPENCLAW_MODEL_PROVIDER` | LLM provider | `ollama` |
| `OPENCLAW_MODEL_NAME` | Model name | `llama3.2` |
| `OPENCLAW_OLLAMA_HOST` | Ollama API host | `http://localhost:11434` |
| `OPENCLAW_OPENAI_API_KEY` | OpenAI API key | — |
| `OPENCLAW_ANTHROPIC_API_KEY` | Anthropic API key | — |
| `OPENCLAW_AGENT_NAME` | Agent display name | `Claw` |
| `OPENCLAW_LOG_LEVEL` | Log level | `INFO` |
| `CLAW_GATEWAY_PORT` | Gateway listen port | `18789` |
| `CLAW_GATEWAY_BIND` | Gateway bind address | `0.0.0.0` |
| `CLAW_CHANNEL_PORT` | Channel adapter port | `18790` |
| `CLAW_CHANNEL_SECRET` | Shared secret for webhook validation | — |
| `CLAW_TELEGRAM_TOKEN` | Telegram bot token | — |
| `CLAW_DISCORD_WEBHOOK` | Discord incoming webhook URL | — |
| `CLAW_SLACK_WEBHOOK` | Slack incoming webhook URL | — |
| `CLAW_CRONTAB` | Path to crontab file | `/opt/claw/config/crontab` |

## Security

- All C binaries enforce blocked-command policies and path allowlists at the kernel syscall level.
- The agent runs as an unprivileged `claw` user in Docker.
- Webhook validation uses a shared secret (`CLAW_CHANNEL_SECRET`) via `X-Claw-Secret` header.
- TLS peer verification is enforced on all outbound HTTPS requests (`claw-fetch`).
- Never expose the gateway or channel adapter ports to the public internet without authentication.

## License

MIT — see [LICENSE](LICENSE).