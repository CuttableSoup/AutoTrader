# v2 Stage 1 pre-tests: all four hypotheses FAIL

Run 2026-09-16 against the pre-registrations in [prereg/](prereg/README.md), committed at `77accb5` before any test touched the data. Clean point-in-time panel, 2016-09-16 to 2025-09-15; the following year stays sealed. Full tables are in `var/research/H*/20260916T*.md` (gitignored, reproducible with the commands at the end).

## Verdict

| Hypothesis | Primary test | Result |
|---|---|---|
| H1 residual momentum | top decile vs SPY, 60 sessions | **FAIL** in every band |
| H2 momentum + profitability + issuance composite | top quintile vs SPY, 60 sessions | **FAIL** in every band |
| H3 SUE earnings drift, relaxed universe | top decile vs SPY, 10 sessions | **FAIL** in every band |
| H4 trend overlay | drawdown −25% for ≤ 0.10 Sharpe | passes on the H1 book only, which is not promoted, so moot |

**The pre-registered kill criterion is met.** Neither H1 nor H2 passes in `5B+` or `2B-5B`, so no monthly-rebalance sleeve gets built and Stage 2 does not start. The trial ledger now stands at 627 configurations.

## The numbers that decided it

Primary horizon, mean excess return over SPY in percent, Newey-West t in brackets.

| Band | H1 top decile | H1 top − bottom | H2 top quintile | H2 top − bottom | H3 top SUE decile |
|---|---|---|---|---|---|
| 5B+ | −0.34 (−0.44) | +0.92 (+0.98) | −0.02 (−0.03) | +1.96 (+2.15) | +0.04 (+0.16) |
| 2B-5B | +0.12 (+0.09) | +2.91 (+3.68) | −0.32 (−0.36) | +2.62 (+3.96) | +0.19 (+0.42) |
| 500M-2B | +0.41 (+0.17) | +2.91 (+2.14) | −0.16 (−0.09) | +2.95 (+2.45) | −0.66 (−1.38) |
| all | +0.02 (+0.02) | +2.00 (+2.40) | −0.09 (−0.11) | +2.41 (+3.75) | −0.04 (−0.16) |

## What the results say

**1. The momentum and quality rankings work, but not long-only against SPY.** H1 and H2 both produce clean, nearly monotone ladders. H2's quintiles run in perfect order in every band (Spearman 1.00), and its top-minus-bottom spread is significant everywhere. But nearly every bucket, the top one included, trails SPY over 2018–2025. The spread comes from the losers losing, not the winners winning. The memo's warning applies directly: anomaly returns sit mostly in the short leg, and this system is long-only by design.

Part of this is the period. An equal-weight book of the whole universe returned 9.1% a year against SPY's 14.3% total return over the same 2018-10 to 2025-09 window, because a few mega-caps drove the index. "Beat SPY" was the pre-registered bar and it stays the verdict. Whether a different benchmark should apply is a new hypothesis, and would count as new trials (see "Open question").

**2. The small-cap earnings drift was an artefact.** The H3 bias check re-ran the event-study table that justified v1.1, on the clean universe, two ways:

| Reaction bucket (all bands) | n | 40 sessions, script's timing | 40 sessions, entry one session later |
|---|---|---|---|
| > +10% | 2,078 | +3.52% | +0.45% (t +0.89) |
| +5% to +10% | 4,228 | +1.00% | −0.95% |
| +2% to +5% | 10,181 | +0.21% | −0.84% |

`scripts/event_study.py` measured the reaction through the close of session i0+1 but started the forward return at that session's open. So the sort saw part of the return it was then credited with. Moving to the clean universe (no survivor selection, plus its price and liquidity filters) roughly halves the original +7.29%; correcting the timing removes most of what is left. The $500M–2B band shows the same collapse (+4.37% → +0.76%, t 0.95). The C++ backtester times entries correctly, which is consistent with v1.1 improving only modestly. But v1.1 also ran on the selection-biased symbol set, so its +0.30 Sharpe is optimistic too.

H3's own test, on SUE rather than price reaction, finds nothing in any band at 2, 5, 10 or 20 sessions. Smaller companies are, if anything, worse.

**3. The trend overlay is not a free lunch.** On the equal-weight universe it cut the drawdown from 43% to 32% but gave back 0.22 of Sharpe, because it sat out rebounds. It passed only on the momentum book, where it cut the drawdown from 44% to 28% for 0.03 of Sharpe. Seven years with three drawdowns is too few episodes to lean on either way.

## What this means for the plan

* Stage 2 (C++ rebalance sleeve, gated walk-forward, hold-out run) is **not started**. The hold-out year stays sealed and unspent.
* The pre-registered next step after the kill criterion was "the case for buying spread data for the small-cap sleeves". **The results undercut that.** Spread data prices an edge, and H3 found no pre-cost edge below $5B to price. Buying it now would pay to measure the cost of nothing.
* v1.1's premise, that the drift is "strong in companies worth $1–5 billion" (NEXT-STEPS.md), is withdrawn. Its config remains as the operations-test strategy for paper trading, which never depended on it being profitable.

## Open question for the owner

The one idea these results suggest is a **market-neutral or benchmark-relative framing**: the rankings work, the long leg alone does not. There are two ways to use that, and both are new hypotheses:

* **Long top / short bottom.** This conflicts with the design (§1: no shorting), with Alpaca's easy-to-borrow limits, and with the memo's evidence that short-leg returns are eaten by borrow and shorting costs.
* **Long-only judged against an equal-weight benchmark** (or long top quintile hedged with SPY). This changes what "working" means, and a long-only book that trails SPY by less than the market does still earns less than SPY.

Either would be a fresh pre-registration, penalised for the 627 configurations already spent. Deciding whether to pursue it, or to stop the strategy search (NEXT-STEPS.md option A), is a judgement call for the owner, not something to settle by testing more variants.

## Reproduce

```
.venv/Scripts/python scripts/fetch_ff_factors.py        # only if data/factors is missing
at-research pretest H1 --prereg docs/prereg/H1-residual-momentum.md
at-research pretest H2 --prereg docs/prereg/H2-composite.md
at-research pretest H3 --prereg docs/prereg/H3-relaxed-pead.md
at-research pretest H4 --prereg docs/prereg/H4-trend-overlay.md
```

Every rerun appends to `docs/trials.jsonl`.
