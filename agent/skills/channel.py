"""
agent/skills/channel.py — Channel dispatch skill for claw-linux.

Allows the autonomous agent to send messages outbound to configured
messaging channels.  Each channel type has a dedicated sender that
forwards the message to the appropriate platform API.

Supported channels
------------------
  telegram  — Telegram Bot API (sendMessage)
  discord   — Discord webhook
  slack     — Slack Incoming Webhook
  webhook   — Generic HTTP POST webhook

Skill call format (from agent)
-------------------------------
  SKILL_CALL: {"skill": "channel", "channel": "telegram",
               "message": "Hello!", "recipient": "@user"}

Returns a structured result dict with ok/error keys.
"""
from __future__ import annotations

import json
import logging
import os
import urllib.request as ur
from typing import Any

logger = logging.getLogger(__name__)


# ---------------------------------------------------------------------------
# Telegram
# ---------------------------------------------------------------------------

def _send_telegram(message: str, recipient: str, cfg: dict) -> dict[str, Any]:
    token = cfg.get("token") or os.environ.get("CLAW_TELEGRAM_TOKEN", "")
    if not token:
        return {"ok": False, "error": "Telegram token not configured (CLAW_TELEGRAM_TOKEN)"}

    chat_id = recipient or cfg.get("default_chat_id", "")
    if not chat_id:
        return {"ok": False, "error": "No recipient/chat_id specified for Telegram"}

    url = f"https://api.telegram.org/bot{token}/sendMessage"
    payload = json.dumps({"chat_id": chat_id, "text": message}).encode()
    req = ur.Request(url, data=payload,
                     headers={"Content-Type": "application/json"})
    try:
        with ur.urlopen(req, timeout=10) as resp:
            data = json.loads(resp.read())
        return {"ok": data.get("ok", False), "channel": "telegram",
                "message_id": data.get("result", {}).get("message_id")}
    except Exception as exc:
        logger.error("Telegram send error: %s", exc)
        return {"ok": False, "error": str(exc)}


# ---------------------------------------------------------------------------
# Discord
# ---------------------------------------------------------------------------

def _send_discord(message: str, cfg: dict) -> dict[str, Any]:
    webhook_url = cfg.get("webhook_url") or os.environ.get("CLAW_DISCORD_WEBHOOK", "")
    if not webhook_url:
        return {"ok": False,
                "error": "Discord webhook URL not configured (CLAW_DISCORD_WEBHOOK)"}

    payload = json.dumps({"content": message}).encode()
    req = ur.Request(webhook_url, data=payload,
                     headers={"Content-Type": "application/json"})
    try:
        with ur.urlopen(req, timeout=10) as resp:
            status = resp.status
        return {"ok": status in (200, 204), "channel": "discord", "status_code": status}
    except Exception as exc:
        logger.error("Discord send error: %s", exc)
        return {"ok": False, "error": str(exc)}


# ---------------------------------------------------------------------------
# Slack
# ---------------------------------------------------------------------------

def _send_slack(message: str, cfg: dict) -> dict[str, Any]:
    webhook_url = cfg.get("webhook_url") or os.environ.get("CLAW_SLACK_WEBHOOK", "")
    if not webhook_url:
        return {"ok": False,
                "error": "Slack webhook URL not configured (CLAW_SLACK_WEBHOOK)"}

    payload = json.dumps({"text": message}).encode()
    req = ur.Request(webhook_url, data=payload,
                     headers={"Content-Type": "application/json"})
    try:
        with ur.urlopen(req, timeout=10) as resp:
            body = resp.read().decode(errors="replace")
        return {"ok": body.strip() == "ok", "channel": "slack", "response": body.strip()}
    except Exception as exc:
        logger.error("Slack send error: %s", exc)
        return {"ok": False, "error": str(exc)}


# ---------------------------------------------------------------------------
# Generic webhook
# ---------------------------------------------------------------------------

def _send_webhook(message: str, recipient: str, cfg: dict) -> dict[str, Any]:
    url = cfg.get("url") or os.environ.get("CLAW_WEBHOOK_URL", "")
    if not url:
        return {"ok": False,
                "error": "Webhook URL not configured (CLAW_WEBHOOK_URL)"}

    payload = json.dumps({"message": message, "sender": "claw",
                          "recipient": recipient}).encode()
    req = ur.Request(url, data=payload,
                     headers={"Content-Type": "application/json"})
    secret = cfg.get("secret") or os.environ.get("CLAW_CHANNEL_SECRET", "")
    if secret:
        req.add_header("X-Claw-Secret", secret)

    try:
        with ur.urlopen(req, timeout=10) as resp:
            status = resp.status
        return {"ok": status == 200, "channel": "webhook", "status_code": status}
    except Exception as exc:
        logger.error("Webhook send error: %s", exc)
        return {"ok": False, "error": str(exc)}


# ---------------------------------------------------------------------------
# Public dispatch
# ---------------------------------------------------------------------------

def send(
    channel: str,
    message: str,
    recipient: str = "",
    channel_cfg: dict | None = None,
) -> dict[str, Any]:
    """
    Send *message* to the named *channel*.

    Parameters
    ----------
    channel     : "telegram" | "discord" | "slack" | "webhook"
    message     : Plain text body to send.
    recipient   : Channel-specific recipient identifier (e.g. Telegram chat ID).
    channel_cfg : Optional per-channel config dict (from agent config).

    Returns a dict with at least ``{"ok": bool}``.
    """
    cfg = channel_cfg or {}
    ch  = channel.lower().strip()

    if ch == "telegram":
        return _send_telegram(message, recipient, cfg)
    elif ch == "discord":
        return _send_discord(message, cfg)
    elif ch == "slack":
        return _send_slack(message, cfg)
    elif ch in ("webhook", "generic"):
        return _send_webhook(message, recipient, cfg)
    else:
        return {"ok": False, "error": f"Unknown channel '{channel}'"}
