# Decisions log

Resolutions of the §12 open decisions, plus every place the implementation
deviates from plan v0.1 and why. An entry is frozen once a backtest has been
run against it; changing a frozen entry bumps `strategy_version`.

## Resolved §12 decisions (v0.1 defaults, frozen in `config/strategy.v1.json`)

>  **Gate G1 was run on the full universe on 2026-09-15 and FAILED.** See
> [G1-RESULT.md](G1-RESULT.md). The v1.0 signal has no forward drift before costs:
> its candidates underperform SPY by 0.80% over the 40-session holding window.
> v1.0 does not proceed to paper trading, and the v2 sleeve stays blocked behind it.
> The parameters below are kept as the record of what was tested and frozen.

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
| Point-in-time fundamentals, prices, constituents (backtest) | Sharadar Bundle 10yr | **OK** since 2026-09-15. 2,168 symbols above the cap floor, 4.4M bars, 73k events, delisted included. Bulk CSV export (302 to a presigned zip) pulls 1.9 GB in ~30 s; paginating would take hours. ETFs are in `funds`, not `stocks`, so the benchmark comes from there. |
| Quoted spreads | none | **Not available.** `median_spread_bps` is written as 0 and the rule is logged as not enforced. |
| Claude validator | Anthropic | **Working** (verified end to end 2026-09-15, $0.066 per candidate). See "Claude validator, measured against the live API" below. |

Consequences:

* **Gate G1 was attempted and failed** on the full universe: deflated Sharpe −1.77, net return positive in 2 of 7 folds. See [G1-RESULT.md](G1-RESULT.md).
* **Gate G4 accrual can start as soon as paper trading starts**, but only forward: the validator reads the live web and so cannot be replayed over history (see below).
* The two universe rules that cannot be sourced (transcript, spread) are **reported as unenforced on every build** rather than defaulted silently, so the universe size is never mistaken for a filtered one.

### Claude validator, measured against the live API (2026-09-15)

