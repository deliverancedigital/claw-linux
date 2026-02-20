"""
agent/skills/shell.py — Shell execution skill backed by the claw-shell C binary.

The C binary (src/claw-shell) handles all sandboxing concerns:
  • blocked-command policy enforcement
  • child-process timeout via SIGALRM
  • stdout/stderr capture via pipes

This module is responsible only for locating the binary, invoking it with the
correct JSON protocol, and returning a structured result.
"""
from __future__ import annotations

import json
import logging
import subprocess
from pathlib import Path
from typing import Any

logger = logging.getLogger(__name__)

# Resolved path to the compiled C binary
_BIN = Path(__file__).resolve().parents[2] / "src" / "bin" / "claw-shell"


def _binary_available() -> bool:
    return _BIN.exists() and _BIN.is_file()


def run(command: str, timeout: int = 30) -> dict[str, Any]:
    """
    Execute *command* via /bin/sh -c inside the claw-shell C binary.

    Returns a dict with keys:
        ok        (bool)  — True if the binary ran the command (even if it exited non-zero)
        exit_code (int)   — Shell exit status
        stdout    (str)   — Captured standard output
        stderr    (str)   — Captured standard error
        error     (str)   — Present only when ok is False (policy block or binary error)
    """
    if not _binary_available():
        logger.warning("claw-shell binary not found at %s; falling back to Python", _BIN)
        return _python_fallback(command, timeout)

    payload = json.dumps({"command": command, "timeout": timeout})

    try:
        proc = subprocess.run(
            [str(_BIN)],
            input=payload.encode(),
            capture_output=True,
            timeout=timeout + 5,   # outer safety margin
        )
    except subprocess.TimeoutExpired:
        return {"ok": False, "error": "claw-shell binary timed out"}
    except OSError as exc:
        logger.error("Failed to launch claw-shell: %s", exc)
        return {"ok": False, "error": str(exc)}

    try:
        result = json.loads(proc.stdout.decode(errors="replace"))
    except json.JSONDecodeError as exc:
        return {"ok": False, "error": f"JSON decode error: {exc}"}

    return result


def _python_fallback(command: str, timeout: int) -> dict[str, Any]:
    """Pure-Python fallback used only when the C binary is absent."""

    BLOCKED = ["rm -rf /", "mkfs", "dd if=/dev/zero"]
    stripped = command.lstrip()
    for b in BLOCKED:
        if stripped.startswith(b):
            return {"ok": False, "error": "Command blocked by security policy"}

    try:
        proc = subprocess.run(
            ["/bin/sh", "-c", command],
            capture_output=True,
            timeout=timeout,
        )
        return {
            "ok": True,
            "exit_code": proc.returncode,
            "stdout": proc.stdout.decode(errors="replace"),
            "stderr": proc.stderr.decode(errors="replace"),
        }
    except subprocess.TimeoutExpired:
        return {"ok": False, "error": "Command timed out"}
    except Exception as exc:
        return {"ok": False, "error": str(exc)}
