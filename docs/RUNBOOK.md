# Runbook

Operations for the paper deployment. Everything here assumes the layout in the
README and the design in [DESIGN.md](DESIGN.md). Live differs only in keys,
base URLs and the 10% capital start (Gate G5).

## Processes

| Process | Language | Binary / entry point | Reads | Writes |
|---|---|---|---|---|
| nats-server | Go (binary) | `tools/nats/nats-server -c config/nats/nats-server.conf` | | `var/jetstream/` |
| logger | Python | `at-logger` | every stream | `var/log/topics/<subject>/<day>.jsonl(.gz)` |
| pager | Python | `at-pager` | `control.>`, `broker.reconcile` | log / webhook |
| ingestor | C++ | `at_ingestor_svc` | Alpaca data, `signals.candidate`, `portfolio.state`, `var/state/universe.json` | `market.data.bar.*` (split-adjusted), `market.data.bar_tr.*` (dividend-adjusted, the fixed 18-ETF TSMOM universe -- SPY needs both: it is also the earnings strategy's trend_symbol), `market.data.shortable.*` (TSMOM universe, once/day), `market.data.quote.*` |
| events feed | Python | `at-events-feed` | FMP, Finnhub | `events.earnings` |
| strategy | C++ | `at_strategy_svc` | `market.data.bar.*`, `events.earnings`, `var/state/universe.json` | `signals.candidate` (16:20 ET) |
| tsmom | C++ | `at_tsmom_svc` | `market.data.bar.*` (fixed 18-ETF universe, `strategy/tsmom_universe.cpp`) | `signals.candidate` (16:25 ET, month-end formation sessions only) |
| validator | Python | `at-validator` | `signals.candidate` | `signals.validated`, `control.pause_new` |
| risk | C++ | `at_risk_svc` | candidates, verdicts, `portfolio.state`, `broker.reconcile`, quotes, `control.>` | `orders.approved` (09:31 / 16:10 ET), `control.>` |
| execution | C++ | `at_execution_svc` | `orders.approved`, `control.>`, Alpaca WS | `orders.submitted`, `orders.status`, `orders.filled` |
| portfolio | C++ | `at_portfolio_svc` | fills, bars, events, reconcile | `portfolio.state`, `watchdog.heartbeat`, HTTP heartbeat |
| reconciler | C++ | `at_reconciler_svc` | Alpaca, `portfolio.state` | `broker.reconcile`, `control.pause_new`, watchdog `/alert` |
| watchdog | Python (stdlib) | `at-watchdog config/watchdog.json` | HTTP heartbeats, Alpaca (own key) | Alpaca cancel/close, `control.halt` |

Start order: nats-server, `scripts/bootstrap_streams.py`, logger, pager, watchdog, portfolio, reconciler, ingestor, events feed, strategy, tsmom, validator, risk, execution. `scripts/run_paper.ps1` / `.sh` do this. `tsmom` has no ordering dependency relative to `strategy` (both are candidate producers feeding `risk`), but both must start before `risk`.

**Nothing trades until `broker.reconcile` reports `CLEAN`.** After any restart, watch for that message before expecting orders.

## Daily timeline (America/New_York)

| Time | What |
|---|---|
| 09:20 | ingestor pulls bars (incremental) |
| 09:30–09:45 | ingestor publishes quotes every 10 s for pending candidates |
| 09:31 | risk processes pending entries -- both earnings candidates and any pending TSMOM rebalance candidates from the prior close (retry at 09:36 for transient quote failures) |
| 15:45 | (portfolio) nothing; stop ratchets are decided at the close sweep |
| 16:05 | portfolio end-of-session: marks, sessions held, HWMs |
| 16:10 | risk exit sweep: time/earnings exits, trend scale-down, stop ratchets -> execution |
| 16:20 | strategy evaluates the session -> candidates -> validator overnight |
| 16:25 (month-end formation sessions only) | tsmom evaluates the monthly rebalance -> candidates straight to risk (no validator -- TSMOM bypasses the Claude sidecar entirely) |
| 16:30 | ingestor pulls closing bars |
| every 15 min | reconciler |
| every 60 s | portfolio.state |
| every 10 s | heartbeat |

## Pages and what to do

| Page | Meaning | Action |
|---|---|---|
| `control.pause_new` VALIDATOR_ERROR_STREAK | Claude API failing or timing out 3× in a row | Check `var/log/validator.log`, API status, budget. Fix, then `control.resume`. |
| `control.pause_new` RECONCILE_MISMATCH | Broker and ledger disagree, an unprotected position, or an orphan order | Read the `diffs` in `broker.reconcile`. Fix at the broker or in `var/state/portfolio_state.json`, restart portfolio, wait for CLEAN, then `control.resume`. |
| `control.pause_new` DAILY_LOSS | −2% on the day | Nothing; auto-resumes next session. |
| `control.pause_new` CONSECUTIVE_LOSERS | 8 losers in a row | Review trades. `control.resume` when satisfied. |
| `control.pause_new` REJECT_RATE | > 10% broker rejects today | Check execution log for the reject reason (buying power, symbol not tradable, wash-trade block). |
| `control.flatten` + `control.halt` DRAWDOWN_FLATTEN | −12% from HWM | Everything is closed. Post-mortem before any restart. Clear `halted` in `var/state/*_state.json` deliberately. |
| `control.halt` HEARTBEAT_LOSS (watchdog) | Main system silent 60 s | Watchdog already cancelled + closed everything. Find why the portfolio service died. `POST /resume` on the watchdog after the fix. |

A plain `control.resume` never clears a halt. After the post-mortem, an operator clears it explicitly with
`{"command": "resume", "source": "operator", "details": {"clear_halt": true}, ...}`; the risk manager and the
execution engine both require `source=operator` and that flag. The watchdog is reset separately with `POST /resume`.

## Sending control commands

```
python - <<'EOF'
import asyncio, sys; sys.path.insert(0, "python")
from autotrader.bus import Bus; from autotrader.schemas import SchemaRegistry; from autotrader.envelope import Envelope, now_utc_iso
async def main():
    b = Bus("nats://127.0.0.1:4222", "operator", SchemaRegistry()); await b.connect()
    await b.publish("control.resume", Envelope("operator", {"command": "resume", "reason": "reviewed", "source": "operator", "issued_at_utc": now_utc_iso()}))
    await b.close()
asyncio.run(main())
EOF
```

Replace `resume` with `pause_new`, `flatten` or `halt` as needed. The watchdog takes `POST /halt`, `/resume`, `/drill` with the bearer token.

## Weekly

* `python scripts/drill_watchdog.py --token <token>`: runs the real kill path against the paper account and records the result in `var/drills.jsonl`. A failed drill is a Gate G2 failure.
* `at-evaluate-claude`: refreshes the G4 counterfactual from the archive.
* Check `var/log/topics/` disk usage; the logger gzips rotated days, nothing deletes them (2-year retention).

## Monthly

* `at-universe live --securities <csv> --bars <csv>` rebuilds `var/state/universe.json`. Strategy and ingestor pick it up automatically.

## State files (var/state)

| File | Owner | Loss consequence |
|---|---|---|
| `portfolio_state.json` | portfolio | Positions re-synced from broker; strategy metadata (stops, deadlines, entry dates) lost -> reconciler flags `unprotected_position` if stops are missing, exits fall back to "exit at next sweep". |
| `risk_state.json` | risk | Pending candidates and day counters lost; pause/halt flags lost (dangerous: always check after a restore). |
| `execution_state.json` | execution | client_order_id -> intent map lost; fills show `intent=UNKNOWN`. The broker still dedupes on client_order_id. |
| `universe.json` | universe builder | No candidates until rebuilt. |
| `validator_state.json` | validator | Error streak and daily spend reset. |
| `watchdog_state.json` | watchdog (other host) | HWM reset to current equity; a latched kill is forgotten. |

## Secrets

`config/secrets.json` is for dev boxes only. Production: `secrets.backend = "aws-secretsmanager"` with the secret name in `secrets.aws_secret_name`; the watchdog host has a separate secret with its own Alpaca key. Keys are scoped paper vs live and never shared between the trading system and the watchdog.
