# Pre-registrations for the v2 hypothesis pre-tests

Written and committed 2026-09-16, **before** any of these tests first touched the data. `at-research pretest` refuses to run a pre-registration that is uncommitted or modified, and every report quotes the commit it ran against. Editing a file here after its test has run does not change that result; it creates a new trial, which goes in `docs/trials.jsonl` like any other.

Why this exists: the Sharadar data has already been searched hard (437 configurations before v2, see `docs/trials.jsonl`). A new idea is evidence only if its test was fixed before it met the data.

## Shared conventions (all hypotheses)

| | |
|---|---|
| Data | Clean point-in-time panel (`python/autotrader/research/panel.py`) from `data/raw/`, 2016-09-16 to **2025-09-15**. The following year is sealed for Stage 2's single hold-out run. |
| Universe at each close | Domestic common stock (delisted included), market cap ≥ $500M (split-adjusted close × SF1 `sharesbas` × `sharefactor` from the latest filing before that session), unadjusted price ≥ $5, 20-session mean dollar volume ≥ $5M, first price ≥ 365 days earlier. |
| Size bands | `500M-2B`, `2B-5B`, `5B+`, and `all`. Each band is sorted on its own. |
| Timing | Score formed at the close of session f; entry at the open of f+1; exit at the close of f+1+h. |
| Return | Total return (Sharadar `closeadj`; open scaled by closeadj/close), **excess over SPY total return** over the identical window, in percent. A name that stops trading is held at its last close for the rest of the window (delisting returns are not in the data). |
| Fundamentals | SF1 rows usable from the first session strictly after their filing `date`, carried forward at most 300 sessions. |
| Cost stress | One full round trip deducted from every holding: 50 bp at $500M–2B, 30 bp at $2–5B, 10 bp at $5B+, 30 bp for `all`. Stricter than a turnover-scaled cost, deliberately. Turnover is reported alongside. |
| Buckets | Rank-based; a bucket-formation cell with fewer than 5 names is dropped. |
| t-statistic | Newey-West (Bartlett) on the time series of per-formation bucket means, lags = ceil(h / 21) for monthly formations (overlapping holds), 1 for quarterly ones. |

## The pass rule

At the hypothesis's **primary horizon**, per band, **all** of:

1. top bucket mean excess > 0 **and** its Newey-West t > 2
2. Spearman correlation between bucket rank and bucket mean ≥ 0.8
3. top bucket mean > 0 in each of the three equal thirds of the formation dates
4. top bucket mean minus the band's round-trip cost > 0

Secondary horizons, reference arms and single-signal arms are reported and never rescue a fail.

## What happens next (fixed now)

* **Promotion to Stage 2.** H1 or H2 passing in a band promotes that hypothesis *in that band* to the C++ walk-forward. If it passes in several bands, the largest-cap passing band is used (lower costs, more capacity), and `all` is used only if no individual band passes. At most two of H1/H2 are promoted.
* **Only H3 passes:** Stage 2 becomes a v1.3 event specification on the existing engine (SUE trigger, 10-session hold, clean universe).
* **Kill criterion.** If neither H1 nor H2 passes in `5B+` or `2B-5B`, the long-only large/mid-cap premise is treated as unviable; no rebalance sleeve is built, and the next step is the case for buying spread data for the small-cap sleeves.
* **H4** only applies to a promoted book for which it passed.
* **Stage 2 hold-out criterion** (for any promoted hypothesis, fixed now, before the development walk-forward): a single run of the frozen configuration over 2025-09-16 to 2026-09-15 passes only if its net Sharpe is ≥ 0 **and** ≥ the development out-of-sample Sharpe minus 1.0 (about two standard errors on one year of daily returns). A fail there ends the hypothesis.
* **Trial counting.** Each run appends `bands × horizons × arms` configurations to `docs/trials.jsonl`; Stage 2's deflated Sharpe is penalised for the ledger total.

## Files

* [H1-residual-momentum.md](H1-residual-momentum.md)
* [H2-composite.md](H2-composite.md)
* [H3-relaxed-pead.md](H3-relaxed-pead.md)
* [H4-trend-overlay.md](H4-trend-overlay.md)
* H5 (insider purchases): the Sharadar insiders table is entitled (docs/DECISIONS.md). No pre-registration yet; H5 is a follow-up and is not run in this stage.
