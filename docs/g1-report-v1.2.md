# Backtest report: sharadar-g1-v12

Strategy `EARNINGS_MOMENTUM_V1.2` (params fingerprint `389e1cd30e43cecf`), data `/cygdrive/c/Users/Administrator/Projects/AutoTrader/data/sharadar`, 2017-10-02 to 2026-09-15. Validator mode: mock.

## Gate G1: FAIL

* deflated Sharpe <= 0
* net return positive in only 3/7 folds
* gross Sharpe after haircut 0.151048 < 0.400000

## Out-of-sample (concatenated test folds)

| Metric | Value |
|---|---|
| Sessions | 1681 |
| Trades | 352 |
| Net return | 9.10497% |
| CAGR | 1.3149% |
| Sharpe (net) | 0.271664 |
| Sharpe (gross) | 0.302097 |
| Gross Sharpe after 50% haircut | 0.151048 |
| Deflated Sharpe (annual) | -1.03261 |
| DSR probability | 0.00394944 |
| PSR | 0.757676 |
| Configurations tried | 84 |
| Max drawdown | 8.71436% |
| Win rate | 45.4545% |
| Profit factor | 1.16531 |
| Avg holding (sessions) | 29.0682 |
| Total costs | $9661.5 |

## Folds

| Fold | Train | Test | EAR thr | Mom top% | OOS Sharpe net | OOS return | Trades |
|---|---|---|---|---|---|---|---|
| 0 | 2017-10-02...2019-08-06 | 2019-10-16...2020-10-02 | 2 | 40 | -0.794037 | -3.48073% | 44 |
| 1 | 2018-10-02...2020-08-06 | 2020-10-16...2021-10-02 | 2 | 50 | 1.41664 | 8.1443% | 50 |
| 2 | 2019-10-02...2021-08-05 | 2021-10-18...2022-10-02 | 2 | 30 | -1.32865 | -4.26002% | 17 |
| 3 | 2020-10-02...2022-08-04 | 2022-10-17...2023-10-02 | 2 | 40 | -0.0882058 | -0.510353% | 60 |
| 4 | 2021-10-02...2023-08-04 | 2023-10-16...2024-10-02 | 2 | 40 | 1.26522 | 7.72779% | 55 |
| 5 | 2022-10-02...2024-08-06 | 2024-10-16...2025-10-02 | 2 | 30 | -0.216408 | -1.45614% | 65 |
| 6 | 2023-10-02...2025-08-06 | 2025-10-16...2026-09-15 | 2 | 30 | 0.665779 | 3.37091% | 61 |

## Validator counterfactual (mock)

```
{
  "diff_pct": 1.443688672431691,
  "mean_fwd_approved_pct": 4.032392385047487,
  "mean_fwd_vetoed_pct": 2.588703712615796,
  "n_approved": 496,
  "n_vetoed": 384,
  "t_stat": 0.9948011050325376,
  "verdict": "no measurable lift"
}
```

Mock rules COUNTER_GAP and SECOND_8K_IN_WINDOW use post-signal data (lookahead proxies). Their lift is an upper bound on what a real validator could add, never evidence of edge.

Haircut and DSR are reported so nobody reasons about live returns from raw backtest numbers (docs/DESIGN.md 7.1, 13).
