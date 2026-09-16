# Brute-force signal grid: FAIL

Run 2026-09-16 against `docs/prereg/BRUTEFORCE-v1.md`, committed at `5ca4810` before
the grid touched data. Clean point-in-time panel, 2016-09-16 to 2025-09-15; the
following year stays sealed. Full cell-by-cell results in
`var/research/bruteforce/20260916T184320.{md,json}` (gitignored, reproducible with
the command at the end). The trial ledger now stands at **1,995 configurations**
(627 before this run + 1,368 here).

## Verdict

**FAIL on both checks.**

| Check | Needs | Actual |
|---|---|---|
| Deflated Sharpe Ratio | > 0 | **-0.635** annualised |
| Probability of Backtest Overfitting (CSCV) | < 0.5 | **0.630** |

Best cell by per-period Sharpe: `asset_growth`, 10 buckets, 5-session horizon,
500M-2B band -- raw annualised Sharpe **+0.560** before correction. That number is
what a search that stopped at DSR alone, or that only looked at raw Sharpe, would
have reported as a promising lead. Penalised for the 1,224 usable trial Sharpes
this grid actually produced (`var_sr_trials` 0.0108), the same cell's deflated
Sharpe is **-0.635**: the observed 0.162-per-period Sharpe sits well below the
0.345-per-period Sharpe the null distribution of "best of 1,224 tries" predicts by
chance alone. P(SR > SR0) = 0.040.

PBO adds an independent confirmation. Across 12,870 combinatorially symmetric
train/test splits (16-way CSCV) over the 1,224 cells with enough valid formation
months, the in-sample winner finished below the out-of-sample median **63% of the
time** -- worse than the 50% a genuinely random selection would produce, not better.
Whatever this grid's best-looking cell is on any given split of the data, it does
not stay the best-looking cell on the rest of it.

## What the top of the grid actually looks like

No cell comes close to significant. Best 5 by primary-horizon (60-session)
Newey-West t, out of 1,224 tested (144 of 1,368 cells were dropped for having
fewer than 10 valid formation months, mostly `insider_buy`/`ownership_family`,
which have no data yet -- see "Known gaps" below):

| arm | buckets | band | top excess % | t | spearman |
|---|---|---|---|---|---|
| mom_12_1 | 10 | 5B+ | +0.52 | +0.49 | +0.30 |
| resid_mom | 10 | 500M-2B | +0.41 | +0.17 | +0.85 |
| resid_mom | 5 | 500M-2B | +0.27 | +0.13 | +1.00 |
| value_issuance_family | 10 | 500M-2B | +0.25 | +0.09 | +0.76 |
| resid_mom | 10 | 2B-5B | +0.12 | +0.09 | +0.88 |

A t-statistic of 0.49 is not a near-miss to build on; it is what noise looks like
across 1,224 tries. This is consistent with H1 and H2's own primary-horizon numbers
(docs/STAGE1-RESULT.md), which is expected: `mom_12_1`, `resid_mom` and the
value/issuance composite share signal ingredients with H1 and H2, and none of the
19 genuinely new arms (reversal, 52-week-high, idiosyncratic volatility, Amihud
illiquidity, asset growth, accruals, book-to-market, leverage change, insider
buying, recent-event flags) did any better.

## Known gaps in this run

* **`insider_buy` and `ownership_family` have no data.** `scripts/fetch_insiders_bulk.py`
  was never run against this account's Sharadar key, so `data/raw/insiders-*.csv`
  does not exist and both arms were dropped from DSR/PBO for insufficient valid
  formations (documented behaviour, not a silent gap -- see the warning in the run
  log and `panel.insiders_path()`). If insider data is fetched later, re-running
  the grid to include it is a **new** trial count, not a correction of this one:
  this result is not invalidated, but a future run must penalise DSR for having
  looked twice.
* SF2 field names in `panel.load_insider_transactions` are asserted from public
  Sharadar documentation, not yet verified against a live response (per
  `docs/prereg/BRUTEFORCE-v1.md`'s own "known limitations" section). Verify before
  trusting any future run that includes this arm.

## What this means for the plan

* The sealed hold-out year stays sealed and unspent. Nothing here cleared the
  DSR > 0 AND PBO < 0.5 bar needed to earn a hold-out run.
* This is a materially stronger basis to stop than `docs/STAGE1-RESULT.md`'s open
  question. That result left two paths (accept and stop, or one more pre-registered
  attempt) because four narrow hypotheses leave open the possibility that the right
  fifth hypothesis was simply never tried. A 1,368-configuration grid spanning
  price/technical, fundamental and event-based families, corrected for exactly how
  hard it searched, closes most of that gap. The honest reading is
  `docs/NEXT-STEPS.md` option A: accept the negative result on this data, this
  universe, and this holding-period family, and stop.
* If the insider data is fetched and still worth checking, or a genuinely new idea
  outside these signal families emerges, it is a new pre-registration or a new
  frozen grid against the ledger's new total of 1,995 -- not a re-run of this one.

## Reproduce

```
.venv/Scripts/at-research build-panel            # cache hit if already built
at-research sweep --grid docs/prereg/BRUTEFORCE-v1.md
```

Every rerun appends to `docs/trials.jsonl` and raises the bar for every run after
it.
