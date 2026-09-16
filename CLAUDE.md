# AutoTrader — notes for Claude Code sessions

Design: docs/DESIGN.md (plan v0.1, the source of truth). Deviations: docs/DECISIONS.md. Ops: docs/RUNBOOK.md. Gates: docs/TESTING.md.

## Build and test

```
# C++ (cygwin GCC on this box; see memory dev-toolchain-cygwin)
/c/cygwin64/bin/bash.exe -c 'export PATH=/usr/local/bin:/usr/bin:/bin:$PATH; cd /cygdrive/c/Users/Administrator/Projects/AutoTrader/cpp && cmake --build build/cyg-full -j8 && ./build/cyg-full/bin/at_tests.exe'
# core-only tree (no NATS/network): build/cyg
# Python
.venv/Scripts/python -m pytest python/tests watchdog/tests -q
# Backtest on synthetic data
cpp/build/cyg-full/bin/at_backtester.exe --config config/backtest.synthetic.json
```

Portable presets (`cmake --preset fetchcontent-release -S cpp`, `windows-release`, `linux-release`) are in cpp/CMakePresets.json.

## Conventions that must hold

* Money is `int64` cents everywhere (`at::Cents`); wire fields end in `_cents`; percentages end in `_pct`.
* Every bus message is an envelope validated against `schemas/` (both C++ `SchemaRegistry` and Python `SchemaRegistry`). New subject = add to `schemas/topics.json` + a payload schema + tests in both languages.
* `client_order_id` is deterministic (`common/ids.hpp`); consumers dedupe on `msg_id`.
* Strategy/risk logic is pure and shared by the backtester and the live services. Do not fork the code path.
* The watchdog (`watchdog/`) imports nothing from `python/autotrader`. Keep it stdlib-only.
* Only `signal.tunable.*` in `config/strategy.v1.json` may be tuned; everything else is frozen per the plan.
* The validator is a veto; it never places orders and its verdicts are ignored in SHADOW mode.
* Writing CSV from Python on Windows produces CRLF; the C++ loaders strip `\r`.

## Where things are

| Concern | Path |
|---|---|
| Signal, exits, sizing, universe, ledger | cpp/src/strategy/ |
| Risk limits + manager | cpp/src/risk/ |
| Backtester, walk-forward, DSR, mock validator | cpp/src/backtester/ |
| Alpaca REST/WS, execution, portfolio, reconciler, ingestor | cpp/src/alpaca, execution, portfolio, reconciler, ingestor |
| Service mains | cpp/src/services/ |
| Claude sidecar | python/autotrader/validator/ |
| Event feed (FMP/Finnhub) | python/autotrader/events_feed/ |
| Universe builder (Sharadar) | python/autotrader/universe/ |
| G4 evaluation | python/autotrader/evaluation/ |
| Fake Alpaca + fault injection | python/autotrader/fake_alpaca/, scripts/fault_injection.py |
| Kill switch | watchdog/ |
