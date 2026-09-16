# Decisions log

Resolutions of the §12 open decisions, plus every place the implementation
deviates from plan v0.1 and why. An entry is frozen once a backtest has been
run against it; changing a frozen entry bumps `strategy_version`.

## Resolved §12 decisions (v0.1 defaults, frozen in `config/strategy.v1.json`)

> **Data entitlements as of 2026-09-15.** The keys supplied do not yet cover what
> §8 assumes. See "Vendor entitlements" at the bottom of this file: Gate G1 is
> blocked on a Sharadar subscription, and the Claude layer is blocked on Anthropic
> credit. Nothing in the design changed because of this; only what can be run today.

| # | Decision | Choice | Rationale |
|---|---|---|---|
| 1 | EAR window | **Close-to-close, day −1 close → day +1 close, minus SPY over the same window.** Day 0 is the first session that can react: the report date if BMO, the next session if AMC or unknown. | Robust to a one-vendor BMO/AMC error; matches the EAR definition in the source research. Open-to-close on day 0 alone is too sensitive to day-0 assignment. |
| 2 | Universe floor | **$5B** market cap, with the over-optioned exclusion list as the Milian guard. | More candidates for sample-size reasons (§7.4). Revisit to $10B if walk-forward shows reversal in the $5–10B bucket. |
| 3 | Revision breadth | **Deferred to v1.1**, behind `signal.revision_breadth.enabled=false`. The code path exists; the condition is not evaluated until point-in-time estimate history is verified. | Per plan. |
| 4 | Sharadar tier / Polygon | **Sharadar SEP (daily prices) + SF1 (fundamentals) + TICKERS + EVENTS** for the backtest. **No Polygon.** Live daily bars from Alpaca. | Sharadar SEP is survivorship-free and already licensed; Alpaca bars are free. |
| 5 | Watchdog language/host | **Python 3.13 stdlib only** (no third-party deps, no shared code), packaged in `watchdog/`. Host: a different provider than the main VPS. | Stdlib-only means the watchdog cannot be broken by a dependency upgrade in the main system. |
| 6 | Transcript text for Claude | **Search only in shadow mode.** Transcript text (FMP Ultimate) sits behind `validator.include_transcript=false`. | Start with the cheapest configuration; the G4 counterfactual decides whether more material helps. |

## Deviations from plan v0.1

| Area | Plan says | Implementation | Why |
|---|---|---|---|
| Entry order type | `order_type=bracket` | `order_type ∈ {oto, bracket}`, default **oto** (entry + resting stop leg). Bracket (take-profit at `take_profit_atr_mult × ATR`) is available via config. | The strategy has no profit target; a bracket needs one. OTO keeps the stop resting at the broker. The trailing ratchet is done by the portfolio service replacing the stop leg upward once per session; if the process dies the last stop still rests. |
| Watchdog heartbeat transport | unspecified | HTTP POST from the portfolio service to the watchdog every 10 s (`/heartbeat`, bearer token). The watchdog also reads `watchdog.heartbeat` on the bus when reachable, but never depends on the bus. | "Never touches the bus for its own action path." |
| Calendars | Hinnant date/tz | `std::chrono` tzdb (C++20; same author, standardized). NYSE holiday rules coded explicitly in `calendar.cpp`. | No extra dependency. |
| Money on the wire | "fixed-point integer cents" | Every money field is an `int64` with the `_cents` suffix. Percentages are `double` with `_pct` suffix. Quantities are integers (no fractional shares in v1). | Self-describing schema. |
| Logger | unspecified language | Python, durable consumer on every stream, JSONL per subject per day, gzip after rotation. | Not latency sensitive. |
| Ingestor cadence | unspecified | Daily bars pulled after close and at 09:20 ET; NBBO quotes pulled on demand for the spread/staleness guards right before an order. No streaming feed in v1. | A daily-bar strategy does not need a tick feed. |
| Local dev toolchain | MSVC via vcpkg | vcpkg manifest is kept for MSVC and Linux. A `AT_DEPS=fetchcontent` CMake mode builds the same code with GCC/cygwin when vcpkg is unavailable (the dev box could not install the Windows SDK unattended). | Same sources, two dependency providers. |

