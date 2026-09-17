# TSMOM-v1: time-series trend-following on a fixed ETF universe

Implementation: `python/autotrader/research/tsmom.py`, `etf_panel.py`,
`walkforward.py`. Written 2026-09-16, before any of this touched data.

## Why this is a different kind of pre-registration

Every hypothesis before this one (H1-H4, and the BRUTEFORCE-v1 grid) ranked
stocks against each other; all of them failed (docs/STAGE1-RESULT.md,
docs/BRUTEFORCE-RESULT.md). This is a genuine strategy-family pivot: trade
each of a small, fixed set of liquid ETFs against its **own** trend, not
against its peers. It is also a narrower hypothesis space than the equity
work — Moskowitz, Ooi & Pedersen (2012, *"Time Series Momentum,"* Journal of
Financial Economics) is a single, decades-replicated formulation, not a
family of thousands of candidate signals — so the discipline here is **one
pre-registered specification**, not a second brute-force search. Repeating
the brute-force pattern on a new asset class would spend more of the ledger's
budget re-learning a lesson already learned. Two named secondary variants are
reported for robustness and are never eligible to rescue a primary fail, the
same discipline `H4-trend-overlay.md` already uses for its `sma10m` rule.

## Universe (18 tickers, 4 asset classes, no sector bets)

| Class | Tickers |
|---|---|
| Equity index | SPY, QQQ, IWM, EFA, EEM |
| Rates/credit | TLT, IEF, SHY, LQD, HYG |
| Commodities | GLD, SLV, USO, DBC |
| Currencies | UUP, FXE, FXY, FXB |

Sector SPDRs are deliberately excluded from v1: they are highly correlated
with SPY and add book concentration rather than genuine cross-asset
diversification, which is the point of a CTA-style book. Prices come from
Sharadar `funds` (confirmed general-purpose, not SPY-special-cased) via
`python/autotrader/research/etf_panel.py`.

**Probed 2026-09-16: all 18 tickers are entitled, but every one starts
exactly 2016-09-19** — the Sharadar subscription's 10-year entitlement
window (docs/DECISIONS.md), not each ETF's real listing date (SPY has traded
since 1993). The usable window is therefore **2016-09-19 to
`seal.SEAL_DATE` (2025-09-15), ~9 years**, not the longer history a
CTA-style strategy would ideally want. It does span two real stress
regimes (the 2020 COVID crash and the 2022 bond bear market), so it is not
without value, but this is a real, accepted limitation, stated here rather
than absorbed silently.

## Primary specification

Monthly formation, same `month_end_indices()` cadence H1/H2/H4 already use.
For each instrument *i* at formation session *f*:

```
mom_i   = sign(close_tr[f] / close_tr[f-252] - 1)         # 12-month total-return sign
vol_i   = annualized stdev of daily total returns, trailing 60 sessions
raw_w_i = mom_i / vol_i                                     # inverse-vol scaled
scale   = min(1.0, 1.0 / sum(|raw_w_i|))   if sum(|raw_w_i|) > 0 else 0
w_i     = raw_w_i * scale                                    # DOWN-scale only, never lever above 1.0x
```

Held from the open of session *f+1* to the open of the next formation's
*f'+1* (same lag convention `h4_trend.py`'s `book_returns` already uses).
Idle capital (`1 - sum(|w_i|)`) earns the Fama-French daily risk-free rate
(`data/factors/ff3_daily.csv`).

**Deliberate, stated departure from Moskowitz-Ooi-Pedersen (2012):** the
paper targets roughly 40% annualized volatility per instrument, which
assumes futures-level leverage. This project is a cash ETF account per the
owner's stated goal (real, modest supplemental income — not a leveraged
futures book), so v1 caps **gross exposure at 100% of capital, no
leverage** — `scale` only ever shrinks, never grows. This is the single most
consequential deviation from the textbook formula and is named explicitly
because it will materially lower Sharpe relative to published results. That
is the honest, deployable version, not the academic-leverage version.

## Secondary variants (reported only, never gating)

* `secondary_1m` — 1-month lookback instead of 12-month.
* `secondary_3m` — 3-month lookback instead of 12-month.
* `secondary_long_only_12m` — 12-month lookback with `mom_i` floored at 0
  (no shorting). Tests how much of any edge depends on shorting bonds/FX/
  commodities, which matters for what a live version would need to support.

## Cost model — sourced, not assumed

