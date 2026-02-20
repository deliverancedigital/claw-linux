#!/bin/sh
# scripts/claw-wizard.sh — Interactive setup wizard for claw-linux.
#
# Walks the user through configuring the claw-linux agent interactively:
# choosing an LLM provider, entering API keys, setting up channels, and
# optionally installing the XFCE desktop environment.
#
# Corresponds to: openclaw/openclaw src/wizard/ (interactive setup wizard)
#
# Usage (as root or the claw user):
#   sh /opt/claw/scripts/claw-wizard.sh
#   sh /opt/claw/scripts/claw-wizard.sh --non-interactive  (uses defaults)

set -e

# ── colours ────────────────────────────────────────────────────────────────
if [ -t 1 ]; then
    C_BOLD="\033[1m"
    C_GREEN="\033[1;32m"
    C_CYAN="\033[1;36m"
    C_YELLOW="\033[1;33m"
    C_RED="\033[1;31m"
    C_RESET="\033[0m"
else
    C_BOLD="" C_GREEN="" C_CYAN="" C_YELLOW="" C_RED="" C_RESET=""
fi

info()    { printf "${C_GREEN}==> %s${C_RESET}\n" "$*"; }
step()    { printf "\n${C_CYAN}${C_BOLD}[ %s ]${C_RESET}\n" "$*"; }
warn()    { printf "${C_YELLOW}WARN: %s${C_RESET}\n" "$*"; }
error()   { printf "${C_RED}ERROR: %s${C_RESET}\n" "$*" >&2; exit 1; }
ask()     { printf "${C_BOLD}%s${C_RESET} " "$1"; }

NON_INTERACTIVE=0
for arg in "$@"; do
    [ "$arg" = "--non-interactive" ] && NON_INTERACTIVE=1
done

prompt() {
    # prompt <question> <default>
    # Prints the question and reads a value; uses default if non-interactive.
    local question="$1" default="$2"
    if [ "$NON_INTERACTIVE" = "1" ]; then
        printf "%s\n" "$default"
        return
    fi
    ask "$question [${default}]:"
    read -r _answer
    printf "%s\n" "${_answer:-$default}"
}

prompt_secret() {
    # prompt_secret <question>
    local question="$1"
    if [ "$NON_INTERACTIVE" = "1" ]; then
        printf "\n"
        return
    fi
    ask "$question (hidden):"
    stty -echo 2>/dev/null || true
    read -r _secret
    stty echo  2>/dev/null || true
    printf "\n"
    printf "%s\n" "$_secret"
}

# ── header ─────────────────────────────────────────────────────────────────

printf "\n"
printf "${C_BOLD}╔═══════════════════════════════════════════════════╗${C_RESET}\n"
printf "${C_BOLD}║          claw-linux Setup Wizard                  ║${C_RESET}\n"
printf "${C_BOLD}╚═══════════════════════════════════════════════════╝${C_RESET}\n\n"
printf "This wizard will configure your claw-linux agent.\n"
printf "Press Enter to accept defaults shown in [brackets].\n\n"

CLAW_HOME="${CLAW_HOME:-/opt/claw}"
CONFIG_FILE="${CLAW_HOME}/config/agent.yaml"
CONFIG_EXAMPLE="${CLAW_HOME}/config/agent.yaml"

if [ ! -f "$CONFIG_EXAMPLE" ]; then
    warn "Could not find $CONFIG_EXAMPLE — wizard will create a minimal config."
fi

# Temp file for accumulated config overrides (env var format)
OVERRIDES_FILE="$(mktemp /tmp/claw-wizard-XXXXXX.env)"
trap 'rm -f "$OVERRIDES_FILE"' EXIT

add_override() {
    printf "%s=%s\n" "$1" "$2" >> "$OVERRIDES_FILE"
}

# ── Step 1: LLM provider ───────────────────────────────────────────────────

step "1/6  LLM Provider"
printf "Choose how the agent will run its language model:\n"
printf "  1) ollama   — local model via Ollama (no API key required)\n"
printf "  2) openai   — OpenAI GPT models\n"
printf "  3) anthropic — Anthropic Claude models\n\n"

PROVIDER_CHOICE="$(prompt "Provider [1/2/3]" "1")"
case "$PROVIDER_CHOICE" in
    2|openai)    PROVIDER="openai" ;;
    3|anthropic) PROVIDER="anthropic" ;;
    *)           PROVIDER="ollama" ;;
esac
info "Provider: $PROVIDER"
add_override "OPENCLAW_MODEL_PROVIDER" "$PROVIDER"

# ── Step 2: Model name ─────────────────────────────────────────────────────

step "2/6  Model Name"
case "$PROVIDER" in
    openai)
        MODEL_DEFAULT="gpt-4o"
        printf "Common OpenAI models: gpt-4o, gpt-4-turbo, gpt-3.5-turbo\n"
        ;;
    anthropic)
        MODEL_DEFAULT="claude-3-5-sonnet-20241022"
        printf "Common Claude models: claude-3-5-sonnet-20241022, claude-3-opus-20240229\n"
        ;;
    *)
        MODEL_DEFAULT="llama3.2"
        printf "Common Ollama models: llama3.2, llava, mistral, codellama\n"
        printf "(run 'ollama pull <model>' to download)\n"
        ;;
