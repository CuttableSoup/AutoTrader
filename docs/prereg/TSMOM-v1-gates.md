# TSMOM-v1: proposed G1/G3 pass criteria for a rebalanced book

This is a proposal for review, not a decided pass rule: it names the exact numbers
(`N` formation-month floors) that still need a deliberate choice, and it is cross-
referenced from `docs/DECISIONS.md` rather than silently edited into `docs/TESTING.md`.

## Why the existing G1/G3 bars don't map

`docs/TESTING.md`'s G1 (`≥ 30 out-of-sample trades`, from `walk_forward.cpp`'s grid
search) and G3 (`≥ 150 closed trades`) both count discrete round-trip entry/exit
events. That's the right unit for the earnings-momentum strategy: each candidate is
a bounded, independent bet with a clear open and close, and "how many independent
bets has this been tested/observed on" is exactly what a trade count measures.

TSMOM is a monthly resize-to-target-weight model on a fixed, always-held 18-name
book. A "trade" here is a monthly weight adjustment, and per `strategy/ledger.cpp`'s
`apply_rebalance_fill`, a `ClosedTrade` record is only produced for the portion of a
resize that reduces exposure toward or through zero -- most months, most of the 18
ETFs get a partial weight nudge, not a close. Counting `closed_trades_total` for
TSMOM would either:

* trivially pass on a high volume of small, noisy weight nudges that carry little
  independent information, or
* fail a structurally sound result simply because the persistent trenders in the
  book (TLT, GLD -- see `docs/TSMOM-RESULT.md`'s per-asset-class attribution) rarely
  flip sign, so they rarely produce a `ClosedTrade` at all.

TSMOM's real unit of decision is the **monthly formation**: once a month, the whole
18-name book is re-evaluated and re-weighted together. That's the natural analog of
"an independent bet" for this strategy, and it's what the proposal below counts.

**G2, G4, G5 are unchanged**, restated here for completeness:

* **G2** (infrastructure fault injection) is already strategy-agnostic -- it exercises
  the broker/execution/portfolio/reconciler/watchdog pipeline generically, and TSMOM
  orders flow through the same pipeline as earnings orders. No TSMOM-specific G2
  criteria are needed.
* **G4** (Claude layer) does not apply to TSMOM: by design, TSMOM candidates bypass
  the validator sidecar entirely (`docs/DECISIONS.md`, this phase's shorting/validator/
  gate-machinery decisions). There is nothing for G4's paired counterfactual to measure.
* **G5** (live rollout: 10% of intended capital for 3 months) is mechanically
  unchanged -- it applies at the account level, not per-strategy.

## Proposed G1 for TSMOM (backtest)

Reuse the four checks TSMOM-v1 already passed in Python, unchanged, rather than
re-litigating whether the strategy works -- that was decided by
`docs/prereg/TSMOM-v1.md`'s pass rule and `docs/TSMOM-RESULT.md`'s result. The C++
port's job is to prove it reproduces that result, not to re-run the experiment:

| Check | Bar | Source |
|---|---|---|
| Deflated Sharpe (`deflated_sharpe_annual`) | > 0 | `at_dsr` CLI (`cpp/src/backtester/metrics.cpp`), already asset-agnostic |
| OOS folds net positive | at least 2 of 3, rounded up | same purge/embargo fold geometry as the Python `walkforward.py` |
| Sub-period stability | net return positive in all three thirds of the concatenated OOS series | same as the pre-registration |
| Minimum sample | at least 4 OOS fold-years | same as the pre-registration |
| **New: C++/Python parity** | the C++ backtester harness's gross daily-return series matches the Python gold-standard series within documented tolerance | `cpp/tests/test_tsmom_backtest.cpp`, **implemented and passing** (600/600 compared days over a ~2.5-year window of the development panel; see `docs/DECISIONS.md`'s Phase 2 entry) |

Replace the round-trip trade-count bar with a **formation-month count**:

> **Proposed floor: `N ≥ 48` formation months (4 years) with at least one non-zero
> target weight**, chosen as a floor well below the ~108 formation months already
> available in the ~9-year development panel, mirroring the spirit of the earnings
> strategy's own minimum-sample checks without pretending to a false precision this
> phase hasn't earned. **This number is a proposal, not a decision** -- an owner
> with a view on how many independent monthly rebalances constitute a fair sample
> for a CTA-style book should feel free to move it.

## Proposed G3 for TSMOM (paper)

Replace `≥ 150 closed trades` with:

> **`≥ 12 formation months observed live`** (one full year of monthly rebalances
> across the 18-name book, so every asset class has cycled through at least a few
> formation events), **plus** positive net return and realized drawdown within
> limits -- both unchanged, already strategy-agnostic checks.

Twelve months mirrors `docs/DESIGN.md` §7.4's existing "twelve to eighteen months"
G3 duration guidance for the earnings strategy, so this isn't a new duration
philosophy for the project, just the same duration expressed in the unit that
actually applies to a monthly-rebalanced book.

## What this doc does not do

It does not change `docs/TESTING.md`'s G1/G3 text for the earnings strategy, which
remains accurate and in use. It does not itself run any check -- it proposes what
`at_tests`'s existing C++ parity tests, the (not-yet-built) fold/DSR harness, and a
live paper deployment's formation-month count should be judged against, once an
owner sets the open `N` floors above. See `docs/DECISIONS.md`'s Phase 2 entry for
what's actually been built vs. still open.
