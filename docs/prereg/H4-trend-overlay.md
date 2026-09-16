# H4: market trend overlay (long or flat)

Implementation: `python/autotrader/research/h4_trend.py`. Shared rules: [README.md](README.md), except that H4 has its own pass rule because it is a risk governor, not a return predictor.

## Hypothesis

Stepping out of equities while SPY is below its long moving average cuts the drawdown of a long-only stock book by at least a quarter, at a cost of no more than 0.10 in annual Sharpe ratio (Moskowitz, Ooi & Pedersen 2012 for time-series momentum; the single-market long/flat version is weaker and regime-dependent, and that is what is being tested).

## Specification

| | |
|---|---|
| Books | Band `all`, monthly formation at the last session of each month from 2018-09 to the last complete month before the seal. `universe_ew`: every eligible name. `h1_top_decile`: top decile of H1's residual momentum score. `h2_top_quint`: top quintile of H2's composite score. |
| Holding | Members chosen at the close of f are held from the close of f+1 to the close of the next formation's f'+1 |
| Daily book return | Equal-weight mean of the members' close-to-close total returns (members with no bar that day are skipped) |
| Rule `sma200` (**primary**) | Invested on day d if SPY's total-return close at d−2 is above its 200-session mean at d−2 |
| Rule `sma10m` (secondary) | Invested for the month after formation if SPY's month-end close is above the mean of the last 10 month-end closes (applied from f+2) |
| When flat | Earn the Fama-French daily risk-free rate |
| Sharpe | Annualised mean ÷ sd of daily return in excess of the risk-free rate |
| Max drawdown | Peak-to-trough of the compounded daily series |

## Pass rule, per book, primary rule only

Overlay max drawdown ≤ 75% of the unfiltered book's **and** overlay Sharpe ≥ unfiltered Sharpe − 0.10.

In Stage 2, the overlay is applied only to a promoted book for which it passed here.

## Trials

3 books × 2 rules = 6 configurations.

## Known limitations, accepted in advance

* 2018-09 to 2025-09 holds essentially two sharp drawdowns (Q4 2018, Feb–Mar 2020) and one slow one (2022). A trend rule's value rests on very few episodes, whichever way this comes out.
* Equal-weight daily rebalancing in the book return is an approximation of monthly-rebalanced buy-and-hold weights.
