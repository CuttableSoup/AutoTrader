# Gate G1 result: FAIL

Run 2026-09-15 on the full Sharadar 10-year universe. Reproduce with:

```
at-universe backtest --config config/paper.json --out data/sharadar --start 2017-01-03 --bulk --years 10
cpp/build/.../at_backtester --config config/backtest.sharadar.json
```

## Data

| | |
|---|---|
| Universe | every US common stock that ever exceeded the $5B market-cap floor, 2016-09 to 2026-09 |
| Symbols | 2,168, delisted names included (survivorship-free) |
| Bars | 4.4M daily, split-adjusted |
| Earnings events | 73,147, point-in-time by SEC filing date |
| Benchmark | SPY from the Sharadar `funds` table |
| Not enforced | analyst coverage and median spread (no vendor supplies them; logged on every build) |

## Verdict

**FAIL**, on three of the four conditions.

| Condition | Required | Actual |
|---|---|---|
| Deflated Sharpe | > 0 | **−1.77** |
| Folds with positive net return | ≥ 2 of 3 | **2 of 7** |
| Gross Sharpe after 50% haircut | ≥ 0.4 | **−0.07** |
| Out-of-sample trades | ≥ 30 | 182 ✓ |

Out-of-sample across seven rolling folds (2y train, 1y test, 40-session purge, 10-session embargo): 1,681 sessions, 182 trades, net return −5.0%, Sharpe −0.18, max drawdown 10.0%, win rate 33%, profit factor 0.72. Deflated Sharpe is penalised for all 84 configurations tried.

| Fold | Test window | EAR thr | Mom top% | Sharpe | Return | Trades |
|---|---|---|---|---|---|---|
| 0 | 2019-10 → 2020-10 | 2 | 40 | −1.17 | −2.83% | 21 |
| 1 | 2020-10 → 2021-10 | 2 | 30 | −0.05 | −0.16% | 19 |
| 2 | 2021-10 → 2022-10 | 2 | 50 | −1.70 | −4.62% | 14 |
| 3 | 2022-10 → 2023-10 | 2 | 40 | +0.31 | +1.00% | 33 |
| 4 | 2023-10 → 2024-10 | 2 | 30 | +0.94 | +4.59% | 32 |
| 5 | 2024-10 → 2025-10 | 3 | 50 | −0.23 | −1.42% | 47 |
| 6 | 2025-10 → 2026-09 | 3 | 50 | −0.38 | −1.40% | 16 |

The original three folds tested only 2019–2022 and left four purchased years unused, so the run was widened to seven folds. The extra folds did not rescue it: the two positive years sit in the middle of five negative ones, which is what noise looks like.

## The signal has no edge before costs

The decisive number. Taking all 519 candidates the strategy generated and measuring the raw 40-session forward return from the next open, with no stops, no costs, no sizing and no risk caps:

| | |
|---|---|
| Mean forward return | +0.58% (t = +0.82) |
| Median | −0.07% |
| Win rate | 49.3% |
| **Excess over SPY, same windows** | **−0.80% (t = −1.19)** |

Candidates underperform simply holding the index over the holding period, and the raw drift is statistically indistinguishable from zero. **This is not an execution problem.** No adjustment to the stop, the cost model, the vol target or the entry timing recovers a drift that is not there. Tuning any of them would be fitting noise.

Two corroborating signs of the same thing: the walk-forward tuner chose the *lowest* EAR threshold in the grid in five of seven folds, which is what happens when the parameter carries no information and a lower bar merely buys more trades; and realised book volatility came in at 3.7% against a 10–12% target, because too few names ever qualify to deploy the capital.

## What this means

Per §7.2, v1.0 does not proceed to paper trading, and §2.3 blocks the v2 momentum sleeve behind a v1 pass, so that is on hold too. The gate did its job: this cost a $49 data subscription and an afternoon instead of 12–18 months of paper trading and live capital.

This is the outcome §13 warned about. Post-publication decay of roughly 58% (McLean-Pontiff) applied to an effect that was modest to begin with leaves nothing, and the note that text-based earnings-surprise evidence is recent and may decay the way SUE-PEAD did now looks like the right read.

The infrastructure is unaffected and independently validated: Gate G2 passes all five fault-injection scenarios, the Claude validator works end to end against the live API, and the backtester correctly refused a strategy that does not work. What needs replacing is the hypothesis in §2.2, not the machinery.

## If the hypothesis is revisited

Do not re-tune the seven frozen parameters against this dataset. Every such pass raises the DSR trial count against the same data, and the deflated Sharpe already prices in 84 configurations. A new idea deserves a fresh specification frozen before it meets the data, exactly as v1.0 was.

Cheap tests worth running before writing another full strategy, each answerable from the dataset already on disk:

* Does *any* horizon show drift after these earnings reactions, or is 40 sessions simply the wrong window? Measure forward excess return at 5, 10, 20, 60 and 120 sessions.
* Is the abnormal return the wrong trigger? Sort the same events by standardised unexpected earnings, by revenue surprise, and by announcement-day volume alone, and check whether any sort produces a monotone forward return spread.
* Is the effect alive only below the $5B floor? The design excluded small caps deliberately, and Milian-style overreaction argues the effect concentrates where attention is scarcer. The dataset already contains those names.