One real candidate (IBM's 2025-10-27 reaction) run through the real two-step call:

| | |
|---|---|
| Verdict | REJECT, flag `ONE_OFF_ITEM`, confidence 0.95, 4 citations |
| Finding | the GAAP EPS jump was aided by a tax benefit rather than operations |
| Latency | 10–17 s per candidate |
| Cost | $0.066 per candidate, about $1.30/day at 20 candidates, roughly $40/month |

Three things the live run changed:

* **Domain allowlist.** Reuters, AP, WSJ, FT, MarketWatch and Barron's block Anthropic's crawler, and naming any of them makes the API reject the *entire* search request with a 400. The allowlist is now SEC plus the PR wires plus Bloomberg, CNBC and Nasdaq, all verified reachable. Opinion sites stay out on quality grounds.
* **Timeout 20 s → 60 s** (deviation from §4). Measured latency is 10–17 s, so a 20 s deadline manufactures `ERROR` verdicts, and three in a row pause new entries. There is no latency pressure at all: the strategy publishes candidates at 16:20 ET and entries are placed at 09:31 ET the next morning.
* **Structured outputs reject `minimum`/`maximum` on numbers.** `confidence` is now an unconstrained number in the schema and is clamped in `Verdict.to_payload`.

**The validator cannot be run on historical candidates.** Its research step searches the live web, so for a past candidate it returns information published after that candidate's session date. In the IBM test it reported on the Q4 report from January 2026 while judging the October 2025 reaction. That is correct in live and shadow mode, where the candidate is the most recent report, and fatal for any attempt to backfill a Gate G4 sample by replaying history. G4 accrues forward only, which is what §7.3's 12–18 months assumes.

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

## v2 research foundations (2026-09-16)

The v2 hypothesis pre-tests (plan: ranked hypotheses H1–H5, Stage 0/1) run on a new research panel, not on `data/sharadar/`. Recorded here because each point changes a number someone might otherwise compare directly with the v1 reports.

* **`data/sharadar/` is selection-biased below its floor.** `build_from_bulk` keeps only symbols whose market cap was *ever* above the floor inside the window. Every sub-floor name in it is one that later grew, so the event study's "under $2B: +4.9% to +10.7%" row is inflated by an unknown amount. `python/autotrader/research/panel.py` rebuilds from `data/raw/` over all 17,049 domestic common-stock tickers (delisted included) and decides membership from data known at each close: market cap ≥ $500M, unadjusted price ≥ $5, 20-day dollar ADV ≥ $5M, listed ≥ 365 days. About 2,000 names qualify at each year-end (roughly 550 at $0.5–2B, 550 at $2–5B, 1,000 above $5B).
* **SF1 `sharesbas` is restated for later splits.** AAPL's 2020-07-31 filing, a month before its 4:1 split, already reports 17.1B shares, and SF1 `marketcap` equals split-adjusted price × `sharesbas`. So daily market cap is split-adjusted `close` × `sharesbas` × `sharefactor`, and year-over-year share changes need no split correction. The plan said `closeunadj`; that would have overstated pre-split caps by the split ratio.
* **Total-return benchmark.** Stock returns use `closeadj` (dividends included). The SPY rows in `data/sharadar/bars.csv` are price-only, which would have added SPY's dividend yield (~1.5%/yr) to every excess return. The panel takes SPY `closeadj` from Sharadar `funds`.
* **Hold-out sealed at 2025-09-15** (`research/seal.py`). Research loaders stop there. Reading further needs `--unseal --reason`, which writes to the trial ledger.
* **Trial ledger** `docs/trials.jsonl`, seeded with 437 prior configurations (v1.0 on the Dow-30 sample 24; v1.0, v1.1, v1.2 walk-forwards 84 each; the 519-candidate raw check 1; the event-study sweep 160). Every later pre-test and backtest appends to it, and Stage 2's deflated Sharpe must be penalised for the total.
* **Fama-French factors** from the Ken French library, `data/factors/` with sha256 in `SOURCE.md`. The daily file currently ends 2026-07-31.
* **Sharadar insiders table is entitled** (probed 2026-09-16: `data/insiders` returns Form 4 rows). `institutions` is not ("Unknown table"). H5 (insider-purchase drift) is therefore testable without a new subscription.

## Methodology shift: brute-force grid with DSR + PBO, after H1-H4 (2026-09-16)

All four pre-registered v2 hypotheses failed (`docs/STAGE1-RESULT.md`); the trial
ledger stood at 627 configurations. `docs/STAGE1-RESULT.md`'s own conclusion left
two paths: accept the negative result and stop, or spend the sealed hold-out year
on one more pre-registered re-specification. The owner chose a third path instead:
stop hand-picking hypotheses one at a time and search the signal space
exhaustively in one shot, using data already licensed but not fully used (full SF1
fundamentals beyond `gp`/`assets`/`sharesbas`, Sharadar insiders, broader 8-K event
codes), correcting for the overfitting a wide search creates rather than avoiding
it by pre-registration.

* **What changed.** `docs/prereg/BRUTEFORCE-v1.md` is a frozen grid (19 signal
  arms x 3 bucket counts x 6 horizons x 4 bands = 1,368 configurations), committed
  before it runs like every other prereg file, but with no per-cell pass rule.
  Instead: Deflated Sharpe Ratio (needs > 0, reusing `cpp/src/backtester/metrics.cpp`
  via a new `at_dsr` CLI so Python does not reimplement the formula) **and**
  Probability of Backtest Overfitting via Combinatorially Symmetric
  Cross-Validation (needs < 0.5, `python/autotrader/research/pbo.py`, new --
  nothing in this repo did CSCV before). Both, not either.
* **Why DSR alone was not enough.** DSR corrects the significance bar for how many
  configurations were tried; it says nothing about whether the specific winner a
  search selects then degrades out of sample, which is exactly what a search over
  1,368 configurations needs checked. PBO/CSCV is the literature's standard
  companion for that (Bailey, Borwein, Lopez de Prado & Zhu 2014), not previously
  used here because H1-H4 each tested one fixed hypothesis with nothing to select
  among.
* **What stayed the same.** The grid still runs only on the pre-seal panel; the
  sealed year is still spent at most once, under the existing Stage-2 hold-out
  criterion (`docs/prereg/README.md`); the grid file is still committed and frozen
  before it touches data, exactly like H1-H4's pre-registrations; a run still
  appends exactly one ledger entry sized to the grid.
* **Scope boundary.** EAR, announcement volume, SUE and SRUE are deliberately not
  re-run in the new grid: H3 already tested all four with proper event-triggered
  timing and found nothing in any band, and re-testing them through a monthly
  cross-sectional proxy would use worse timing precision while still spending
  trial-count budget. The new grid's budget goes to families H1-H4 did not test.

## Strategy-family pivot: ETF time-series trend-following, after the brute-force grid (2026-09-16)

The brute-force grid also failed (`docs/BRUTEFORCE-RESULT.md`: DSR -0.635, PBO
0.630; ledger at 1,995 configurations). Every strategy tried on single-name US
equities — the plan's entire original scope (`docs/DESIGN.md` §1) — has now
failed. The owner chose to pivot strategy families entirely rather than search
equities further: **time-series trend-following** (each instrument traded
against its own trend, not ranked against peers) on a small, fixed universe of
18 liquid ETFs spanning equities, bonds, commodities and currencies — the
classic managed-futures/CTA approach, historically more durable live than
cross-sectional equity factor-picking, and a materially narrower hypothesis
space (one literature-established formulation, not a family to search).

* **What changed.** `docs/prereg/TSMOM-v1.md` pre-registers one specification
  (Moskowitz, Ooi & Pedersen 2012, 12-month sign-of-return, inverse-vol scaled,
  gross exposure capped at 1.0x — no leverage, a stated departure from the
  paper's own ~40%-vol target, since this is a cash account not a futures
  book) plus three named secondary variants, reported only. This is a return to
  the H1-H4 discipline (one frozen idea, not a grid) rather than a continuation
  of the brute-force pattern: PBO/CSCV is explicitly not run, because there is
  no search and no in-sample winner for it to interrogate.
* **New research infrastructure, Python-only.** `python/autotrader/research/
  etf_panel.py` (a small bespoke ETF loader from Sharadar `funds` — deliberately
  not a reuse of `panel.py`'s `Panel`, which exists to answer a point-in-time
  stock-universe-membership question this doesn't have), `tsmom.py` (signal,
  turnover-based cost model sourced from a Corwin & Schultz 2012 spread
  estimate on `funds`' own high/low fields, portfolio construction), and
  `walkforward.py` (purge/embargo fold geometry ported from
  `cpp/src/backtester/walk_forward.cpp`, and the shared `at_dsr` CLI caller).
  Wired as `at-research trend --prereg docs/prereg/TSMOM-v1.md`.
* **Data limitation, accepted in advance.** Probed 2026-09-16: all 18 ETFs are
  entitled in Sharadar `funds`, but every one's data starts exactly 2016-09-19 —
  the subscription's 10-year entitlement window, not each ETF's real listing
  date. The usable backtest window is ~9 years, not the longer history a
  CTA-style strategy would ideally want, though it does span two real stress
  regimes (the 2020 COVID crash, the 2022 bond bear market).
* **No C++, schema, or live-service work yet.** This is explicitly scoped as a
  cheap Python validation phase before any of the live system changes; see
  `docs/prereg/TSMOM-v1.md`'s "Out of scope for this phase." The owner does not
  want to deploy even paper trading without a genuinely validated strategy.

## Phase 2: wiring TSMOM into the live C++ system (2026-09-16)

TSMOM-v1 passed both its development gate and the sealed hold-out
(`docs/TSMOM-RESULT.md`) -- the first strategy in this project's history to clear a
gate. This phase ports the validated spec into the C++ system that already runs the
earnings-momentum strategy, without forking shared strategy/risk code. Three
architecture decisions were made explicitly before implementation and are not
re-litigated here: (1) ship the real, gated primary spec including shorts on
bonds/FX/commodities, with real short-sale support built into sizing/risk/execution/
ledger, rather than a long-only substitute; (2) TSMOM candidates bypass the Claude
validator sidecar entirely -- no LLM veto for a systematic signal with no
discretionary thesis; (3) do not genericize `walk_forward.cpp`'s grid search (TSMOM
has zero tunable parameters), and build a parity/regression test against the
published Python numbers instead.

* **What shipped.** A faithful C++ port of the primary spec (`strategy/tsmom_signal.
  {hpp,cpp}`, `strategy/tsmom_sizing.{hpp,cpp}`, `strategy/tsmom_universe.{hpp,cpp}`,
  `strategy/tsmom_params.{hpp,cpp}`, `config/strategy.v3.json`); a monthly-rebalance
  path on `StrategyEngine` (`evaluate_rebalance_session`) driven by a new
  `MonthlyTrigger`; short-sale-aware `Ledger::apply_rebalance_fill` (opens, closes,
  resizes and zero-crossing flips in one fill, composed from the existing close/open
  P&L math); a new `asset_class_cap_pct` risk limit mirroring `sector_cap_pct`
  end-to-end (`RiskLimits`, `RiskManager`, `Ledger`, `portfolio.state` schema); a new
  `RiskManager::evaluate_rebalance` path (no validator wait, no SPY-trend/earnings-
  gate/max-open-positions checks -- this is a resize of a fixed 18-name book, not a
  bounded set of new entries); a new `REBALANCE_TO_WEIGHT` order intent through
  execution (`submit_rebalance`) and the portfolio service's fill handler
  (`apply_rebalance_fill`, keyed on a new `target_qty` field threaded through
  `orders.approved`/`orders.filled` because a rebalance fill's side alone doesn't
  say how to apply it -- a sell can reduce a long, open/increase a short, or flip
  through zero); `signals.candidate`/`orders.approved`/`orders.filled`/
  `portfolio.state` schema surgery (a new `TSMOM_ETF_V1` signal_type, conditionally
  required fields via draft-07 `if`/`then`, validated in both the C++ and Python
  `SchemaRegistry`s); the ingestor pulling bars for the fixed 18-ETF universe
  (`strategy/tsmom_universe.cpp`'s `kTsmomUniverse`, which they were previously
  excluded from entirely by `exclude_etfs: true`); a new `at_tsmom_svc` binary
  (separate process from `at_strategy_svc` -- independent failure domain, no
  `events.earnings` dependency, a different cadence), wired into
  `docs/RUNBOOK.md` and `scripts/run_paper.{ps1,sh}`; and a real, data-backed C++
  parity test (`cpp/tests/test_tsmom_signal.cpp`, fixture exported by
  `python/autotrader/research/export_tsmom_fixture.py` from the same cached
  development panel TSMOM-v1 used) confirming the C++ signal math reproduces
  Python's on real formation dates -- momentum sign matches exactly; vol/weight
  magnitudes match within a tolerance derived from, and documented against, the
  system's own cents-quantization of a continuously-adjusted total-return price
  series (this project's `Cents` money convention applied to a synthetic series
  that was never really "money" in the first place).

* **Follow-up work (2026-09-17), closing out everything flagged above except one
  item.**
  * **Backtester parity harness, built.** `cpp/src/backtester/tsmom_backtester.
    {hpp,cpp}` (`run_tsmom_gross_backtest`) is a direct monthly-formation replay
    over `MarketStore`'s total-return-adjusted bars -- deliberately not routed
    through `Backtester::run`'s earnings-shaped daily-event loop, nor through
    `Ledger`/`RiskManager` (this harness checks the signal + return-attribution
    *formula*, not risk-gate logic, which is exercised separately and live-shaped
    in `test_tsmom_risk.cpp`). Gross only, matching decision 3's scoping: no
    turnover cost, no idle-capital risk-free credit on either side of the
    comparison. Checked in `cpp/tests/test_tsmom_backtest.cpp` against a second
    Python fixture (`python/autotrader/research/export_tsmom_backtest_fixture.py`,
    also `end=SEAL_DATE`, also not a new trial) covering a real ~2.5-year,
    600-session window of the development panel spanning several formation
    transitions -- 600/600 days agree within a documented tolerance (most days
    within a few basis points; a handful spike into the tens of basis points on
    days when an already-characterized, already-accepted weight-quantization
    effect coincides with an unusually large single-day move in the affected
    instrument, explained and bounded in the test's own comments). This is what
    `docs/prereg/TSMOM-v1-gates.md`'s proposed G1 parity check now points to.
  * **Live dividend/distribution adjustment, fixed.** `AlpacaClient::daily_bars`
    takes an `adjustment` parameter (`"split"`, unchanged default, vs. `"all"`).
    The ingestor now pulls a *second*, separate bar series for the TSMOM universe
    on a new subject, `market.data.bar_tr.*` (`IngestorService::pull_bars_tr`),
    requesting `adjustment=all` (Alpaca's own split+dividend total-return
    adjustment) -- kept off the existing `market.data.bar.*` deliberately, because
    SPY is *both* the earnings strategy's `trend_symbol` (needs raw/split-adjusted
    prices for its SMA-200 filter) and a TSMOM universe member (needs
    total-return-adjusted prices for its momentum/vol): one subject cannot serve
    both without corrupting one consumer's series. `at_tsmom_svc` subscribes to
    `market.data.bar_tr.*` instead of `market.data.bar.*`.
  * **Shortable/easy-to-borrow verification, added.** `AlpacaClient::
    shortable_flags` (`GET /v2/assets/{symbol}`) is polled once daily by the
    ingestor for the TSMOM universe and published on a new
    `market.data.shortable.*` subject; `RiskManager::evaluate_rebalance` gates on
    it (`check("shortable", ...)`), but *only* when an order would increase a
    short (open one from flat/long, or add to an existing one) -- reducing a short
    or going long never needs to borrow more, so never blocks on this. An
    unconfirmed symbol (no `market.data.shortable.*` seen yet, or the broker
    reports it false) blocks rather than assumes shortable, since this is a
    broker-enforced fact, not a modeling choice.
  * **Margin/buying-power, added.** `AlpacaAccount` now carries a second field,
    `marginable_buying_power_cents` (Alpaca's plain `buying_power`, Reg-T), kept
    separate from the existing `buying_power_cents` (`non_marginable_buying_power`,
    which the earnings strategy's cash-account sizing still uses unchanged). The
    portfolio service syncs it into a new `Ledger::margin_buying_power_cents` /
    `portfolio.state.margin_buying_power_cents`, and `evaluate_rebalance`'s
    buying-power check now reads that instead -- still a secondary sanity check,
    since `gross_exposure_cap_pct=100%` (matching the spec's own no-leverage cap)
    remains the real binding constraint. Confirmed against a real account response
    from the configured paper account (see next item): `non_marginable_buying_power`
    equaled cash exactly and `buying_power` reflected the account's margin
    multiplier, matching this field mapping.
  * **Alpaca long-to-short single-order flip -- partially verified, one genuine
    finding.** Ran a live probe against the configured Alpaca paper account
    (explicitly approved first): account confirmed `shorting_enabled: true`,
    `multiplier: "4"` (margin account). The probe itself was inconclusive on the
    core question (does a single sell order for more shares than currently held
    net a close+reverse in one fill?) because the market was closed at the time
    and Alpaca's paper matching only simulates fills during real market hours --
    the test buy order sat `accepted`/unfilled. It did surface one real, useful
    fact: Alpaca's wash-trade guard rejects submitting an opposite-side order for
    the same symbol while an order on the current side is still open/unfilled
    (`"potential wash trade detected... opposite side market/stop order exists"`).
    This does not affect `submit_rebalance` under TSMOM's actual cadence (one
    `tif=day` order per symbol per month, which fills or expires same-day long
    before the next month's formation runs), so no code change followed from it --
    noted here because it's a real broker behavior worth knowing about, not
    because it changes the design. The account was left clean (no positions, no
    open orders) after the probe. **The core single-order-flip question during
    live market hours remains unverified** -- the only item from the original
    flagged list still open.
  * **`docs/prereg/TSMOM-v1-gates.md`** still proposes G1/G3 pass criteria for a
    rebalanced book (replacing round-trip trade counts with formation-month
    counts) but leaves the exact formation-month floor as an open number for the
    owner to set, not a decision made here.

* **Deployment-readiness audit (2026-09-17), asked directly: "anything in the way
  of deployment?"** Triggered a targeted review of the broker-sync/reconciliation
  path, which every prior test in this phase exercised only through synthetic
  `Ledger`/`RiskManager` fixtures, never through the actual `AlpacaClient` ->
  reconciler chain.
  * **Found and fixed: the reconciler's `unprotected_position` check would have
    permanently blocked every TSMOM long position.** `reconcile_diff`
    (`cpp/src/reconciler/service.cpp`) flags a broker position as unprotected
    when no resting stop order covers it -- correct for the earnings strategy,
    which always places one, but TSMOM never places a stop by design (monthly
    rebalance is its only exit mechanism). Before this fix, every TSMOM long
    position would have permanently failed reconciliation and triggered
    `control.pause_new` the moment it synced from the broker. Fixed by skipping
    the check for any internal position carrying a non-empty `asset_class`
    (the same TSMOM-ownership tag `end_of_session_sweep`'s exit-sweep skip
    already uses). Not caught earlier because no test in this phase exercised
    `reconcile_diff` with a TSMOM-tagged position -- `at_reconciler` has no unit
    tests at all (gated behind `AT_WITH_NET`, not linked into `at_tests`, same
    testing-boundary precedent as `execution/engine.cpp` and
    `ingestor/service.cpp`), so this was only found by deliberately re-reading
    the reconciliation path end to end rather than by a failing test.
  * **Investigated and ruled out: a claimed `qty`-sign bug.** A first pass
    (subagent-assisted) concluded Alpaca's `/v2/positions` `qty` is an unsigned
    magnitude with direction carried separately in a `side` field, and that this
    codebase's `AlpacaClient::positions()` (never reads `side`) would silently
    drop the sign of every TSMOM short, corrupting `Ledger::sync_from_broker`.
    Independently verified before touching any code (a live paper-account check
    was the deciding step, since the docs alone were inconsistent depending on
    which page/summary was consulted): Alpaca's actual API returns `qty` already
    signed (`"side": "short", "qty": "-2478"` in a real example, with
    `market_value`/`cost_basis`/`unrealized_pl` also negative for a short) --
    this codebase's existing `qty_of()` already parses a signed decimal string
    correctly, so `AlpacaClient`/`Ledger::sync_from_broker`/`PortfolioService`
    were already correct as written. No code change was made for this; noted
    here so the (incorrect) claim doesn't get rediscovered and acted on later
    without the verification trail.
  * **Still open, genuinely blocking a real deployment (not just a nice-to-have):**
    the full service stack (`nats-server` + all services including the new
    `at_tsmom_svc`) has never actually been started together end-to-end in this
    phase -- every check in this phase ran through `at_tests`/`pytest`, never
    through `scripts/run_paper.{ps1,sh}`. Gate G2's fault-injection scenarios
    (`scripts/fault_injection.py`) don't exercise TSMOM order flow at all (its
    only order-construction helper hardcodes `intent=ENTRY, side=buy`; its
    service-start list in `docs/TESTING.md` doesn't even start `at_tsmom_svc`) --
    extending G2 to cover a `REBALANCE_TO_WEIGHT` short-and-flip scenario against
    the fake broker would be the natural way to close both this gap and the
    still-unverified Alpaca single-order-flip question above without needing
    live market hours. `bootstrap_streams.py` and both `run_paper` scripts were
    checked and need no changes (confirmed: they read `schemas/topics.json`
    dynamically / the new service lines are consistent with their siblings).

* **Scope boundary held.** This phase does not start paper trading -- that remains
  an explicit, separate decision after this work is reviewed, per the owner's
  stated position (`docs/TSMOM-RESULT.md`).
