# claw-linux

**claw-linux** is an Alpine Linux-based operating system distribution built to run [OpenClaw](https://open-claw.org/) — an open-source, locally-hosted autonomous AI agent that acts on your behalf while keeping all data private on your own hardware.

## Features

- **Minimal footprint** — Built on Alpine Linux for a lean, fast, secure base.
- **Autonomous AI agent** — Full OpenClaw-compatible agent runtime included.
- **Skill/plugin system** — Shell execution, filesystem access, web browsing, and more.
- **Persistent memory** — Agent remembers context and preferences across sessions.
- **Model agnostic** — Works with OpenAI, Anthropic Claude, or local models via Ollama.
- **Private by default** — No data leaves your machine unless you explicitly allow it.
- **Docker & bare-metal** — Run as a container or build an ISO for dedicated hardware.

## Quick Start (Docker)

```bash
# Clone the repository
git clone https://github.com/deliverancedigital/claw-linux.git
cd claw-linux

# Build and start
make build
make run

# Or use Docker Compose for the full stack
docker compose up -d
```

## Building from Source

```bash
# Build the Docker image
make build

# Run an interactive shell inside the OS
make shell

# Run the autonomous agent
make agent

# View agent logs
make logs
```

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
| `agent.memory_enabled` | Enable persistent memory across sessions |
| `skills.*` | Enable/disable individual skill modules |

## Project Structure

```
claw-linux/
├── Dockerfile              # Alpine Linux OS image definition
├── docker-compose.yml      # Multi-service deployment
├── Makefile                # Build and management automation
├── config/
│   ├── packages.txt        # Alpine apk packages installed in the OS
│   └── agent.yaml          # Default agent configuration
├── scripts/
│   ├── entrypoint.sh       # Container startup entrypoint
│   └── setup-agent.sh      # Agent dependency installation script
└── agent/
    ├── main.py             # Agent entry point
    ├── requirements.txt    # Python dependencies
    ├── core/
    │   ├── agent.py        # Core autonomous agent loop
    │   └── config.py       # Configuration loader
    ├── skills/
    │   ├── shell.py        # Shell command execution skill
    │   ├── filesystem.py   # File system read/write skill
    │   └── web.py          # Web search/fetch skill
    └── memory/
        └── store.py        # Persistent memory store
```

## Environment Variables

| Variable | Description | Default |
|---|---|---|
| `OPENCLAW_MODEL_PROVIDER` | LLM provider | `ollama` |
| `OPENCLAW_MODEL_NAME` | Model name | `llama3.2` |
| `OPENCLAW_OLLAMA_HOST` | Ollama API host | `http://localhost:11434` |
| `OPENCLAW_OPENAI_API_KEY` | OpenAI API key | — |
| `OPENCLAW_ANTHROPIC_API_KEY` | Anthropic API key | — |
| `OPENCLAW_AGENT_NAME` | Agent display name | `Claw` |
| `OPENCLAW_LOG_LEVEL` | Log level | `INFO` |

## Security

The agent can execute shell commands and access the filesystem. To reduce risk:

- Run in an unprivileged Docker container (default in this repo).
- Restrict Docker volume mounts to only directories the agent needs.
- Use read-only mounts where write access is not required.
- Audit enabled skills in `config/agent.yaml` and disable what you don't need.
- Never expose the agent API port to the public internet without authentication.

## License

MIT — see [LICENSE](LICENSE).