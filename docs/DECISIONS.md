# Decisions log

Resolutions of the §12 open decisions, plus every place the implementation
deviates from plan v0.1 and why. An entry is frozen once a backtest has been
run against it; changing a frozen entry bumps `strategy_version`.

## Resolved §12 decisions (v0.1 defaults, frozen in `config/strategy.v1.json`)

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

## Frozen parameters (v1.0)

See `config/strategy.v1.json`. Only `ear_threshold_pct` and `momentum_top_pct` are tunable by walk-forward. Every other value is fixed; changing one requires a new strategy version.
