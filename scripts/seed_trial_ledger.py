#!/usr/bin/env python
"""One-off: seed docs/trials.jsonl with every trial run against the Sharadar data before v2.

Counts come from
  * the Dow-30 free-sample walk-forward (var/backtest/sharadar-dow30): 24 configurations.
and from the committed reports and var/backtest/*/walk_forward.json:
  * v1.0, v1.1, v1.2 walk-forwards: 7 folds x 12 grid points = 84 configurations each,
    with the per-period Sharpe variance across those configurations as the runs reported.
  * the raw 40-session forward-return check on v1.0's 519 candidates (docs/G1-RESULT.md): 1.
  * scripts/event_study.py (commit 3c0a9fd): 26 bucket rows x 6 horizons + 4 period cells = 160.
    Each cell is a look at the data, so each is counted, though most were not "strategies".
Refuses to run twice.
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from autotrader.research.ledger import Ledger  # noqa: E402

WINDOW = "2016-09-16..2026-09-15"


def main() -> None:
    led = Ledger()
    if led.read():
        raise SystemExit(f"{led.path} already has entries; not seeding again")
    led.append(kind="walkforward", hypothesis="v1.0 on the Dow-30 sample", n_configs=24, data_window="free-tier sample",
               spec_hash="config/strategy.v1.json", notes="var/backtest/sharadar-dow30: 24 configurations, no trades")
    led.append(kind="walkforward", hypothesis="v1.0 EARNINGS_MOMENTUM", n_configs=84, data_window=WINDOW,
               var_sr_trials=0.001645910781956289, spec_hash="config/strategy.v1.json@6c638fc",
               notes="Gate G1 FAIL: DSR -1.77, 2/7 folds positive (docs/G1-RESULT.md)")
    led.append(kind="sweep", hypothesis="v1.0 raw forward return", n_configs=1, data_window=WINDOW,
               notes="519 candidates, 40-session excess over SPY -0.80% t=-1.19 (docs/G1-RESULT.md)")
    led.append(kind="sweep", hypothesis="earnings event study", n_configs=160, data_window=WINDOW,
               spec_hash="scripts/event_study.py@3c0a9fd",
               notes="26 bucket rows x 6 horizons + 4 period cells; found drift only below $5B, on the "
                     "ever-above-floor symbol set (selection-biased below the floor)")
    led.append(kind="walkforward", hypothesis="v1.1 floor $1B", n_configs=84, data_window=WINDOW,
               var_sr_trials=0.0013589909442571373, spec_hash="config/strategy.v1.1.json@3c0a9fd",
               notes="FAIL: haircut Sharpe 0.17, 3/7 folds positive (docs/g1-report-v1.1.md)")
    led.append(kind="walkforward", hypothesis="v1.2 5xATR stop", n_configs=84, data_window=WINDOW,
               var_sr_trials=0.0011076989681929952, spec_hash="config/strategy.v1.2.json@3c0a9fd",
               notes="FAIL: haircut Sharpe 0.15, 3/7 folds positive (docs/g1-report-v1.2.md)")
    print(f"seeded {led.path}: {led.count()} configurations")


if __name__ == "__main__":
    main()