## Vendor entitlements (measured 2026-09-15, re-verify at renewal)

Every vendor was called with the live keys. What §8 assumes vs what the keys return:

| Need (§8) | Vendor | Status |
|---|---|---|
| Execution, account, trade-updates WS | Alpaca paper | **OK.** Account ACTIVE, $10,000 equity. |
| Daily bars, NBBO quotes (live) | Alpaca data (IEX feed) | **OK.** |
| Forward earnings calendar | FMP `stable/earnings-calendar` + Finnhub | **OK** for roughly the next quarter. |
| BMO/AMC timing | Finnhub only | **Partial.** FMP's `stable` calendar carries no time field, so only Finnhub states timing. The feed now accepts one vendor's timing when the other is silent, and still falls back to UNKNOWN on disagreement; UNKNOWN means day 0 is the session after the report date. |
| Historical consensus (backtest) | FMP | **Blocked.** `from` earlier than ~30 days returns HTTP 402 on this plan. Not fatal: the v1.0 signal is price, volume, momentum and trend only. Consensus feeds the mock validator's revenue-surprise proxy and the thesis facts, both of which degrade quietly. |
| Transcripts | FMP | **Blocked.** Transcript endpoints return HTTP 402. The `require_transcript` universe rule cannot be enforced and is reported as not enforced rather than silently passing. |
| Analyst coverage | FMP `stable/analyst-estimates` | **OK** (`numAnalystsEps`, `numAnalystsRevenue`), current values only, so a small look-ahead on a slow-moving rule. |
| Point-in-time fundamentals, prices, constituents (backtest) | Sharadar | **Free tier: the ~30 Dow constituents only.** Everything else returns `403 Exceeds free tier`. |
| Quoted spreads | none | **Not available.** `median_spread_bps` is written as 0 and the rule is logged as not enforced. |
| Claude validator | Anthropic | **Blocked.** Key is workspace-scoped now, but the account has no credit balance (`400 credit balance is too low`). |

Consequences:

* **Gate G1 cannot be attempted** until Sharadar covers the real universe. Thirty mega-caps are not a cross-sectional universe: eleven of them are on the over-optioned exclusion list, the momentum percentile is computed over a handful of names, and there is no survivorship-free breadth. The pipeline itself is exercised on those thirty so the code path is proven.
* **Gate G4 accrual cannot start** until the Anthropic account has credit. The sidecar runs in MOCK mode meanwhile, which exercises the bus contract but produces no counterfactual signal.
* The two universe rules that cannot be sourced (transcript, spread) are **reported as unenforced on every build** rather than defaulted silently, so the universe size is never mistaken for a filtered one.

### Sharadar API migration

Sharadar moved off Nasdaq Data Link. The loader targets `https://api.sharadar.com/v1.0/data/{table}`:

| Old (Nasdaq Data Link) | New |
|---|---|
| `data.nasdaq.com/api/v3/datatables/SHARADAR/SEP` | `api.sharadar.com/v1.0/data/stocks` |
| `SHARADAR/SF1` | `data/fundamentals` (`dimension=ARQ`, `date` is the old `datekey`, `fiscalperiod` is given directly) |
| `SHARADAR/TICKERS` | `data/tickers?table=stocks` |
| `SHARADAR/EVENTS` | `data/events` (8-K items as **2 digits**: `11` = 1.01, `42` = 4.02; the old loader's 3-digit codes matched nothing) |
| `api_key` query param | `x-api-key` header, so keys never reach request logs |
| bulk zip export | subscription-gated; the loader paginates `limit`/`offset` and backs off on 429 |

`fundamentals.marketcap` is in raw dollars; `daily.marketcap` is in millions. The loader uses the former.

## Frozen parameters (v1.0)

See `config/strategy.v1.json`. Only `ear_threshold_pct` and `momentum_top_pct` are tunable by walk-forward. Every other value is fixed; changing one requires a new strategy version.