esac

MODEL="$(prompt "Model name" "$MODEL_DEFAULT")"
info "Model: $MODEL"
add_override "OPENCLAW_MODEL_NAME" "$MODEL"

# ── Step 3: API keys ────────────────────────────────────────────────────────

step "3/6  API Keys"

if [ "$PROVIDER" = "openai" ]; then
    OPENAI_KEY="$(prompt_secret "OpenAI API key (sk-…)")"
    if [ -n "$OPENAI_KEY" ]; then
        add_override "OPENCLAW_OPENAI_API_KEY" "$OPENAI_KEY"
        info "OpenAI API key saved."
    else
        warn "No OpenAI key entered — set OPENCLAW_OPENAI_API_KEY before starting the agent."
    fi
elif [ "$PROVIDER" = "anthropic" ]; then
    ANTHROPIC_KEY="$(prompt_secret "Anthropic API key (sk-ant-…)")"
    if [ -n "$ANTHROPIC_KEY" ]; then
        add_override "OPENCLAW_ANTHROPIC_API_KEY" "$ANTHROPIC_KEY"
        info "Anthropic API key saved."
    else
        warn "No Anthropic key entered — set OPENCLAW_ANTHROPIC_API_KEY before starting."
    fi
else
    OLLAMA_HOST="$(prompt "Ollama host URL" "http://localhost:11434")"
    add_override "OPENCLAW_OLLAMA_HOST" "$OLLAMA_HOST"
    info "Ollama host: $OLLAMA_HOST"
fi

# ── Step 4: Agent identity ─────────────────────────────────────────────────

step "4/6  Agent Identity"
AGENT_NAME="$(prompt "Agent name" "Claw")"
add_override "OPENCLAW_AGENT_NAME" "$AGENT_NAME"
info "Agent name: $AGENT_NAME"

# ── Step 5: Channel integrations ──────────────────────────────────────────

step "5/6  Channel Integrations (optional)"
printf "Set up messaging channel webhooks (press Enter to skip any).\n\n"

TELEGRAM_TOKEN="$(prompt_secret "Telegram bot token (from @BotFather)")"
if [ -n "$TELEGRAM_TOKEN" ]; then
    add_override "CLAW_TELEGRAM_TOKEN" "$TELEGRAM_TOKEN"
    info "Telegram token saved."
fi

DISCORD_WEBHOOK="$(prompt "Discord webhook URL" "")"
if [ -n "$DISCORD_WEBHOOK" ]; then
    add_override "CLAW_DISCORD_WEBHOOK" "$DISCORD_WEBHOOK"
    info "Discord webhook saved."
fi

SLACK_WEBHOOK="$(prompt "Slack webhook URL" "")"
if [ -n "$SLACK_WEBHOOK" ]; then
    add_override "CLAW_SLACK_WEBHOOK" "$SLACK_WEBHOOK"
    info "Slack webhook saved."
fi

# ── Step 6: Desktop / environment ─────────────────────────────────────────

step "6/6  Deployment Target"
printf "  1) Docker / container\n"
printf "  2) Bare metal (Alpine Linux + XFCE desktop)\n\n"
DEPLOY="$(prompt "Deployment target [1/2]" "1")"

if [ "$DEPLOY" = "2" ]; then
    printf "\n"
    INSTALL_DESKTOP="$(prompt "Install XFCE desktop environment? (requires root)" "no")"
    if [ "$INSTALL_DESKTOP" = "yes" ] || [ "$INSTALL_DESKTOP" = "y" ]; then
        if [ "$(id -u)" != "0" ]; then
            warn "XFCE installation requires root. Skipping — run as root to install."
        else
            info "Running desktop setup…"
            sh "${CLAW_HOME}/scripts/setup-desktop.sh"
        fi
    fi
fi

# ── Write config ──────────────────────────────────────────────────────────

printf "\n"
step "Writing configuration"

# Determine output location
if [ "$DEPLOY" = "2" ]; then
    ENV_FILE="${CLAW_HOME}/config/claw.env"
else
    ENV_FILE="${CLAW_HOME}/config/claw.env"
fi

mkdir -p "$(dirname "$ENV_FILE")"
cp "$OVERRIDES_FILE" "$ENV_FILE"
chmod 600 "$ENV_FILE"

info "Configuration written to $ENV_FILE"
printf "\nTo start the agent:\n"
if [ "$DEPLOY" = "2" ]; then
    printf "  # Load env and start\n"
    printf "  set -a && . %s && set +a\n" "$ENV_FILE"
    printf "  claw-daemon start gateway\n"
    printf "  claw-daemon start agent\n"
else
    printf "  # Docker with env file:\n"
    printf "  docker run --rm -it --env-file %s claw-linux\n" "$ENV_FILE"
    printf "\n  # Or export and run compose:\n"
    printf "  set -a && . %s && set +a\n" "$ENV_FILE"
    printf "  docker compose up -d\n"
fi

printf "\n"
printf "${C_GREEN}${C_BOLD}✓ claw-linux setup complete!${C_RESET}\n\n"
