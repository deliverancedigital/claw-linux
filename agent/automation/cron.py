"""
agent/automation/cron.py — Python interface to the claw-cron scheduler.

Provides helpers for:
  • reading and writing the crontab file
  • querying job status
  • triggering the agent for @reboot / on-demand tasks

The heavy lifting (actual job firing) is done by the native claw-cron
binary.  This module handles configuration-level concerns such as adding
or listing automation rules from within the agent.
"""
from __future__ import annotations

import logging
import os
import re
from pathlib import Path
from typing import Any

logger = logging.getLogger(__name__)

_DEFAULT_CRONTAB = Path("/opt/claw/config/crontab")


# ---------------------------------------------------------------------------
# Data types
# ---------------------------------------------------------------------------

class CronJob:
    """Represents a single crontab entry."""

    def __init__(
        self,
        schedule: str,
        command: str,
        comment: str = "",
    ) -> None:
        self.schedule = schedule.strip()
        self.command  = command.strip()
        self.comment  = comment.strip()

    def to_line(self) -> str:
        parts = []
        if self.comment:
            parts.append(f"# {self.comment}")
        parts.append(f"{self.schedule} {self.command}")
        return "\n".join(parts)

    def to_dict(self) -> dict[str, Any]:
        return {
            "schedule": self.schedule,
            "command":  self.command,
            "comment":  self.comment,
        }

    @classmethod
    def from_dict(cls, d: dict) -> "CronJob":
        return cls(d.get("schedule", ""), d.get("command", ""),
                   d.get("comment", ""))


# ---------------------------------------------------------------------------
# File I/O
# ---------------------------------------------------------------------------

class CronManager:
    """Read and write the claw-cron crontab file."""

    def __init__(self, crontab_path: str | None = None) -> None:
        env_path = os.environ.get("CLAW_CRONTAB")
        self._path = Path(
            crontab_path or env_path or _DEFAULT_CRONTAB
        )

    # ---- read ----

    def load(self) -> list[CronJob]:
        """Return all active (non-comment) jobs in the crontab."""
        jobs: list[CronJob] = []
        if not self._path.exists():
            return jobs

        pending_comment = ""
        try:
            with open(self._path, "r", encoding="utf-8") as f:
                for raw in f:
                    line = raw.rstrip("\n")
                    stripped = line.strip()

                    if not stripped:
                        pending_comment = ""
                        continue

                    if stripped.startswith("#"):
                        pending_comment = stripped.lstrip("# ").strip()
                        continue

                    # Parse schedule + command
                    parts = stripped.split(None, 5)
                    if len(parts) < 6 and not stripped.startswith("@"):
                        pending_comment = ""
                        continue

                    if stripped.startswith("@"):
                        at, _, cmd = stripped.partition(" ")
                        jobs.append(CronJob(at, cmd.strip(), pending_comment))
                    else:
                        schedule = " ".join(parts[:5])
                        command  = parts[5] if len(parts) > 5 else ""
                        jobs.append(CronJob(schedule, command, pending_comment))

                    pending_comment = ""
        except OSError as exc:
            logger.error("Failed to read crontab %s: %s", self._path, exc)

        return jobs

    # ---- write ----

    def save(self, jobs: list[CronJob]) -> bool:
        """Overwrite the crontab file with *jobs*."""
        self._path.parent.mkdir(parents=True, exist_ok=True)
        try:
            with open(self._path, "w", encoding="utf-8") as f:
                f.write("# claw-linux crontab — managed by claw-cron\n")
                f.write("# Format: MIN HOUR MDAY MON WDAY COMMAND\n")
                f.write("# Specials: @reboot @daily @hourly @weekly @monthly\n\n")
                for job in jobs:
                    f.write(job.to_line() + "\n")
            return True
        except OSError as exc:
            logger.error("Failed to write crontab %s: %s", self._path, exc)
            return False

    def add(self, job: CronJob) -> bool:
        """Append *job* to the crontab."""
        jobs = self.load()
        jobs.append(job)
        return self.save(jobs)

    def remove(self, command_pattern: str) -> int:
        """Remove jobs whose command matches *command_pattern* (regex). Returns count removed."""
        jobs   = self.load()
        before = len(jobs)
        jobs   = [j for j in jobs if not re.search(command_pattern, j.command)]
        removed = before - len(jobs)
        if removed:
            self.save(jobs)
        return removed

    def list_dicts(self) -> list[dict[str, Any]]:
        """Return all jobs as plain dicts (suitable for JSON serialization)."""
        return [j.to_dict() for j in self.load()]
