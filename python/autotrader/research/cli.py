"""at-research: v2 hypothesis research CLI.

  at-research build-panel [--rebuild]                 clean point-in-time panel from data/raw (sealed)
  at-research check-trials --config <backtest.json>   backtest report.prior_trials must cover the ledger
"""
from __future__ import annotations

import argparse
import json
import logging
from pathlib import Path

import numpy as np

from autotrader.research import seal
from autotrader.research.ledger import Ledger


def cmd_build_panel(a: argparse.Namespace) -> int:
    from autotrader.research.panel import build_panel

    end = seal.data_end(a.unseal, a.reason, hypothesis="build-panel")
    p = build_panel(end=seal.SEAL_DATE if end == seal.SEAL_DATE else "2099-12-31", rebuild=a.rebuild)
    print(f"panel: {p.T} sessions x {p.N} symbols, {p.dates[0]} .. {p.dates[-1]}")
    bands = p.band_masks()
    years = p.dates.astype("datetime64[Y]").astype(int) + 1970
    print(f"{'year-end':<10}" + "".join(f"{k:>10}" for k in bands))
    for y in sorted(set(years)):
        i = np.nonzero(years == y)[0][-1]
        print(f"{y:<10}" + "".join(f"{int(v[i].sum()):>10}" for v in bands.values()))
    return 0


def cmd_check_trials(a: argparse.Namespace) -> int:
    cfg = json.loads(Path(a.config).read_text(encoding="utf-8"))
    prior = int(cfg.get("report", {}).get("prior_trials", 0))
    have = Ledger().count()
    if prior < have:
        print(f"FAIL: {a.config} report.prior_trials={prior} but docs/trials.jsonl records {have} configurations")
        return 1
    print(f"OK: prior_trials={prior} >= ledger {have}")
    return 0


def main() -> None:
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    ap = argparse.ArgumentParser(prog="at-research")
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build-panel")
    b.add_argument("--rebuild", action="store_true")
    b.add_argument("--unseal", action="store_true", help="read past the hold-out seal (logged to the ledger)")
    b.add_argument("--reason", default=None)
    b.set_defaults(fn=cmd_build_panel)
    c = sub.add_parser("check-trials")
    c.add_argument("--config", required=True)
    c.set_defaults(fn=cmd_check_trials)
    a = ap.parse_args()
    raise SystemExit(a.fn(a))


if __name__ == "__main__":
    main()
