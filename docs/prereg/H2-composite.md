# H2: long-only composite of momentum, profitability and net issuance

Implementation: `python/autotrader/research/h2_composite.py`. Shared rules: [README.md](README.md).

## Hypothesis

Three individually weak, economically distinct and weakly correlated signals, combined with fixed equal weights, rank stocks better than any one of them: price momentum, gross profitability (Novy-Marx 2013) and net share issuance (Pontiff & Woodgate 2008; buybacks score high). Diversifying across weak signals is the most robust result in the replication literature (Jensen, Kelly & Pedersen 2023).

Prediction: the top composite quintile beats SPY at 60 sessions, monotone across quintiles, in at least one size band.

## Specification

| | |
|---|---|
| Formation | Last session of every calendar month from 2017-09 (first month with 252 sessions of prices) to the last complete month before the seal |
| `mom` | `close_tr[f−21] / close_tr[f−252] − 1` |
| `gpa` | SF1 **ART** `gp` (trailing twelve months) ÷ SF1 **ARQ** `assets`, both as known at f; missing if assets ≤ 0 |
| `iss` | `−ln(shares[f] / shares[f−252])`, shares = SF1 ARQ `sharesbas` × `sharefactor` as known at each date (already restated for splits) |
| Standardisation | Within the band being sorted, at each formation: winsorise each signal at its 1st and 99th percentiles, then z-score (sample sd) |
| Composite | Mean of the available z-scores, **fixed equal weights**, needs at least 2 of 3. Weights are never estimated or tuned. |
| Buckets | Quintiles (5) |
| Horizons | **60 (primary)**, 20, 120 sessions |
| Bands | 5B+, 2B-5B, 500M-2B, all |

## Diversification check (reported, not gating)

Each single signal is sorted the same way. The report states, per band at 60 sessions, whether the composite's top quintile beats every single signal's top quintile. If H2 passes but the composite does not beat its best component, Stage 2 uses the composite anyway (choosing the best single component after seeing this table would be a selection on the data); the result is recorded as weak support for the diversification rationale.

## Trials

4 arms (composite + 3 singles) × 4 bands × 3 horizons = 48 configurations.

## Known limitations, accepted in advance

* Banks and insurers often have no meaningful `gp`. They are scored on the other two signals rather than excluded.
* The single momentum arm is the same 12-1 construction as H1's reference arm, so that cell is measured twice. It is counted twice.