A real mechanical difference from `pretest.py`'s per-holding-period
`ROUND_TRIP_BPS`: this book holds continuously and only *adjusts* weights
monthly, so cost is charged on **turnover**:

```
cost_f = sum_i |w_i,f - w_i,f-1| * (spread_roundtrip_bps_i + 10bp) / 2 / 100
```

The per-ETF spread estimate is Corwin & Schultz (2012, *Journal of Finance*,
*"A Simple Way to Estimate Bid-Ask Spreads from Daily High and Low
Prices"*), computed from the `high`/`low` fields `funds` already returns —
not a flat guessed number, so SPY/TLT/GLD correctly come out tighter than
FXY/FXB/HYG. The +10bp is the project's own existing conservative floor
(`commission_bps_per_side: 5.0`, already the default in every
`config/backtest.*.json`, round-tripped), so this cost model stays at least
as conservative as the equity work's rather than assumed free just because
Alpaca charges no stock/ETF commission. A missing/invalid Corwin-Schultz
estimate for a given ETF and day falls back to the widest spread quoted
elsewhere in the universe that day (or a fixed 50bp default if nothing is
available), never to a 0bp free cost.

## Validation mechanics

* **Folds**: rolling 3-year train / 1-year test, 1-month (21-session) purge,
  1-month (21-session) embargo (`walkforward.purge_embargo_folds`), expected
  to yield 5-6 folds from ~9 years of history. Classical purge/embargo
  leakage-prevention doesn't strictly apply here — the 252-session and
  60-session windows are cited constants, nothing is fitted per fold — but
  it still buys several genuinely independent out-of-sample blocks to check
  individually, and keeps this consistent with the project's own gate
  discipline.
* **Deflated Sharpe** via the existing `at_dsr` CLI
  (`cpp/src/backtester/metrics.cpp`, Bailey & López de Prado 2014), on the
  concatenated out-of-sample daily return series, `periods_per_year=252`
  (a genuine daily P&L series, unlike the equity brute-force grid's
  monthly-bucket 12.0). `trial_sharpes_per_period` = the primary spec's and
  all three secondary variants' per-period Sharpes (4 total) — conservative
  and honest, even though only the primary is pass/fail-eligible.
* **PBO/CSCV is deliberately not run.** `pbo.cscv_pbo()` measures whether
  the in-sample winner of a *search* degrades out of sample (it already
  raises `ValueError` below 2 candidates). This is one frozen,
  literature-cited spec with no selection step — there is no "winner" for
  CSCV to interrogate, so running it would answer a question that was never
  asked. Noted here as a decision, not an oversight.
* **Sub-period stability**: the concatenated OOS series split into thirds,
  same check `pretest.summarize()` applies to bucket sorts, ported to a
  single return series.
* **Hold-out**: identical mechanism to every other hypothesis
  (`seal.data_end()`/`seal.check_window()`, `--unseal --reason`, logged as
  `kind="holdout"`). One run only, after the primary passes development.

## Pass rule

| Check | Bar |
|---|---|
| Deflated Sharpe (`deflated_sharpe_annual`) | > 0 |
| OOS folds net positive | at least 2 of 3 (rounded up) |
| Sub-period stability | net return positive in all three thirds of the concatenated OOS series |
| Minimum sample | at least 4 OOS fold-years |

All four checks must pass. Then, only if development passes: one sealed
hold-out run, net Sharpe ≥ 0 **and** not more than ~2 standard errors below
the development OOS Sharpe (computed from the actual OOS series length).

If the primary spec fails development: write it up exactly as BRUTEFORCE-v1
was (`docs/TSMOM-RESULT.md`), and consider at most one further pre-registered
variant (e.g. a dual-moving-average crossover, the other standard TSMOM
formulation) before concluding trend-following on this universe doesn't
clear the bar either. Do not open a second brute-force grid.

## Trial counting

`n_configs = 4` (primary + 3 secondary variants), logged to
`docs/trials.jsonl` via `at-research trend --prereg docs/prereg/TSMOM-v1.md`,
same shape as every other hypothesis's ledger entry.

## Out of scope for this phase

New C++ `StrategyEngine` rebalance path, a `signals.candidate` schema
variant, an asset-class risk cap, a fixed-ETF `universe.cpp` source, a
genericized `walk_forward.cpp` grid search, `config/strategy.v3.json`, live
service wiring, and paper trading. Gated on this phase's result, not
designed here.
