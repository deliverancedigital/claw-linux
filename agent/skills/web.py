"""
agent/skills/web.py — Web fetch skill backed by the claw-fetch C binary.

The C binary (src/claw-fetch) handles:
  • URL scheme validation (http/https only)
  • libcurl TLS peer verification
  • Response size capping
  • Per-request timeout enforcement
"""
from __future__ import annotations

import json
import logging
import subprocess
from pathlib import Path
from typing import Any

logger = logging.getLogger(__name__)

_BIN = Path(__file__).resolve().parents[2] / "src" / "bin" / "claw-fetch"

_DEFAULT_TIMEOUT   = 15
_DEFAULT_MAX_BYTES = 1_048_576   # 1 MiB


def _binary_available() -> bool:
    return _BIN.exists() and _BIN.is_file()


def fetch(
    url: str,
    method: str = "GET",
    timeout: int = _DEFAULT_TIMEOUT,
    max_bytes: int = _DEFAULT_MAX_BYTES,
) -> dict[str, Any]:
    """
    Fetch *url* via the claw-fetch C binary.

    Returns:
        {"ok": True,  "status_code": 200, "body": "<response body>"}
        {"ok": False, "error": "<reason>"}
    """
    if not _binary_available():
        logger.warning("claw-fetch binary not found at %s; falling back to Python", _BIN)
        return _python_fallback(url, method, timeout, max_bytes)

    payload = json.dumps({
        "url":       url,
        "method":    method.upper(),
        "timeout":   timeout,
        "max_bytes": max_bytes,
    })

    try:
        proc = subprocess.run(
            [str(_BIN)],
            input=payload.encode(),
            capture_output=True,
            timeout=timeout + 5,
        )
    except subprocess.TimeoutExpired:
        return {"ok": False, "error": "claw-fetch binary timed out"}
    except OSError as exc:
        logger.error("Failed to launch claw-fetch: %s", exc)
        return {"ok": False, "error": str(exc)}

    try:
        return json.loads(proc.stdout.decode(errors="replace"))
    except json.JSONDecodeError as exc:
        return {"ok": False, "error": f"JSON decode error: {exc}"}


def _python_fallback(
    url: str, method: str, timeout: int, max_bytes: int
) -> dict[str, Any]:
    """Pure-Python fallback using the `requests` library."""
    try:
        import requests

        if not url.startswith(("http://", "https://")):
            return {"ok": False, "error": "URL must start with http:// or https://"}

        resp = requests.request(
            method,
            url,
            timeout=timeout,
            stream=True,
        )
        body = b""
        for chunk in resp.iter_content(chunk_size=4096):
            body += chunk
            if len(body) >= max_bytes:
                break

        return {
            "ok": True,
            "status_code": resp.status_code,
            "body": body.decode(errors="replace"),
        }
    except Exception as exc:
        return {"ok": False, "error": str(exc)}
