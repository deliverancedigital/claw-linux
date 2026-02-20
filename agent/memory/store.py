"""
agent/memory/store.py — Persistent memory store for the claw-linux agent.

Uses a simple JSON-lines file as the backing store so that the agent retains
context and facts across sessions without requiring an external database.
"""
from __future__ import annotations

import json
import logging
import os
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

logger = logging.getLogger(__name__)

_TIMESTAMP_FMT = "%Y-%m-%dT%H:%M:%SZ"


class MemoryStore:
    """
    Append-only memory store backed by a JSON-lines file.

    Each line in the file is one JSON record:
        {"ts": "<ISO-8601>", "role": "user|assistant|fact", "content": "..."}
    """

    def __init__(self, memory_dir: str) -> None:
        self._dir  = Path(memory_dir)
        self._dir.mkdir(parents=True, exist_ok=True)
        self._path = self._dir / "memory.jsonl"

    # ---- write ----

    def append(self, role: str, content: str, meta: dict | None = None) -> None:
        """Append a new record to the memory store."""
        record: dict[str, Any] = {
            "ts":      datetime.now(timezone.utc).strftime(_TIMESTAMP_FMT),
            "role":    role,
            "content": content,
        }
        if meta:
            record["meta"] = meta

        try:
            with open(self._path, "a", encoding="utf-8") as f:
                f.write(json.dumps(record, ensure_ascii=False) + "\n")
        except OSError as exc:
            logger.error("Failed to write to memory store: %s", exc)

    def add_fact(self, fact: str) -> None:
        """Store a standalone fact (not tied to a conversation turn)."""
        self.append("fact", fact)

    # ---- read ----

    def load_recent(self, limit: int = 20) -> list[dict[str, Any]]:
        """Return the *limit* most-recent records in chronological order."""
        if not self._path.exists():
            return []
        records: list[dict[str, Any]] = []
        try:
            with open(self._path, "r", encoding="utf-8") as f:
                for line in f:
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        records.append(json.loads(line))
                    except json.JSONDecodeError:
                        pass
        except OSError as exc:
            logger.error("Failed to read memory store: %s", exc)
            return []
        return records[-limit:]

    def load_facts(self) -> list[str]:
        """Return all stored facts."""
        return [
            r["content"]
            for r in self.load_recent(limit=10_000)
            if r.get("role") == "fact"
        ]

    # ---- maintenance ----

    def clear(self) -> None:
        """Delete all memory records."""
        try:
            if self._path.exists():
                os.remove(self._path)
            logger.info("Memory store cleared.")
        except OSError as exc:
            logger.error("Failed to clear memory store: %s", exc)

    def size(self) -> int:
        """Return the number of records currently stored."""
        if not self._path.exists():
            return 0
        count = 0
        try:
            with open(self._path, "r", encoding="utf-8") as f:
                for line in f:
                    if line.strip():
                        count += 1
        except OSError:
            pass
        return count
