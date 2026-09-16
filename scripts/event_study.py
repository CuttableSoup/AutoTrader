#!/usr/bin/env python
"""Event study over the Sharadar dataset: does ANY version of the earnings-reaction
idea show forward drift?

Gate G1 failed because the v1.0 signal has no drift to execute on. Before writing
another strategy it is worth knowing whether the underlying effect exists at all in
this data, in some other form. This measures that directly, with no trading logic,
no costs and no risk limits in the way.

For every earnings event it computes the announcement reaction, then the forward
return over several horizons, always as EXCESS over SPY across the identical
window, and reports the mean and t-statistic per bucket.

READ THE OUTPUT AS EXPLORATORY. It sweeps many cuts of the same data, so the most
extreme cell is expected to look good by luck alone. A cell is only interesting if
it is large, monotone across neighbouring buckets, and stable across sub-periods.
Anything acted on must then be frozen and re-tested on data this sweep never saw.

usage: python scripts/event_study.py --data data/sharadar
"""
from __future__ import annotations

import argparse
import bisect
import csv
import datetime as dt
import math
import statistics as st
from collections import defaultdict
from pathlib import Path

HORIZONS = [5, 10, 20, 40, 60, 120]


def load_bars(path: Path):
    """symbol -> (dates[], opens[], closes[], volumes[]) sorted by date."""
    tmp = defaultdict(list)
    with open(path, newline="", encoding="utf-8") as f:
        rd = csv.reader(f)
        next(rd)
        for sym, d, o, h, l, c, v in rd:
            tmp[sym].append((d, float(o), float(c), float(v)))
    out = {}
    for sym, rows in tmp.items():
        rows.sort()
        out[sym] = ([r[0] for r in rows], [r[1] for r in rows], [r[2] for r in rows], [r[3] for r in rows])
    return out


