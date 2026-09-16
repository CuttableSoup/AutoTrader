# Testing and gates

Maps docs/DESIGN.md section 7 to concrete commands. Every gate has a runnable check.

## Unit and component tests

```
# C++ (54 test cases; strategy, risk, sizing, exits, calendar, money, schemas, backtester, DSR)
cmake --preset fetchcontent-release -S cpp && cmake --build --preset fetchcontent-release && ctest --preset fetchcontent-release
# Python sidecars + watchdog (17 tests)
.venv/Scripts/python -m pytest python/tests watchdog/tests -q
```

The C++ suite includes an end-to-end backtest on a synthetic universe with planted earnings reactions and asserts the invariants that matter: no position without a stop, ≤ 15 positions, gross exposure ≤ equity, holding ≤ 41 sessions, costs on every trade, net = gross − costs.

## Gate G1: backtest

Data layout for `at_backtester` (`data_dir`):

| File | Columns |
|---|---|
| `bars.csv` | `symbol,date,open,high,low,close,volume` (split-adjusted, SPY included) |
| `securities.csv` | `symbol,name,sector,category,market_cap,first_listed,analyst_coverage,transcript_available,median_spread_bps,as_of` (one row per point-in-time snapshot) |
| `earnings.csv` | `event_id,symbol,report_date,timing,fiscal_period,eps_actual,eps_consensus,eps_consensus_asof,revenue_actual,revenue_consensus,next_report_date,material_8k_dates` |

`at-universe backtest` produces this layout from Sharadar bulk downloads; `scripts/gen_synthetic_data.py` produces an edge-free synthetic version to exercise the machinery.

```
python scripts/gen_synthetic_data.py --out data/synthetic --years 6
cpp/build/.../at_backtester --config config/backtest.synthetic.json        # walk-forward + DSR + G1 verdict, exit code 2 on FAIL
cpp/build/.../at_backtester --config config/backtest.synthetic.json --single
```

Outputs in `out_dir`: `walk_forward.json`, `oos_trades.csv`, `report.md`. The walk-forward tunes only `ear_threshold_pct` and `momentum_top_pct` (grid in `config/strategy.v1.json`), purges 40 sessions before each test window and embargoes 10 after, and reports the Deflated Sharpe penalised for every configuration tried (folds × grid). G1 passes when deflated Sharpe > 0, net return is positive in ≥ 2 of 3 folds, gross Sharpe after the 50% haircut ≥ 0.4, and there are ≥ 30 out-of-sample trades.

On the synthetic data G1 **fails by construction** (random reactions); that is the expected result and a sanity check on the gate itself.

The mock validator's COUNTER_GAP and SECOND_8K rules use post-signal data and are labelled `lookahead` in every verdict; the counterfactual they produce is an upper bound on what a real validator could add.

## Gate G2: infrastructure fault injection

Runs against `at-fake-alpaca` (Python) so no broker is involved. `scripts/fault_injection.py` drives the scenarios and checks the bus:

| Scenario | Injection | Expected |
|---|---|---|
| Lost response after accept | `drop_next_response` on POST /v2/orders | execution retries with the same client_order_id, gets 422 duplicate, fetches the existing order, publishes exactly one `orders.submitted` with `deduped_at_broker=true`; one fill on the bus |
| Broker 500 | `http_500_next` | retry with backoff, then success; no duplicate order |
| Broker reject | `reject_next_order` | `orders.status event=rejected`; reject-rate guard counts it |
| WebSocket drop | `ws_disconnect` | stream reconnects, re-authenticates, resumes; fills after reconnect still reach `orders.filled` |
| Kill main process mid-order | `kill -9` of at_execution_svc between submit and trade update | on restart the durable consumer redelivers `orders.approved`; execution finds the order by client_order_id (no second order); reconciler compares broker vs ledger |
| Corrupt state | delete `var/state/portfolio_state.json` while positions exist | portfolio re-syncs qty from broker; reconciler reports `unprotected_position` if the stop leg is unknown -> `control.pause_new` + page |
| Forced mismatch | `corrupt_position` (fake broker reports a GHOST position) | `broker.reconcile MISMATCH missing_internal` -> `control.pause_new`, watchdog `/alert`, pager fires; risk rejects new entries with reason `reconciled` |
| Heartbeat loss | stop at_portfolio_svc | after 60 s the watchdog cancels all + closes all through its own key and publishes `control.halt`; `scripts/drill_watchdog.py` exercises the same path weekly |
| Drawdown | `POST /__price` to push equity −12% | watchdog flatten from its own HWM; risk manager independently emits `control.flatten` + `control.halt` |

Pass condition: every scenario ends flat or paused with a page, never with an unprotected position or a duplicate order.

How to run it (all against the fake, no broker keys):

```
tools/nats/nats-server -c config/nats/nats-server.conf          # terminal 1
python scripts/bootstrap_streams.py
at-fake-alpaca --port 8790                                       # terminal 2
export AT_ALPACA_KEY_ID=fake AT_ALPACA_SECRET_KEY=fake AT_WATCHDOG_TOKEN=fake-token
cp config/paper.fake.example.json config/paper.fake.json          # watchdog: config/watchdog.fake.json (see runbook)
at-watchdog config/watchdog.fake.json; at-logger --config config/paper.fake.json; at-validator --config config/paper.fake.json
at_portfolio_svc / at_reconciler_svc / at_ingestor_svc / at_strategy_svc / at_risk_svc / at_execution_svc --config config/paper.fake.json
python scripts/fault_injection.py --scenario all                  # exit 0 = all PASS
python scripts/drill_watchdog.py --token fake-token               # weekly drill against the same watchdog
```

Harness notes learned the hard way:

* The harness never resets the fake broker's state behind the system's back: the reconciler would (correctly) report `missing_broker` and trip the kill switch. It flattens through the normal `DELETE /v2/positions` API and waits for a `CLEAN` reconcile before the first scenario.
* The mismatch scenario legitimately latches halts in the execution engine, the risk manager and the watchdog. The harness clears them the way an operator would: `POST /resume` on the watchdog and a `control.resume` from `source=operator` with `details.clear_halt=true`.
* The `lost_response` scenario verified a race that exists with a real broker too: the WebSocket fill can arrive before the REST response. The execution engine registers an order's intent before submitting, and the portfolio adopts any buy fill as a position even with unknown intent.
* Verdicts can overtake candidates on the bus (two consumers, no cross-subject ordering). The risk manager buffers early verdicts and persists them.

Last full run on 2026-09-15 (fake broker, cygwin build): lost_response PASS, http500 PASS, reject PASS, ws_drop PASS, mismatch PASS, drill flat in 1 attempt.

## Gate G3: paper strategy

`≥ 150 closed trades`, positive net return, realised drawdown inside the limits. Trades are in `var/state/portfolio_state.json` (ledger.trades) and in the logger archive (`orders.filled`). Twelve to eighteen months (docs/DESIGN.md 7.4).

## Gate G4: Claude layer

`at-evaluate-claude` computes the paired counterfactual from the archive (shadow-mode verdicts vs 40-session forward returns) and prints one of PROMOTE / DEMOTE / REMOVE / insufficient_sample. The rule is mechanical: promote only when approved-minus-vetoed forward return is positive with t > 1.5 over ≥ 150 labelled candidates.

## Gate G5: live

Only after G1–G4. `env: live` config, live keys in the secrets manager, `initial capital = 10%` of intended for 3 months, watchdog on a different provider with a live-scoped key.
