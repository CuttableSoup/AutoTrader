# Brute-force signal grid v1

Implementation: `python/autotrader/research/sweep.py`. Shared conventions
(universe, timing, return definition, buckets, Newey-West t): [README.md](README.md),
except that this is not a single pre-registered hypothesis and has no per-cell pass
rule -- see "Overfitting control" below for what replaces it.

## Why a different method from H1-H4

H1-H4 each committed one hypothesis before touching data and were judged one at a
time; all four failed (docs/STAGE1-RESULT.md). The owner's decision after that
result was to stop hand-picking hypotheses and instead search the signal space
exhaustively in one shot, using data already licensed but not yet fully used, and
correct for the overfitting a wide search creates rather than avoiding it by
pre-registration. This file is that search's frozen specification, committed
before it is run, so the freedom to edit the grid after seeing results does not
become the same p-hacking pre-registration exists to prevent.

## Grid

Every arm is evaluated at every `(n_buckets, horizon, band)` combination:

| Dimension | Values |
|---|---|
| n_buckets | 3, 5, 10 |
| horizon (sessions) | 5, 10, 20, 40, 60, 120 (primary: 60) |
| band | 500M-2B, 2B-5B, 5B+, all |

### Arms (19: 14 solo signals + 5 family composites)

Solo, all already-licensed data, no new vendor spend (DECISIONS.md's blocked
items -- quoted spreads, transcripts, historical consensus -- stay out of scope):

| Arm | Source | New? |
|---|---|---|
| mom_12_1 | h1_resid_mom.momentum_12_1 | reused |
| resid_mom | h1_resid_mom.residual_momentum_scores | reused |
| gpa | h2_composite gross profitability | reused |
| iss | h2_composite share issuance | reused |
| reversal_1m | signals.reversal_1m (1-month reversal) | new |
| high52w | signals.high52w_proximity (52-week-high momentum) | new |
| idio_vol | signals.idio_vol_ff3 (low-vol anomaly, reuses H1's FF3 regression) | new |
| amihud | signals.amihud_illiquidity | new |
| asset_growth | signals.asset_growth (SF1 `assets`, same raw file) | new |
| accruals | signals.accruals (SF1 `netinc`/`ncfo`/`assets`) | new |
| book_to_market | signals.book_to_market (SF1 `equity`) | new |
| leverage_change | signals.leverage_change (SF1 `debt`/`assets`) | new |
| insider_buy | signals.insider_net_buying (Sharadar SF2, entitled per DECISIONS.md, unused until now) | new |
| event_flag | signals.recent_material_event_flag (Sharadar EVENTS, MATERIAL_8K_CODES beyond item 2.02) | new |

Composite (equal-weight winsorised z-score mean, gated arm's own family only --
not every pairwise/triplet subset, to keep the grid in the low thousands):

| Composite | Members |
|---|---|
| momentum_family | mom_12_1, resid_mom, reversal_1m, high52w |
| quality_family | gpa, accruals, leverage_change |
| value_issuance_family | book_to_market, iss, asset_growth |
| risk_family | idio_vol, amihud |
| ownership_family | insider_buy, event_flag |

**Grid size: 19 arms x 3 bucket counts x 6 horizons x 4 bands = 1,368 configurations.**

### Explicitly out of scope for this grid

EAR, announcement volume ratio, SUE and SRUE (H3's event-triggered signals) are
**not** re-run here. H3 already tested all four with proper point-in-time
event-mechanics (entry keyed to the actual earnings-event date, not a monthly
formation proxy) and found nothing in any band (docs/STAGE1-RESULT.md). Re-running
the same signals through a monthly cross-sectional proxy would use *worse* timing
precision than H3 already used and would not add information -- it would only
spend trial-count budget re-litigating a decisively answered question. The budget
here goes entirely to signal families H1-H4 did not test.

Trend overlay (H4's `rule_sma200`) is **not** part of the grid. It is evaluated
only on whichever composite clears both overfitting-control thresholds below, using
H4's existing machinery unchanged, exactly as H4 restricted itself to a promoted
book.

## Overfitting control (replaces a per-cell pass rule)

No cell needs to individually pass a Newey-West-t threshold. Instead, two checks
run over the whole grid's results:

1. **Deflated Sharpe Ratio** (`cpp/src/backtester/metrics.cpp`, Bailey & Lopez de
   Prado 2014, via the `at_dsr` CLI) on the grid's best cell by per-period Sharpe,
   penalised by the per-period Sharpe of every one of the 1,368 configurations
   (`sweep.run_dsr`). Needs **DSR > 0**.
2. **Probability of Backtest Overfitting via CSCV** (`autotrader.research.pbo`,
   Bailey, Borwein, Lopez de Prado & Zhu 2014) over every cell with at least 10
   valid formation months, 16-way combinatorially symmetric cross-validation.
   Needs **PBO < 0.5** (the literature's convention: below ~0.2 is comfortable,
   at 0.5 the in-sample winner carries no more information than a coin flip).

**Pass rule: DSR > 0 AND PBO < 0.5.** Both, not either -- DSR corrects the
significance bar for how many configurations were tried; PBO checks whether the
specific selected winner then degrades out of sample. A config that clears DSR by
chance among 1,368 trials but still shows high PBO is not evidence of anything.

A pass promotes the winning composite (its family, band, horizon, bucket count) to
one hold-out run against the sealed year (`docs/prereg/README.md`'s existing
Stage-2 criterion: net Sharpe >= 0 and >= the development out-of-sample Sharpe
minus 1.0), spending the seal exactly once via `at-research sweep --unseal
--reason ...`. A fail means this data has now been searched exhaustively across
every signal family with data already on hand, corrected honestly for how hard it
was searched -- a materially stronger basis to stop than STAGE1-RESULT.md's open
question, per NEXT-STEPS.md option A.

## Trial counting

This run appends one `sweep` entry to `docs/trials.jsonl` with `n_configs = 1368`,
whatever the outcome, and `var_sr_trials` set to the variance of the 1,368
per-period Sharpes -- available afterward as a floor for any later DSR computation,
the same way H1-H4's entries are.

## Known limitations, accepted in advance

* `insider_buy`'s field names (Sharadar SF2) are asserted from public documentation,
  not yet verified against this account's live response (`data/insiders`), unlike
  every other table this repo reads. If the fetched file's columns differ,
  `panel.load_insider_transactions` needs correcting before this arm's numbers can
  be trusted; a silent parse failure there would show up as an all-NaN arm, not a
  wrong number, because `pretest.run_monthly_sort` drops any all-NaN score.
* 19 arms sharing 4 of the 5 composite families' members means those members are
  not independent draws; DSR's trial-count penalty and PBO's CSCV both already
  account for this by construction (they operate on the realised per-cell return
  series, not on an assumption of independence between cells), but it is worth
  remembering that "1,368 configurations" is not 1,368 independent looks at the
  data.
* Monthly cross-sectional formation at multiple overlapping horizons (5 to 120
  sessions) reuses the same Newey-West-lag convention H1/H2/H4 already accepted;
  it is not a new statistical assumption introduced here.
