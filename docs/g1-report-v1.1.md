# Backtest report: sharadar-g1-v11

Strategy `EARNINGS_MOMENTUM_V1.1` (params fingerprint `5862c0fb2adc9826`), data `/cygdrive/c/Users/Administrator/Projects/AutoTrader/data/sharadar`, 2017-10-02 to 2026-09-15. Validator mode: mock.

## Gate G1: FAIL

* deflated Sharpe <= 0
* net return positive in only 3/7 folds
* gross Sharpe after haircut 0.168606 < 0.400000

## Out-of-sample (concatenated test folds)

| Metric | Value |
|---|---|
| Sessions | 1681 |
| Trades | 315 |
| Net return | 11.2042% |
| CAGR | 1.60477% |
| Sharpe (net) | 0.296302 |
| Sharpe (gross) | 0.337212 |
| Gross Sharpe after 50% haircut | 0.168606 |
| Deflated Sharpe (annual) | -1.14836 |
| DSR probability | 0.00155095 |
| PSR | 0.777295 |
| Configurations tried | 84 |
| Max drawdown | 11.0819% |
| Win rate | 38.7302% |
| Profit factor | 1.07137 |
| Avg holding (sessions) | 19.4762 |
| Total costs | $14815.6 |

## Folds

| Fold | Train | Test | EAR thr | Mom top% | OOS Sharpe net | OOS return | Trades |
|---|---|---|---|---|---|---|---|
| 0 | 2017-10-02...2019-08-06 | 2019-10-16...2020-10-02 | 2 | 30 | -0.566645 | -2.23801% | 29 |
| 1 | 2018-10-02...2020-08-06 | 2020-10-16...2021-10-02 | 2 | 50 | 1.50331 | 11.162% | 62 |
| 2 | 2019-10-02...2021-08-05 | 2021-10-18...2022-10-02 | 2 | 30 | -1.52875 | -4.57568% | 17 |
| 3 | 2020-10-02...2022-08-04 | 2022-10-17...2023-10-02 | 2 | 50 | -0.284653 | -1.82653% | 73 |
| 4 | 2021-10-02...2023-08-04 | 2023-10-16...2024-10-02 | 2 | 30 | 1.24634 | 9.81546% | 53 |
| 5 | 2022-10-02...2024-08-06 | 2024-10-16...2025-10-02 | 2 | 30 | 0.118593 | 0.562614% | 65 |
| 6 | 2023-10-02...2025-08-06 | 2025-10-16...2026-09-15 | 2 | 30 | -0.290863 | -1.08964% | 16 |

## Validator counterfactual (mock)

```
{
  "diff_pct": 1.60758909951003,
  "mean_fwd_approved_pct": 4.080811708105248,
  "mean_fwd_vetoed_pct": 2.473222608595218,
  "n_approved": 475,
  "n_vetoed": 365,
  "t_stat": 1.0688500533924943,
  "verdict": "no measurable lift"
}
```

Mock rules COUNTER_GAP and SECOND_8K_IN_WINDOW use post-signal data (lookahead proxies). Their lift is an upper bound on what a real validator could add, never evidence of edge.

Haircut and DSR are reported so nobody reasons about live returns from raw backtest numbers (docs/DESIGN.md 7.1, 13).
