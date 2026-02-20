"""
agent/skills/filesystem.py — Filesystem skill backed by the claw-fs C binary.

The C binary (src/claw-fs) handles all path safety:
  • realpath(3) resolution to prevent directory traversal
  • allowed-path allowlist enforcement
  • read-only path enforcement for writes

Supported operations: read, write, list.
"""
from __future__ import annotations

import json
import logging
import subprocess
from pathlib import Path
from typing import Any

logger = logging.getLogger(__name__)

_BIN = Path(__file__).resolve().parents[2] / "src" / "bin" / "claw-fs"


def _binary_available() -> bool:
    return _BIN.exists() and _BIN.is_file()


def _call_binary(payload: dict) -> dict[str, Any]:
    """Send *payload* as JSON to claw-fs on stdin and return the parsed response."""
    if not _binary_available():
        raise RuntimeError(f"claw-fs binary not found at {_BIN}")

    raw = json.dumps(payload)
    try:
        proc = subprocess.run(
            [str(_BIN)],
            input=raw.encode(),
            capture_output=True,
            timeout=10,
        )
    except subprocess.TimeoutExpired:
        return {"ok": False, "error": "claw-fs binary timed out"}
    except OSError as exc:
        logger.error("Failed to launch claw-fs: %s", exc)
        return {"ok": False, "error": str(exc)}

    try:
        return json.loads(proc.stdout.decode(errors="replace"))
    except json.JSONDecodeError as exc:
        return {"ok": False, "error": f"JSON decode error: {exc}"}


def read(path: str) -> dict[str, Any]:
    """
    Read the contents of a file.

    Returns ``{"ok": True, "content": "<text>"}`` or ``{"ok": False, "error": "..."}``.
    """
    return _call_binary({"op": "read", "path": path})


def write(path: str, content: str) -> dict[str, Any]:
    """
    Write *content* to a file (creates or truncates).

    Returns ``{"ok": True}`` or ``{"ok": False, "error": "..."}``.
    """
    return _call_binary({"op": "write", "path": path, "content": content})


def list_dir(path: str) -> dict[str, Any]:
    """
    List the entries in a directory.

    Returns ``{"ok": True, "entries": ["file.txt", "subdir/"]}``
    or ``{"ok": False, "error": "..."}``.
    """
    return _call_binary({"op": "list", "path": path})
