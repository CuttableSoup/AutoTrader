"""at-research: v2 hypothesis research CLI.

  at-research build-panel [--rebuild]                 clean point-in-time panel from data/raw (sealed)
  at-research pretest H1 --prereg docs/prereg/H1-residual-momentum.md
                                                      run a committed pre-registration once; logs to docs/trials.jsonl
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


HYPOTHESES = {
    "H1": "autotrader.research.h1_resid_mom",
    "H2": "autotrader.research.h2_composite",
    "H3": "autotrader.research.h3_pead",
    "H4": "autotrader.research.h4_trend",
}


def cmd_pretest(a: argparse.Namespace) -> int:
    import importlib

    from autotrader.research import pretest
    from autotrader.research.panel import build_panel, root

    prereg = Path(a.prereg)
    if not prereg.name.upper().startswith(a.hypothesis.upper() + "-"):
        raise SystemExit(f"{prereg.name} is not a pre-registration for {a.hypothesis}")
    commit = pretest.prereg_commit(prereg)
    end = seal.data_end(a.unseal, a.reason, hypothesis=a.hypothesis)
    panel = build_panel(end=seal.SEAL_DATE if end == seal.SEAL_DATE else "2099-12-31")
    ledger = Ledger()
    before = ledger.count()
    module = importlib.import_module(HYPOTHESES[a.hypothesis])
    result, markdown, n_configs = module.run(panel)
    window = f"{panel.dates[0]}..{panel.dates[-1]}"
    header = (f"# Pre-test {a.hypothesis}\n\n* Pre-registration: `{prereg.as_posix()}` at commit `{commit[:12]}`\n"
              f"* Data: clean panel {window} ({'UNSEALED' if a.unseal else 'sealed at ' + seal.SEAL_DATE})\n"
              f"* Configurations in this run: {n_configs}; ledger before this run: {before}; after: {before + n_configs}\n\n")
    result.update({"prereg": prereg.as_posix(), "prereg_commit": commit, "data_window": window, "n_configs": n_configs,
                   "ledger_before": before})
    path = pretest.write_outputs(a.hypothesis, result, header + markdown, root() / "var" / "research")
    ledger.append(kind="holdout" if a.unseal else "pretest", hypothesis=a.hypothesis, n_configs=n_configs, data_window=window,
                  spec_hash=f"{prereg.as_posix()}@{commit[:12]}", notes=f"report {path.relative_to(root()).as_posix()}")
    print(header + markdown)
    print(f"\nwritten: {path}")
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
    t = sub.add_parser("pretest")
    t.add_argument("hypothesis", choices=sorted(HYPOTHESES))
    t.add_argument("--prereg", required=True)
    t.add_argument("--unseal", action="store_true")
    t.add_argument("--reason", default=None)
    t.set_defaults(fn=cmd_pretest)
    c = sub.add_parser("check-trials")
    c.add_argument("--config", required=True)
    c.set_defaults(fn=cmd_check_trials)
    a = ap.parse_args()
    raise SystemExit(a.fn(a))


if __name__ == "__main__":
    main()
