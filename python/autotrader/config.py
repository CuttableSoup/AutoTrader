"""Runtime config (config/paper.json) + secrets backends (env | file | aws-secretsmanager)."""
from __future__ import annotations

import json
import os
import subprocess
from pathlib import Path
from typing import Any

from .schemas import find_project_root

SECRET_KEYS = ("alpaca_key_id", "alpaca_secret_key", "anthropic_api_key", "fmp_api_key", "finnhub_api_key", "nasdaq_data_link_api_key", "watchdog_token")


class Config:
    def __init__(self, root: dict[str, Any], base_dir: Path):
        self.root = root
        self.base_dir = base_dir

    @classmethod
    def load(cls, path: str | Path | None = None) -> "Config":
        base = find_project_root()
        p = Path(path) if path else base / "config" / "paper.json"
        if not p.is_absolute():
            p = base / p
        if not p.exists():
            example = base / "config" / "paper.example.json"
            if p.name == "paper.json" and example.exists():
                p = example
            else:
                raise FileNotFoundError(f"config not found: {p}")
        return cls(json.loads(p.read_text(encoding="utf-8")), base)

    def get(self, dotted: str, default: Any = None) -> Any:
        cur: Any = self.root
        for tok in dotted.split("."):
            if not isinstance(cur, dict) or tok not in cur:
                return default
            cur = cur[tok]
        return cur

    def require(self, dotted: str) -> Any:
        v = self.get(dotted, None)
        if v is None:
            raise KeyError(f"config: missing required key '{dotted}'")
        return v

    def resolve(self, rel: str) -> Path:
        p = Path(rel)
        return p if p.is_absolute() else self.base_dir / p


class Secrets:
    def __init__(self, kv: dict[str, str], backend: str):
        self._kv = kv
        self.backend = backend

    @classmethod
    def load(cls, cfg: Config) -> "Secrets":
        backend = cfg.get("secrets.backend", "env")
        kv: dict[str, str] = {}
        if backend == "env":
            for k in SECRET_KEYS:
                v = os.environ.get("AT_" + k.upper())
                if v:
                    kv[k] = v
        elif backend == "file":
            p = cfg.resolve(cfg.get("secrets.file", "config/secrets.json"))
            data = json.loads(p.read_text(encoding="utf-8"))
            kv = {k: v for k, v in data.items() if isinstance(v, str) and not k.startswith("_")}
        elif backend == "aws-secretsmanager":
            name = cfg.require("secrets.aws_secret_name")
            out = subprocess.run(["aws", "secretsmanager", "get-secret-value", "--secret-id", name, "--query", "SecretString", "--output", "text"], check=True, capture_output=True, text=True).stdout
            kv = {k: v for k, v in json.loads(out).items() if isinstance(v, str)}
        else:
            raise ValueError(f"secrets: unknown backend {backend}")
        return cls(kv, backend)

    def require(self, key: str) -> str:
        v = self._kv.get(key)
        if not v:
            raise KeyError(f"secrets: missing '{key}' (backend {self.backend})")
        return v

    def get(self, key: str, default: str = "") -> str:
        return self._kv.get(key, default)

    def has(self, key: str) -> bool:
        return bool(self._kv.get(key))
