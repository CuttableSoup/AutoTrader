# Autonomous Swing-Trading Bot — Design Plan v0.1

2026-09-15. Merges the architecture review and the strategy-family research into one buildable plan. Engineering guidance, not financial advice.

## 1. Objective and scope

* Fully autonomous, long-only swing bot on liquid US equities. No human adjustment after go-live.
* Holds: 5–40 trading days. No day trading by design; occasional same-day exits are fine (FINRA retired the PDT rule 2026-06-04).
* No options, no shorting in v1 (Alpaca locate/HTB fragility undermines autonomy).
* Broker: Alpaca. Paper first, extended.
* Strategy: earnings-momentum hybrid (v1), cross-sectional momentum sleeve (v2). Claude is a veto on candidates, never a signal source.

## 2. Strategy

### 2.1 Universe (rebuilt monthly, point-in-time)

* US common stock, market cap > $5B, 20-day ADV > $50M, median spread < 5 bps.
* Analyst coverage ≥ 5 and transcript available (so Claude has real material).
* Exclude: ETFs, ADRs, IPOs < 12 months, the ~20 most heavily-optioned mega-caps (Milian overreaction regime).
* Membership from Sharadar point-in-time data in backtests; live from the same rules on current data.

### 2.2 v1.0 signal — earnings-momentum, long-only

Evaluate on each earnings event in the universe:

