"""Trial ledger: every configuration ever evaluated against this dataset.

The deflated Sharpe ratio is only honest if it is penalised for every trial, not just
the ones in the current run. docs/trials.jsonl is committed and append-only. One line
is one run; `n_configs` says how many configurations that run evaluated, and
`var_sr_trials` (per-period Sharpe variance across those configurations) is recorded
where a run produced one, so Stage 2 can floor the DSR variance with it.
"""
from __future__ import annotations

import datetime as dt
import hashlib
import json
from pathlib import Path
from typing import Any

from autotrader.schemas import find_project_root

KINDS = ("walkforward", "pretest", "sweep", "holdout", "unseal")


def default_path() -> Path:
    return find_project_root(Path(__file__).parent) / "docs" / "trials.jsonl"


class Ledger:
    def __init__(self, path: Path | None = None):
        self.path = Path(path) if path else default_path()

    def read(self) -> list[dict[str, Any]]:
        if not self.path.exists():
            return []
        with open(self.path, encoding="utf-8") as f:
            return [json.loads(line) for line in f if line.strip()]

    def append(self, *, kind: str, hypothesis: str, n_configs: int, data_window: str, notes: str,
               spec_hash: str = "", var_sr_trials: float | None = None, extra: dict | None = None) -> dict[str, Any]:
        if kind not in KINDS:
            raise ValueError(f"ledger kind must be one of {KINDS}, got {kind!r}")
        if n_configs < 0:
            raise ValueError("n_configs must be >= 0")
        entry: dict[str, Any] = {
            "date": dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "kind": kind, "hypothesis": hypothesis, "n_configs": int(n_configs), "data_window": data_window,
            "spec_hash": spec_hash, "var_sr_trials": var_sr_trials, "notes": notes,
        }
        if extra:
            entry["extra"] = extra
        entry["id"] = hashlib.sha256(json.dumps(entry, sort_keys=True).encode()).hexdigest()[:12]
        self.path.parent.mkdir(parents=True, exist_ok=True)
        with open(self.path, "a", encoding="utf-8", newline="\n") as f:
            f.write(json.dumps(entry, sort_keys=True) + "\n")
        return entry

    def count(self) -> int:
        """Total configurations evaluated so far (the DSR trial count)."""
        return sum(int(e.get("n_configs", 0)) for e in self.read())

    def max_var_sr_trials(self) -> float:
        vals = [float(e["var_sr_trials"]) for e in self.read() if e.get("var_sr_trials") is not None]
        return max(vals) if vals else 0.0
