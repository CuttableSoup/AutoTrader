# H3: earnings drift on the relaxed, bias-free universe

Implementation: `python/autotrader/research/h3_pead.py`. Shared rules: [README.md](README.md).

## Hypothesis

Post-earnings drift is dead in large caps (Martineau 2022) but survives, briefly, where attention is scarce. Sorting announcements by standardised unexpected earnings, the top SUE decile beats SPY over the following two weeks in the $500M–$2B and/or $2–5B bands, and not in $5B+.

This is also a correction exercise. The v1.1 decision to lower the floor rested on `scripts/event_study.py`, which (a) ran on a symbol set chosen for having *later* exceeded $5B and (b) measured the announcement reaction through the close of the session whose open it then used as the entry price. Both inflate small-cap drift. The bias check below measures by how much.

## Specification

| | |
|---|---|
| Events | Every SF1 ARQ filing (one per ticker and calendar quarter, the first filed) for a panel symbol |
| Announcement date | Latest Sharadar event with item 2.02 (code `22`) in the 60 calendar days up to and including the filing date; the filing date if there is none |
| day0 | First session strictly after the announcement date (BMO/AMC timing is not in the data). Formation at the **close of day0**; entry at the open of day0+1. |
| SUE | (EPS_q − EPS_{q−4}) ÷ sample sd of that same difference over the up-to-8 previous quarters, ≥ 4 present; EPS is SF1 `eps` |
| Eligibility | In the band's universe at the formation close |
| Breakpoints | Deciles from all SUE values of the band's events in the **previous four calendar quarters** (fully point-in-time); quarters with < 100 prior events are skipped |
| Time series | Mean excess return per bucket per calendar quarter of formation; ≥ 5 events per cell |
| t-statistic | Newey-West with 1 lag over the quarterly series |
| Horizons | **10 (primary)**, 2, 5, 20 sessions |
| Bands | 5B+, 2B-5B, 500M-2B, all |
| Gating arm | SUE at the announcement date |

## Non-gating arms (reported)

* SUE at the SF1 filing date (the conservative timing: the numbers are certainly public by then)
* Revenue surprise (same construction on SF1 `revenue`) at the announcement date
* Announcement-day volume ÷ mean volume of the previous 20 sessions, at the announcement date

## Bias check (reported, never gating)

The v1 event study's reaction table on the clean panel (reaction = close(i0−1) → close(i0+1) minus SPY, i0 = first session after the SF1 filing date; buckets < −5, −5…−2, −2…+2, +2…+5, +5…+10, > +10 percent; 40-session excess return), computed two ways, for all four bands:

* **as script**: entry at the open of i0+1, as `scripts/event_study.py` did
* **corrected**: entry at the open of i0+2, after the reaction is fully observed

## Trials

4 arms × 4 bands × 4 horizons + 2 timings × 4 bands × 6 reaction buckets = 64 + 48 = 112 configurations.

## Known limitations, accepted in advance

* The EPS and revenue figures come from the 10-Q/10-K, used as of the earlier press-release date. Press releases almost always carry the same numbers; the filing-date arm bounds the damage if not.
* The SUE needs eight quarters of history, and the fundamentals file starts at calendar 2014-Q4, so the sortable sample starts around 2017 and the first quarter with four prior quarters of breakpoints is later still.
* No bid-ask data exists. The 50 bp round trip for $500M–2B is a stress assumption, not a measurement. A pass here justifies buying spread data, not trading.