* EAR: 2-day abnormal announcement return ≥ +3% (or top tercile of that week's announcers).
* Volume confirmation: announcement-day volume ≥ 2× 20-day ADV.
* Momentum confirmation: 12-1 return (skip most recent month) in top 40% of universe.
* Trend filter: SPY > 200-day SMA. Otherwise no new longs.

v1.1 (after point-in-time estimate history is verified): add analyst-revision breadth > +0.3 over trailing 4 weeks as a fifth condition.

Entry: next open after signal (or same close if pre-market announcement), within 2 trading days. Optional pullback timing: 5-day RSI < 50.

Exit, whichever first:

* 40 trading days elapsed
* next scheduled earnings date (exit the day before)
* 3×ATR(20) trailing stop, resting at broker as the bracket stop leg
* SPY < 200-day SMA → no adds; scale down existing by 50%

Sizing: equal-weight target, inverse-vol tilt, book vol-targeted to 10–12% annualized. Single-name cap 8%. Sector cap 30%. Max 15 positions. Gap budget: size so a 2×ATR adverse gap ≤ 0.5% of equity.

Parameters (7, few tunable): EAR threshold, volume multiple, momentum percentile, drift window, ATR multiple, vol target, trend MA. Walk-forward tune only EAR threshold and momentum percentile. Everything else fixed.

### 2.3 v2 — momentum sleeve (add only after v1 gates pass)

* Rank by residual 12-1 return; hold top quintile; monthly rebalance; same trend filter; vol-target 12%.
* Run as a separate book. Combine only if combined Sharpe > either sleeve alone in walk-forward.
* Claude role here is narrower: tail-risk flags only (fraud allegation, going-concern, imminent binary event). Never valuation.

### 2.4 Skipped

* Standalone short-term reversal: cost-dominated in liquid names, needs weekly+ turnover, nothing for Claude to check.
* Standalone SUE-PEAD: non-existent in large caps since ~2006 (Martineau 2022).
* Long-short constructions in v1.

## 3. Architecture

### 3.1 Services and topics

```
market data ingestor  ──▶ market.data
earnings/event feed   ──▶ events.earnings
strategy engine       ──▶ signals.candidate
claude sidecar        ──▶ signals.validated
risk manager          ──▶ orders.approved
execution engine      ──▶ orders.submitted, orders.status, orders.filled
portfolio service     ──▶ portfolio.state      (on every fill + 60s timer)
reconciler            ──▶ broker.reconcile     (startup + every 15 min)
watchdog/kill switch  ──▶ control.*            (separate process, own creds)
logger                ──▶ subscribes to everything
```

Broker is the source of truth. No service may act on in-memory position state until broker.reconcile reports clean after startup.

### 3.2 Message contracts (JSON Schema, versioned)

Every message: `{schema_version, msg_id (uuid), ts_utc, producer, payload}`.

* signals.candidate: symbol, side, signal_type, event_id, ear_pct, vol_ratio, mom_pct, entry_px_ref, atr20, thesis_facts[] (neutral facts only — never a strength adjective, to avoid sycophancy in the validator).
* signals.validated: candidate_msg_id, verdict ∈ {APPROVE, REJECT, ERROR}, confidence 0–1, reasons[], flags[], model, latency_ms, cost_usd, citations[]. Risk manager treats anything not APPROVE as reject.
* orders.approved: candidate_msg_id, symbol, qty, order_type=bracket, limit_px, stop_px, tif=GTC, client_order_id (deterministic: hash of candidate_msg_id).
* orders.status: full lifecycle from Alpaca trade-updates WS: new, partial_fill, filled, canceled, rejected, replaced, expired.
* portfolio.state: equity, cash, buying_power, positions[{symbol, qty, avg_px, unrealized, entry_ts, exit_deadline}], gross_exposure, sector_exposure{}, hwm_equity, drawdown_pct.
* control.*: control.halt, control.flatten, control.pause_new, control.resume.

### 3.3 Correctness rules

* Idempotency: client_order_id on every order; retries dedupe at the broker. Bus is at-least-once; consumers dedupe on msg_id.
* Bracket orders on entry so stop and target rest at Alpaca, independent of process liveness. TIF = GTC. No extended hours.
* Reconciliation before trading resumes after any restart. Mismatch → control.pause_new + page.
* Staleness: reject market data older than 30s; reject candidates built on data older than one session.
* Time: NTP; all market-hours logic in America/New_York; explicit holiday/half-day calendar.
* Corporate actions: adjust ATR/price references on splits; skip entries within 2 days of ex-dividend; honor halts.

## 4. Claude validation sidecar

* Runtime: Python (Anthropic SDK + nats-py). Not C++.
* Role: veto only. Question posed: "Is there material public information that contradicts the fundamental quality of this earnings reaction, or a binary event inside the next 40 trading days?"
* Model: Haiku 4.5 default. Escalate to Sonnet only if shadow-mode results justify it.
* Two-step call (structured outputs and citations cannot coexist in one request):
  1. Web search + reasoning, citations on, max_uses = 3, domain allowlist (filings, major wires, company IR).
  2. Strict-JSON verdict via output_config.format, no tools.
* Prompt caching on the static system prompt. Keep the dynamic section byte-stable (no timestamps in cached region).
* Injection defenses: web content only inside tool_result blocks; system prompt declares search content untrusted; JSON-encode candidate facts; optional Haiku pre-screen for injection_suspected. The sidecar cannot place orders.
* Timeout: 20s. On timeout/error → verdict=ERROR → reject. After 3 consecutive errors → control.pause_new + page. No silent zero-trade state.
* Shadow mode (mandatory first): validator labels every candidate; risk manager ignores the label; paper executes both approved and vetoed sets. Promote to live veto only per §7.3.
* Budget: ~20 candidates/day × 2 searches ≈ $0.40/day search + tokens → low tens of $/month.

## 5. Risk manager — hard limits

All limits enforced before orders.approved; breaches also feed the kill switch.

| Limit | Value |
|---|---|
| Per-position risk | 0.5% of equity at stop |
| Single-name cap | 8% of equity |
| Sector cap | 30% |
| Gross exposure | ≤ 100% (no margin in v1) |
| Max open positions | 15 |
| Max new positions/day | 3 |
| Max orders/day | 30 (runaway guard) |
| Daily loss | −2% → pause new |
| Drawdown from HWM | −8% → halve size; −12% → flatten + halt |
| Consecutive losers | 8 → pause new + page |
| Spread guard | reject if spread > 10 bps |
| Size guard | reject if order > 1% of 20-day ADV |
| Earnings gate | never hold through the next scheduled earnings |
| Stale data | reject if last quote > 30s old |

## 6. Kill switch / watchdog

* Separate process, separate host or region, own Alpaca API key.
* Heartbeat from main system every 10s. Miss 6 → cancel-all (DELETE /v2/orders) + close-all + control.halt.
* Triggers: heartbeat loss, drawdown breach, reconciliation mismatch, manual command, validator error streak.
* Talks to Alpaca REST directly. Never touches the bus for its own action path.
* Tested on a weekly schedule in paper. Untested = nonexistent.

## 7. Testing and validation

### 7.1 Backtest

* C++ event-driven backtester replaying market.data and events.earnings through the identical strategy code path.
* Data: Sharadar point-in-time fundamentals + constituent history (survivorship-free). Prices: Alpaca or Polygon daily bars.
* Cost model: 5 bps/side + SEC/FINRA fees. Fill at next open with half-spread slippage.
* Mock validator (deterministic stand-in for Claude): reject if any of
  * revenue surprise ≤ 0 while |EAR| large (one-off proxy)
  * second material 8-K inside the drift window
  * counter-gap > 1.5×ATR within 3 days after entry
  * symbol in excluded over-optioned set
* Walk-forward with purge/embargo; tune only the two designated parameters.
* Report Deflated Sharpe Ratio on every run, penalized for total configurations tried. Haircut backtest returns 50% before reasoning about live.

### 7.2 Gates

| Gate | Pass condition |
|---|---|
| G1 backtest | DSR > 0 and net return positive in ≥ 2 of 3 walk-forward folds; gross Sharpe ≥ 0.4 after 50% haircut |
| G2 infra | kill switch, reconciliation, idempotent retries all pass fault-injection tests in paper |
| G3 paper (strategy) | ≥ 150 closed trades; net return positive; realized drawdown within limits |
| G4 Claude | see 7.3 |
| G5 live | G1–G4 passed; start at 10% of intended capital for 3 months |

### 7.3 Evaluating the Claude layer

* Paired counterfactual: compare 40-day forward returns of vetoed vs approved candidates from shadow mode.
* Promote to live veto when approved-minus-vetoed forward return is positive with t > 1.5 over ≥ 150 labeled candidates (≈ 12–18 months).
* If vetoes show negative selectivity (Claude vetoes winners), demote to tail-risk-only flags.
* If no measurable lift after an adequate sample, remove the layer. It is cost and a failure surface for no edge.

### 7.4 Sample-size reality

At ~80–120 trades/year, Sharpe standard error ≈ 0.1 after one year. Twelve months of paper cannot distinguish Sharpe 0.9 from 0.7. Plan 12–18 months paper minimum; pool by earnings events, not months.

## 8. Data stack

| Need | Source | Cost |
|---|---|---|
| Execution, trade updates WS, Benzinga news | Alpaca | free (SIP feed optional, $99/mo) |
| Earnings calendar with BMO/AMC, actual vs consensus, transcripts | Financial Modeling Prep Premium (Ultimate for transcripts) | $49 / $99 per month |
| Point-in-time fundamentals, constituents, insider data (backtest) | Sharadar via Nasdaq Data Link | retail bundle |
| Cross-check for announcement dates, FDA/insider events | Finnhub free tier | free |

Rules: consensus must be as-of the day before the announcement (no look-ahead); use as-reported EPS, not restated; cross-check BMO/AMC across two vendors because it determines day 0.

## 9. Tech stack

* Bus: NATS JetStream, single binary. Streams with max_age and max_msgs set from day one. Durable consumers, explicit ack.
* C++ services (ingestor, strategy, risk, execution, portfolio, reconciler, backtester): C++20, CMake + vcpkg, nats.c, libcurl or cpp-httplib, Boost.Beast or ixwebsocket, nlohmann/json, spdlog, Catch2. Fixed-point integer cents for all money. Hinnant date/tz for calendars.
* Alpaca client: hand-rolled REST + trade-updates WebSocket (community C++ SDKs are stale). Rate limit 200 req/min trading; WS is the authoritative order-state feed.
* Python sidecars: Claude validator; earnings/event feed adapter (FMP, Finnhub). Both speak JetStream.
* Watchdog: small standalone (Go or Python), no shared code with strategy.

## 10. Operations

* Host: US-East VPS. Watchdog in a different provider or region.
* Secrets: manager, not env files. Watchdog has its own key. Keys scoped paper vs live.
* Page on: validator error streak, heartbeat loss, reconciliation mismatch, daily-loss or drawdown breach, data staleness, order-reject rate > 10%.
* Retain all topic logs ≥ 2 years for post-mortems and the Claude counterfactual.
* Track tax lots independently.
* Do not use deprecated Alpaca PDT fields (pattern_day_trader, daytrade_count, daytrading_buying_power); rely on intraday buying power fields.

## 11. Build order

1. JetStream + schemas + logger. All topics from §3.1 defined and versioned.
2. Alpaca client + orders.status + portfolio.state + reconciler. Paper-verified.
3. Watchdog/kill switch + risk manager. Fault-injection tests: kill main process mid-order, corrupt state, force reconcile mismatch.
4. Universe builder + data adapters (Sharadar, FMP, Finnhub).
5. C++ backtester replaying market.data + events.earnings. v1.0 strategy. Walk-forward + DSR. → Gate G1.
6. Mock validator wired into backtest and paper.
7. Claude sidecar in shadow mode. → Gate G4 accrual begins.
8. Paper trading 12–18 months. → Gates G3, G4.
9. v2 momentum sleeve, only after G1 passes for v1.
10. Live at 10% size. → Gate G5.

## 12. Open decisions (iterate on these next)

* EAR window definition: close-to-close over day −1 to +1 vs open-to-close on day 0. Pick one and freeze.
* Universe cap: $5B vs $10B floor. Higher floor = fewer Milian reversals, fewer candidates.
* Add revision breadth in v1.1 or defer to v2.
* Sharadar tier and whether Polygon is needed at all for daily bars.
* Watchdog language and host.
* Whether Claude gets the transcript text (Ultimate tier) or search only.

## 13. Standing caveats

* Every family's live edge will be smaller than backtests (post-publication decay ≈ 58%, McLean-Pontiff).
* Vol-managed momentum's benefit is contested out of sample.
* Text-based earnings surprise evidence is recent and may decay the same way SUE-PEAD did.
* Alpaca paper fills are optimistic; live results will be worse.
* Vendor pricing and Anthropic API details drift; re-verify at signup and before coding against them.
