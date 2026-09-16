# Where the project stands, and what to do next

Written 2026-09-16, after Gate G1 was run for real.

## In one paragraph

The plumbing works. The strategy does not, yet. The original strategy looked for
big jumps after earnings and bought them, but only in companies worth more than
$5 billion. Measured over ten years of real data, that jump-and-drift effect is
absent in companies that large. It is strong in companies worth $1–5 billion.
Moving the size floor down turned the backtest from losing money to making some,
but not enough to clear the bar the plan set. The infrastructure is separately
validated and can start paper trading now, as an operations test, while the
strategy question is settled.

## What was measured

Ten years of data: 2,168 companies, 4.4 million daily prices, 73,147 earnings
reports, delisted companies included so the test is not flattered by survivors.

**The effect is real.** Sorting every earnings report by how much the stock moved
relative to the market, then measuring what happened over the following 40 trading
days, gives a clean ladder. Bigger reaction, bigger subsequent drift:

| Reaction to earnings | Next 40 days vs market | Events |
|---|---|---|
| below −5% | −0.82% | 7,050 |
| −2% to +2% | −0.30% | 35,145 |
| +2% to +5% | +1.34% | 10,067 |
| +5% to +10% | +2.97% | 4,355 |
| above +10% | **+7.29%** | 2,321 |

**But it is entirely in smaller companies.** Same trigger, split by company size:

| Company size | Next 40 days vs market | Events | Real? |
|---|---|---|---|
| under $2B | +4.89% to +10.70% | 987 | yes, strongly |
| $2–5B | +2.10% to +3.37% | 1,225 | yes |
| **$5–50B** | **−0.30% to +0.92%** | 1,630 | **no** |
| **above $50B** | **−0.96% to +0.02%** | 210 | **no** |

The original plan traded only the bottom two rows, where nothing happens. This is
not a surprise in hindsight: DESIGN.md §2.4 cites Martineau (2022) for exactly this
finding, that the effect died in large caps around 2006, and then §2.1 set a $5B
floor anyway. The universe rule contradicted the paper it was based on.

## What was tried

| Version | Change | Trades | Return | Sharpe | After haircut | Folds up |
|---|---|---|---|---|---|---|
| v1.0 | as designed | 182 | −5.0% | −0.18 | −0.07 | 2/7 |
| v1.1 | size floor $5B → $1B, liquidity floor $50M → $10M/day | 315 | +11.2% | +0.30 | +0.17 | 3/7 |
| v1.2 | v1.1 plus a wider stop (3×ATR → 5×ATR) | 352 | +9.1% | +0.27 | +0.15 | 3/7 |

To pass, the gate needs a haircut Sharpe of 0.40 and a majority of folds positive.

Two things worth recording because they cost real time:

* **The risk limits were written for large companies too.** With the size floor
  lowered, the 10 basis point spread guard silently rejected 639 of 1,169
  candidates and the strategy made zero trades. It had to move to 30bp to match
  the kind of company now being traded. Whatever spread the backtest assumes and
  whatever the live risk manager permits must be the same number, or the backtest
  trades things the live system would refuse.
* **The wider stop did not help.** Stated as a prediction before testing, and
  wrong: win rate and drawdown improved, Sharpe did not. So the gap between the
  raw effect and the tradeable result is not mainly the stop.

## Why the gap between "+2% per trade" and "Sharpe 0.30"

The raw signal in v1.1's universe is worth about +2% over 40 days with a
t-statistic of 4.5 across 1,668 events, stable in 2017–2020, 2020–2023 and
2023–2026. Converting that into a portfolio loses most of it:

* **Costs.** These companies do not trade at a 4bp spread. At an honest 25bp plus
  commission, a round trip costs roughly 35bp against a 200bp expected move.
* **The book is never full.** Realised volatility came in at 3.7% against a
  10–12% target, because at most 15 positions and 3 new per day, with candidates
  arriving in earnings-season bursts, leaves the account mostly in cash. Note that
  leverage does not fix this: it scales returns and volatility together and leaves
  Sharpe where it is.
* **Timing.** The event study buys at the next open and holds 40 sessions
  unconditionally. The strategy also exits on the earnings gate and the trend
  filter, which cut some winners short.

## Recommended next steps, in order

### 1. Start the paper trader now, as an operations test (this week)

This does not need a passing strategy and should not wait for one. Gate G2 passed
all five fault-injection scenarios against a simulated broker, but that is not the
same as surviving real markets: real fills, real feed gaps, overnight restarts,
weekend reconnects, month-end universe rebuilds, the watchdog firing at 3am.

Run it with `config/strategy.v1.1.json`, in shadow validator mode, at small size.
Be explicit that **this is not Gate G3** and that the numbers it produces are not
evidence about the strategy. What to watch: does reconciliation stay clean for
weeks, does the watchdog drill pass every week, does the validator error streak
stay at zero, do any orders get rejected.

This also starts Gate G4 accruing, which is the one thing that cannot be rushed
later: the Claude validator has to label candidates forward in real time, because
it reads the live web and cannot be replayed over history.

### 2. Decide the strategy question separately

The honest position is that this data has now been searched hard. Three
specifications and 84 configurations per run have been tried against the same ten
years. Anything further fitted to it is not evidence.

Two defensible paths:

**A. Accept and stop.** The plan set a deliberately conservative bar and v1.2 does
not clear it. A gross Sharpe near 0.3 before the haircut is thin for something
meant to run unattended.

**B. One clean re-specification, tested once on data never used.** Hold out
2025-09 onward completely. Develop on 2016–2025 only. The event study points at a
simpler idea than the current one: the reaction size does nearly all the work,
the momentum and volume filters may be subtracting rather than adding, and the
drift persists out to 120 days rather than stopping at 40. Write that down, freeze
it, then run it once against the held-out period. If it fails there, stop for real.

My view: B is worth one attempt, on the condition that the hold-out is genuinely
untouched until the end. If that is not going to be respected, A is the better
choice, because a strategy tuned until it passes will fail with real money instead.

### 3. Things worth fixing regardless

* The ex-dividend rule in §3.3 is parsed from config but never evaluated; there is
  no dividend feed wired in. It is a stub, not a working rule.
* Nobody in the current vendor set sells quoted spreads, so the median-spread
  universe rule cannot be enforced and is currently reported as unenforced. Given
  that spread assumptions now drive the result, this is worth sourcing properly.
* The walk-forward defaulted to 3 folds, which tested only 2019–2022 and left four
  years unused. It is now 7. Any future run should confirm the folds actually span
  the data.
