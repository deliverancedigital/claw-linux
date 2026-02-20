"""
agent/core/config.py — Configuration loader for the claw-linux autonomous agent.

Loads settings from (in priority order, highest first):
  1. Environment variables  (OPENCLAW_<SECTION>_<KEY> in upper-case)
  2. config/agent.local.yaml  (user overrides, gitignored)
  3. config/agent.yaml        (project defaults)
"""
from __future__ import annotations

import os
import yaml
from pathlib import Path

# Root of the repository (two levels up from this file)
_REPO_ROOT = Path(__file__).resolve().parents[2]

_DEFAULT_CONFIG_PATH = _REPO_ROOT / "config" / "agent.yaml"
_LOCAL_CONFIG_PATH   = _REPO_ROOT / "config" / "agent.local.yaml"


def _deep_merge(base: dict, override: dict) -> dict:
    """Recursively merge *override* into *base*, returning a new dict."""
    result = dict(base)
    for k, v in override.items():
        if k in result and isinstance(result[k], dict) and isinstance(v, dict):
            result[k] = _deep_merge(result[k], v)
        else:
            result[k] = v
    return result


def _load_yaml(path: Path) -> dict:
    if not path.exists():
        return {}
    with open(path, "r", encoding="utf-8") as f:
        data = yaml.safe_load(f)
    return data if isinstance(data, dict) else {}


def _apply_env(cfg: dict, prefix: str = "OPENCLAW") -> dict:
    """
    Override config values from environment variables.

    Mapping convention:  OPENCLAW_MODEL_PROVIDER  →  cfg["model"]["provider"]
    Section separator is a single underscore between the section and key names.
    """
    for key, value in os.environ.items():
        if not key.startswith(prefix + "_"):
            continue
        parts = key[len(prefix) + 1:].lower().split("_", 1)
        if len(parts) == 2:
            section, field = parts
            if section in cfg and isinstance(cfg[section], dict):
                if field in cfg[section]:
                    # Attempt type coercion to match existing type
                    existing = cfg[section][field]
                    if isinstance(existing, bool):
                        cfg[section][field] = value.lower() in ("1", "true", "yes")
                    elif isinstance(existing, int):
                        try:
                            cfg[section][field] = int(value)
                        except ValueError:
                            cfg[section][field] = value
                    elif isinstance(existing, float):
                        try:
                            cfg[section][field] = float(value)
                        except ValueError:
                            cfg[section][field] = value
                    else:
                        cfg[section][field] = value
    return cfg


class Config:
    """Parsed and validated agent configuration."""

    def __init__(self) -> None:
        base     = _load_yaml(_DEFAULT_CONFIG_PATH)
        local    = _load_yaml(_LOCAL_CONFIG_PATH)
        merged   = _deep_merge(base, local)
        merged   = _apply_env(merged)
        self._cfg = merged

    def get(self, *keys, default=None):
        """Retrieve a nested config value by a sequence of keys."""
        node = self._cfg
        for k in keys:
            if not isinstance(node, dict) or k not in node:
                return default
            node = node[k]
        return node

    # ---- convenience accessors ----

    @property
    def agent_name(self) -> str:
        return self.get("agent", "name", default="Claw")

    @property
    def memory_enabled(self) -> bool:
        return bool(self.get("agent", "memory_enabled", default=True))

    @property
    def memory_dir(self) -> str:
        return self.get("agent", "memory_dir", default="/var/lib/claw/memory")

    @property
    def context_window(self) -> int:
        return int(self.get("agent", "context_window", default=20))

    @property
    def log_level(self) -> str:
        return self.get("agent", "log_level", default="INFO").upper()

    @property
    def model_provider(self) -> str:
        return self.get("model", "provider", default="ollama").lower()

    @property
    def model_name(self) -> str:
        return self.get("model", "name", default="llama3.2")

    @property
    def model_temperature(self) -> float:
        return float(self.get("model", "temperature", default=0.3))

    @property
    def model_max_tokens(self) -> int:
        return int(self.get("model", "max_tokens", default=4096))

    @property
    def ollama_host(self) -> str:
        return self.get("model", "ollama", "host",
                        default="http://localhost:11434")

    @property
    def openai_api_key(self) -> str:
        return (os.environ.get("OPENCLAW_OPENAI_API_KEY")
                or self.get("model", "openai", "api_key", default=""))

    @property
    def anthropic_api_key(self) -> str:
        return (os.environ.get("OPENCLAW_ANTHROPIC_API_KEY")
                or self.get("model", "anthropic", "api_key", default=""))

    def skill_enabled(self, skill: str) -> bool:
        return bool(self.get("skills", skill, "enabled", default=True))

    def skill_cfg(self, skill: str) -> dict:
        return self.get("skills", skill) or {}
