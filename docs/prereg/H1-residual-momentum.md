# H1: residual (idiosyncratic) momentum

Implementation: `python/autotrader/research/h1_resid_mom.py`. Shared rules: [README.md](README.md).

## Hypothesis

Stocks with high momentum in their *firm-specific* returns, after removing exposure to the market, size and value factors, outperform over the following one to six months. Stripping factor exposure avoids the momentum crashes that come from betting on a factor reversal (Blitz, Huij & Martens 2011; Blitz, Hanauer & Vidojevic 2017).

Prediction: the top residual-momentum decile beats SPY at 60 sessions, with a monotone decile ladder, in at least one size band.

## Specification

| | |
|---|---|
| Formation | Last session of every calendar month from 2018-09 (the first month with 24 months of returns in the panel) to the last complete month before the seal |
| Monthly returns | Compounded daily total returns per calendar month; a stock-month with fewer than 15 sessions is missing |
| Factors | Fama-French 3 daily from the Ken French library (`data/factors/`), compounded to months; stock returns in excess of the factor file's risk-free rate |
| Regression | OLS with intercept of monthly excess return on Mkt-RF, SMB, HML over the 36 months ending with the formation month m; needs ≥ 24 valid months |
| Score | Sum of the regression residuals over months m−11 … m−1, divided by their sample standard deviation; needs ≥ 8 of the 11. Month m is skipped. |
| Scored population | Every name in the universe at the formation close; each band then sorts its own members |
| Buckets | Deciles (10) |
| Horizons | **60 (primary)**, 20, 120 sessions |
| Bands | 5B+, 2B-5B, 500M-2B, all |

## Reference arm (not gating)

Vanilla 12-1 momentum, `close_tr[f−21] / close_tr[f−252] − 1`, with the same buckets, horizons and bands. It is here so the result says whether the residual construction adds anything; it cannot pass or fail H1.

## Trials

2 arms × 4 bands × 3 horizons = 24 configurations.

## Known limitations, accepted in advance

* Only about seven years of formations (2018-09 to 2025-08) because the panel starts in 2016-09. With overlapping 60-session holds, t > 2 needs a strong effect.
* The factor file comes from a vendor with a one-to-two-month publication lag. That is irrelevant historically, and the skipped month absorbs it live.
* Delisting returns are not in the data. Delisted losers are held at their last close, which slightly flatters the bottom decile, not the top.