def load_caps(path: Path):
    """symbol -> sorted [(as_of, market_cap)] for point-in-time size buckets."""
    caps = defaultdict(list)
    with open(path, newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            try:
                caps[r["symbol"]].append((r["as_of"], float(r["market_cap"])))
            except (ValueError, KeyError):
                pass
    for v in caps.values():
        v.sort()
    return caps


def cap_asof(caps, sym, date):
    rows = caps.get(sym)
    if not rows:
        return None
    i = bisect.bisect_right([r[0] for r in rows], date) - 1
    return rows[i][1] if i >= 0 else None


def tstat(xs):
    if len(xs) < 3:
        return 0.0
    s = st.stdev(xs)
    return 0.0 if s == 0 else st.fmean(xs) / (s / math.sqrt(len(xs)))


def summarize(label, xs, n_min=30):
    if len(xs) < n_min:
        return f"  {label:<26} n={len(xs):<5} (too few)"
    return f"  {label:<26} n={len(xs):<5} mean {st.fmean(xs):+7.3f}%  t={tstat(xs):+6.2f}"


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default="data/sharadar")
    ap.add_argument("--benchmark", default="SPY")
    a = ap.parse_args()
    data = Path(a.data)

    bars = load_bars(data / "bars.csv")
    caps = load_caps(data / "securities.csv")
    bench = bars.get(a.benchmark)
    if not bench:
        raise SystemExit(f"benchmark {a.benchmark} missing from bars.csv")
    bdates, bopens, bcloses, _ = bench
    bidx = {d: i for i, d in enumerate(bdates)}

    events = []
    with open(data / "earnings.csv", newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            events.append(r)
    print(f"loaded {len(bars)} symbols, {len(events)} earnings events\n")

    # ---- build one record per event -------------------------------------------------
    recs = []
    for e in events:
        sym, rd = e["symbol"], e["report_date"]
        b = bars.get(sym)
        if not b:
            continue
        dates, opens, closes, vols = b
        # day 0 = first session strictly after the filing date (timing is UNKNOWN in this data)
        i0 = bisect.bisect_right(dates, rd)
        if i0 < 25 or i0 + 1 >= len(dates):
            continue
        j0 = bidx.get(dates[i0])
        if j0 is None or j0 < 1 or j0 + 1 >= len(bdates):
            continue
        # announcement reaction: close(day0-1) -> close(day0+1), minus the benchmark
        stock = closes[i0 + 1] / closes[i0 - 1] - 1
        mkt = bcloses[j0 + 1] / bcloses[j0 - 1] - 1
        ear = (stock - mkt) * 100
        adv20 = st.fmean(vols[i0 - 20:i0]) if i0 >= 20 else 0
        vol_ratio = vols[i0] / adv20 if adv20 > 0 else 0
        mom = (closes[i0 - 21] / closes[i0 - 252] - 1) * 100 if i0 >= 252 and closes[i0 - 252] > 0 else None
        eps = float(e["eps_actual"]) if e.get("eps_actual") else None
        # forward excess returns from the next open
        fwd = {}
        for h in HORIZONS:
            if i0 + 1 + h < len(dates) and j0 + 1 + h < len(bdates):
                s = closes[i0 + 1 + h] / opens[i0 + 1] - 1
                m = bcloses[j0 + 1 + h] / bopens[j0 + 1] - 1
                fwd[h] = (s - m) * 100
        recs.append({"sym": sym, "date": rd, "ear": ear, "vol_ratio": vol_ratio, "mom": mom,
                     "eps": eps, "cap": cap_asof(caps, sym, rd), "fwd": fwd})
    print(f"usable events: {len(recs)}\n")

    def bucket_report(title, keyfn, buckets, filt=None):
        print(f"=== {title}  (excess over {a.benchmark}, %)")
        rows = [r for r in recs if (filt is None or filt(r))]
        print(f"    {'bucket':<26}" + "".join(f"{h:>10}d" for h in HORIZONS))
        for name, lo, hi in buckets:
            sel = [r for r in rows if keyfn(r) is not None and lo <= keyfn(r) < hi]
            line = f"    {name:<26}"
            for h in HORIZONS:
                xs = [r["fwd"][h] for r in sel if h in r["fwd"]]
                line += f"{st.fmean(xs):+7.2f}({tstat(xs):+.1f})" if len(xs) >= 30 else f"{'-':>11}"
            print(line + f"   n={len(sel)}")
        print()

    # 1. the v1.0 trigger: abnormal announcement return
    bucket_report("SORT BY ANNOUNCEMENT REACTION (EAR)", lambda r: r["ear"],
                  [("< -5%", -1e9, -5), ("-5% to -2%", -5, -2), ("-2% to +2%", -2, 2),
                   ("+2% to +5%", 2, 5), ("+5% to +10%", 5, 10), ("> +10%", 10, 1e9)])

    # 2. v1.0's actual gate: big positive reaction AND heavy volume
    bucket_report("EAR > +3% AND VOLUME >= 2x ADV", lambda r: r["ear"],
                  [("+3% to +6%", 3, 6), ("+6% to +10%", 6, 10), ("> +10%", 10, 1e9)],
                  filt=lambda r: r["vol_ratio"] >= 2.0)

    # 3. volume alone
    bucket_report("SORT BY ANNOUNCEMENT VOLUME", lambda r: r["vol_ratio"],
                  [("< 1x ADV", 0, 1), ("1-2x", 1, 2), ("2-4x", 2, 4), ("> 4x", 4, 1e9)])

    # 4. momentum alone
    bucket_report("SORT BY 12-1 MOMENTUM", lambda r: r["mom"],
                  [("< -20%", -1e9, -20), ("-20% to 0%", -20, 0), ("0% to +20%", 0, 20),
                   ("+20% to +50%", 20, 50), ("> +50%", 50, 1e9)])

    # 5. does the effect live below the $5B floor the design excluded?
    for label, lo, hi in [("MICRO/SMALL (<$2B)", 0, 2e9), ("MID ($2-5B)", 2e9, 5e9),
                          ("LARGE ($5-50B)", 5e9, 5e10), ("MEGA (>$50B)", 5e10, 1e15)]:
        bucket_report(f"EAR > +3%, VOL >= 2x, SIZE {label}", lambda r: r["ear"],
                      [("+3% to +8%", 3, 8), ("> +8%", 8, 1e9)],
                      filt=lambda r, lo=lo, hi=hi: r["vol_ratio"] >= 2.0 and r["cap"] is not None and lo <= r["cap"] < hi)

    # 6. sanity: the exact v1.0 combination, split by era
    print("=== THE v1.0 COMBINATION (EAR>+3%, vol>=2x, momentum>0), BY PERIOD")
    sel = [r for r in recs if r["ear"] > 3 and r["vol_ratio"] >= 2 and (r["mom"] or -1) > 0]
    for lo, hi in [("2017-01-01", "2020-01-01"), ("2020-01-01", "2023-01-01"), ("2023-01-01", "2027-01-01")]:
        xs = [r["fwd"][40] for r in sel if lo <= r["date"] < hi and 40 in r["fwd"]]
        print(summarize(f"{lo[:4]}-{hi[:4]} @40d", xs))
    xs = [r["fwd"][40] for r in sel if 40 in r["fwd"]]
    print(summarize("ALL @40d", xs))
    print(f"\n{len(sel)} events matched the v1.0 combination out of {len(recs)}")


if __name__ == "__main__":
    main()
