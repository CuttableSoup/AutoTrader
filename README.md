# AutoTrader — autonomous swing-trading bot

Long-only, fully autonomous swing bot on liquid US equities. Earnings-momentum
hybrid (v1), cross-sectional momentum sleeve (v2). Broker: Alpaca, paper first.
Claude is a **veto** on candidates, never a signal source.

The design is in [docs/DESIGN.md](docs/DESIGN.md) (plan v0.1). Deviations and
open decisions are tracked in [docs/DECISIONS.md](docs/DECISIONS.md). Operations
are in [docs/RUNBOOK.md](docs/RUNBOOK.md); test gates and fault injection in
[docs/TESTING.md](docs/TESTING.md).

## Layout

| Path | What |
|---|---|
| `schemas/` | Versioned JSON Schema for every bus topic + `topics.json` stream registry |
| `cpp/` | C++20 services: ingestor, strategy, risk, execution, portfolio, reconciler, backtester (CMake + vcpkg) |
| `python/autotrader/` | Python sidecars: logger, earnings/event feed (FMP, Finnhub), universe builder (Sharadar), Claude validator, mock validator, G4 evaluation, fake Alpaca for fault injection |
| `watchdog/` | Standalone kill switch. **No shared code** with the rest of the system. Own Alpaca key. |
| `config/` | Frozen strategy parameters, risk limits, NATS server config, example runtime config |
| `scripts/` | Bootstrap (nats-server, JetStream streams), run/stop, drills |
| `data/` | Sample and synthetic data for smoke tests (vendor data is gitignored) |

## Quick start (paper, Windows)

```powershell
# 1. Python
uv venv .venv --python 3.13
uv pip install --python .venv\Scripts\python.exe -e python -e watchdog cmake ninja

# 2. NATS JetStream
python scripts\get_nats_server.py            # downloads tools\nats\nats-server.exe
tools\nats\nats-server.exe -c config\nats\nats-server.conf   # in its own terminal
python scripts\bootstrap_streams.py          # creates streams from schemas\topics.json

# 3. C++ (needs VS 2022 + vcpkg; VCPKG_ROOT must be set)
cmake --preset windows-release -S cpp
cmake --build --preset windows-release
ctest --preset windows-release

# 4. Backtest on synthetic data (Gate G1 machinery)
python scripts\gen_synthetic_data.py --out data\synthetic --years 6
cpp\build\windows-release\bin\at_backtester.exe --config config\backtest.synthetic.json

# 5. Paper trading
copy config\paper.example.json config\paper.json   # edit, keys come from the secrets backend
scripts\run_paper.ps1
```

Linux/macOS: the same CMake presets exist as `linux-release`; all services are
POSIX-clean (libcurl + ixwebsocket + cnats via vcpkg).

## Principles that the code enforces

* Broker is the source of truth. Nothing trades until `broker.reconcile` reports `CLEAN` after startup.
* Every order carries a deterministic `client_order_id`; every consumer dedupes on `msg_id`.
* Stops rest at the broker (OTO/bracket leg). Process death does not remove protection.
* Anything from the validator that is not `APPROVE` is a reject. Three consecutive `ERROR`s pause new entries and page.
* Money is `int64` cents everywhere. Time is UTC on the wire, America/New_York for market logic.
* Watchdog is untested = nonexistent: `scripts/drill_watchdog.py` runs weekly in paper.
