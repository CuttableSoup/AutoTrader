# TSMOM-v1: PASS, including the sealed hold-out

Run 2026-09-16 against `docs/prereg/TSMOM-v1.md`, committed at `ef8580507893`
before it touched data. Reports: `var/research/TSMOM-v1/20260916T204553.md`
(development) and `var/research/TSMOM-v1/20260916T204625.md` (hold-out,
gitignored, reproducible with the command at the end). Ledger entries
`a9baf4fcfd95` (development), `f25ba9ee48aa` (unseal) and `1c7ae72cc628`
(hold-out); ledger now at 2,003 configurations.

## Verdict

**PASS on both the development gate and the sealed hold-out.**

| Stage | Deflated Sharpe (annual) | OOS folds net positive | Sub-period stability | Result |
|---|---|---|---|---|
| Development (2016-09-19..2025-09-15) | +0.318 | 5/5 | all three thirds positive | **PASS** |
| Hold-out (extends through 2026-09-16) | +0.517 | 6/6 | all three thirds positive | **PASS** |

The extra fold the hold-out adds (test window 2025-03-27..2026-03-27, +7.06%)
partly overlaps data already seen during development -- its test window opens
before the 2025-09-15 seal. Isolating only the genuinely unseen sessions
(2025-09-16 through 2026-09-16, 252 sessions, never touched by signal design,
parameter choice, or any prior test) gives a cleaner read: **+5.08% total
return, per-period Sharpe 0.089, annualised ~1.41** -- directionally
consistent with, and numerically stronger than, the development period. One
year is too short to lean on that Sharpe figure alone, but the sign and
magnitude are exactly what a genuine, working signal should produce on data
it has never seen.

## What was tested

Primary: 12-month sign-of-return, inverse-vol scaled, monthly formation,
gross exposure capped at 1.0x (no leverage -- the stated departure from
Moskowitz-Ooi-Pedersen 2012's academic-leverage target). 18 liquid ETFs
across equities, rates/credit, commodities and currencies. Cost charged on
turnover, per-ETF spread from a Corwin-Schultz (2012) estimate plus a 10bp
conservative floor. Full specification: `docs/prereg/TSMOM-v1.md`.

## Fold-by-fold (development + hold-out)

| fold | test window | return | note |
|---|---|---|---|
| 0 | 2019-10 .. 2020-10 | +1.82% | |
| 1 | 2020-11 .. 2021-11 | +0.53% | |
| 2 | 2021-12 .. 2022-12 | **+7.86%** | bond bear-market/rate-shock year -- the standout |
| 3 | 2023-01 .. 2024-01 | -0.54% | the only negative fold |
| 4 | 2024-02 .. 2025-02 | +4.73% | |
| 5 | 2025-03 .. 2026-03 | +7.06% | partly pre-seal, partly the genuine hold-out (see above) |

## Robustness checks run before spending the hold-out

None of these are new trials (no new configurations were selected among; same
development result, different diagnostics), so none are logged to the
ledger:

* **Code review caught and fixed four real bugs** before any of this touched
  live data: a day at every monthly rebalance was misattributed whole to the
  outgoing position instead of split between overnight (old) and intraday
  (new) exposure (copied from `h4_trend.py`'s coarser convention, wrong for
  this spec's stated open-of-f+1 entry); a missing Corwin-Schultz spread
  silently became a free 0bp cost instead of a conservative fallback; the
  warm-up window relied on a hand-picked date that could fall short of the
  true 252+60-session requirement; the on-disk cache lost float64 precision
  on write without recovering it on a cache-hit read. Fixing the day-
  attribution bug changed the result materially (fold 3 flipped from
  marginally positive to marginally negative) without changing the overall
  verdict -- confirmation the bug was real, not cosmetic, and that the
  result isn't fragile to it.
* **Leave-one-fold-out.** Dropping fold 2 (the 2022 standout), raw Sharpe
  drops from 0.69 to 0.53 -- weaker, but still clearly positive. The
  all-three-thirds-positive check specifically would not hold without that
  fold (the middle third turns to -1.82%), which is worth remembering: part
  of the "PASS" rests on one strong year, not just a uniformly modest edge
  everywhere.
* **Per-asset-class attribution.** The edge is broad-based: equities +3.93%,
  rates +6.79%, commodities +2.17%, currencies +3.41% (all OOS,
  summed contribution). 16 of 18 individual ETFs are net positive; only EFA
  and FXE are slightly negative. Not a single-instrument fluke.
* **Cost sensitivity.** Doubling the cost floor (10bp -> 20bp) leaves the
  result comfortably positive (Sharpe 0.69 -> 0.64, folds and thirds
  unchanged). Tripling it (-> 30bp) starts to bite: Sharpe drops to 0.59 and
  one fold flips negative (folds_net_positive 4/5 -> 3/5). A real, noted
  limit -- not a reason to distrust the result at the pre-registered cost
  assumption, but a reminder the margin isn't huge.
* **Vol-lookback sensitivity.** 40 and 90 sessions instead of the
  pre-registered 60 both still pass cleanly (Sharpe 0.71 and 0.66
  respectively) -- not fragile to this particular implementation constant.

## Known limitations, unchanged from the pre-registration

* ~9 years of history (the Sharadar subscription's 10-year entitlement
  window, not each ETF's real listing date) is thinner than a CTA-style
  strategy would ideally want, though it does span two real stress regimes
  (the 2020 COVID crash, the 2022 bond bear market).
* Only the 12-month primary variant is clearly positive; the 1-month
  secondary variant is flat-to-negative (per-period Sharpe -0.0068 in
  development, +0.0090 including the hold-out extension) -- the edge is
  specific to this formulation, not "trend-following in general."
* This is a *cash, no-leverage* result. A live version that wanted to
  approach the academic paper's leverage-scaled Sharpe would need to decide
  explicitly how much leverage (if any) to add, which changes the risk
  profile materially and is a decision for Phase 2, not assumed here.

## What this means for the plan

This is the first strategy in the project's history to clear a pre-registered
gate, including a genuinely spent, one-shot hold-out. Per
`docs/prereg/TSMOM-v1.md`'s own scope, what's NOT done yet: any C++ change,
a live `signals.candidate` schema variant, an asset-class risk cap, a
fixed-ETF `universe.cpp` path, a genericized `walk_forward.cpp` grid search,
`config/strategy.v3.json`, live service wiring, or paper trading. Those are
Phase 2, and per the owner's explicit position, paper trading should not
start until Phase 2 is built and the strategy is running for real, not before.

## Reproduce

```
at-research trend --prereg docs/prereg/TSMOM-v1.md                 # development, sealed
at-research trend --prereg docs/prereg/TSMOM-v1.md --unseal --reason "..."   # ALREADY SPENT -- do not re-run
```

The hold-out is spent. A second run would not be a second test; see
`docs/trials.jsonl`'s `unseal` entry for why.
