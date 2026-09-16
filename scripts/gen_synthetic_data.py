#!/usr/bin/env python
"""Generate a synthetic Sharadar-like dataset to exercise the backtester.

Writes bars.csv, securities.csv, earnings.csv into --out. The data has NO edge
by construction (random walks with planted earnings reactions of random sign),
so a G1 pass on it would be a bug, not a result. Its only purpose is to run the
machinery end to end: universe rules, signal evaluation, mock validator, risk
limits, fills, costs, walk-forward, DSR.

usage: python scripts/gen_synthetic_data.py --out data/synthetic --years 6 --symbols 120 --seed 1
"""
from __future__ import annotations

import argparse
import csv
import datetime as dt
import hashlib
import math
import os
import random

SECTORS = ["Technology", "Healthcare", "Financial Services", "Industrials", "Consumer Cyclical",
           "Consumer Defensive", "Energy", "Utilities", "Communication Services", "Basic Materials"]


def nyse_holidays(year: int) -> set[dt.date]:
    def nth_weekday(month, weekday, n):
        d = dt.date(year, month, 1)
        delta = (weekday - d.weekday()) % 7
        return d + dt.timedelta(days=delta + 7 * (n - 1))

    def last_weekday(month, weekday):
        d = dt.date(year, month + 1, 1) - dt.timedelta(days=1) if month < 12 else dt.date(year, 12, 31)
        while d.weekday() != weekday:
            d -= dt.timedelta(days=1)
        return d

    def observed(d, new_years=False):
        if d.weekday() == 5:
            return d if new_years else d - dt.timedelta(days=1)
        if d.weekday() == 6:
            return d + dt.timedelta(days=1)
        return d

    a = year % 19; b = year // 100; c = year % 100; d_ = b // 4; e = b % 4
    f = (b + 8) // 25; g = (b - f + 1) // 3; h = (19 * a + b - d_ - g + 15) % 30
    i = c // 4; k = c % 4; l = (32 + 2 * e + 2 * i - h - k) % 7; m = (a + 11 * h + 22 * l) // 451
    easter = dt.date(year, (h + l - 7 * m + 114) // 31, ((h + l - 7 * m + 114) % 31) + 1)
    hs = {observed(dt.date(year, 1, 1), True), nth_weekday(1, 0, 3), nth_weekday(2, 0, 3), easter - dt.timedelta(days=2),
          last_weekday(5, 0), observed(dt.date(year, 7, 4)), nth_weekday(9, 0, 1), nth_weekday(11, 3, 4), observed(dt.date(year, 12, 25))}
    if year >= 2022:
        hs.add(observed(dt.date(year, 6, 19)))
    for adhoc in (dt.date(2018, 12, 5), dt.date(2025, 1, 9)):
        if adhoc.year == year:
            hs.add(adhoc)
    return hs


def sessions(start: dt.date, end: dt.date) -> list[dt.date]:
    out, d = [], start
    hol = {}
    while d <= end:
        if d.year not in hol:
            hol[d.year] = nyse_holidays(d.year)
        if d.weekday() < 5 and d not in hol[d.year]:
            out.append(d)
        d += dt.timedelta(days=1)
    return out


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="data/synthetic")
    ap.add_argument("--years", type=int, default=6)
    ap.add_argument("--symbols", type=int, default=120)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--start", default="2019-01-02")
    args = ap.parse_args()
    rng = random.Random(args.seed)
    os.makedirs(args.out, exist_ok=True)

    start = dt.date.fromisoformat(args.start)
    end = dt.date(start.year + args.years, 12, 31)
    days = sessions(start, end)
    n = len(days)

    def walk(px0, vol, drift, base_vol):
        px = px0
        bars = []
        for _ in range(n):
            r = drift + vol * rng.gauss(0, 1)
            o = px * (1 + 0.3 * vol * rng.gauss(0, 1))
            c = px * math.exp(r)
            hi = max(o, c) * (1 + abs(0.5 * vol * rng.gauss(0, 1)))
            lo = min(o, c) * (1 - abs(0.5 * vol * rng.gauss(0, 1)))
            v = int(base_vol * (1 + 0.25 * abs(rng.gauss(0, 1))))
            bars.append([o, hi, lo, c, v])
            px = c
        return bars

    bars_path = os.path.join(args.out, "bars.csv")
    sec_path = os.path.join(args.out, "securities.csv")
    earn_path = os.path.join(args.out, "earnings.csv")
    with open(bars_path, "w", newline="") as fb, open(sec_path, "w", newline="") as fs, open(earn_path, "w", newline="") as fe:
        wb = csv.writer(fb); ws = csv.writer(fs); we = csv.writer(fe)
        wb.writerow(["symbol", "date", "open", "high", "low", "close", "volume"])
        ws.writerow(["symbol", "name", "sector", "category", "market_cap", "first_listed", "analyst_coverage", "transcript_available", "median_spread_bps", "as_of"])
        we.writerow(["event_id", "symbol", "report_date", "timing", "fiscal_period", "eps_actual", "eps_consensus", "eps_consensus_asof", "revenue_actual", "revenue_consensus", "next_report_date", "material_8k_dates"])

        spy = walk(300.0, 0.010, 0.0003, 80_000_000)
        for d, b in zip(days, spy):
            wb.writerow(["SPY", d.isoformat(), f"{b[0]:.2f}", f"{b[1]:.2f}", f"{b[2]:.2f}", f"{b[3]:.2f}", b[4]])
        ws.writerow(["SPY", "SPDR S&P 500", "", "ETF", "500000000000", "1993-01-29", 0, 0, 0.5, start.isoformat()])

        for i in range(args.symbols):
            sym = f"SY{i:03d}"
            sector = SECTORS[i % len(SECTORS)]
            vol = rng.uniform(0.012, 0.03)
            drift = rng.uniform(-0.0003, 0.0008)
            base_vol = int(rng.uniform(800_000, 6_000_000))
            bars = walk(rng.uniform(20, 300), vol, drift, base_vol)
            # Quarterly earnings with a random-sign planted reaction and random BMO/AMC.
            q_offset = rng.randrange(0, 63)
            idx = 60 + q_offset
            fiscal_q = 0
            events = []
            while idx + 3 < n:
                report = days[idx]
                timing = rng.choice(["BMO", "AMC"])
                day0 = idx if timing == "BMO" else idx + 1
                jump = rng.gauss(0, 6.0)
                vol_mult = rng.uniform(1.2, 5.0)
                f = 1 + jump / 100.0
                for k in range(day0, n):
                    bars[k][0] *= f; bars[k][1] *= f; bars[k][2] *= f; bars[k][3] *= f
                bars[day0][0] = bars[day0 - 1][3]
                bars[day0][4] = int(bars[day0][4] * vol_mult)
                eps_c = rng.uniform(0.5, 3.0)
                eps_a = eps_c + rng.gauss(0, 0.2) + (0.1 if jump > 0 else -0.1)
                rev_c = int(rng.uniform(5e8, 2e10))
                rev_a = int(rev_c * (1 + rng.gauss(0.01 if jump > 0 else -0.01, 0.03)))
                next_idx = idx + 63
                k8 = ""
                if rng.random() < 0.08 and idx + 20 < n:
                    k8 = days[idx + rng.randrange(5, 20)].isoformat()
                events.append([sym, report.isoformat(), timing, f"{report.year}Q{fiscal_q % 4 + 1}", f"{eps_a:.2f}", f"{eps_c:.2f}",
                               (report - dt.timedelta(days=1)).isoformat(), f"{rev_a}.00", f"{rev_c}.00",
                               days[next_idx].isoformat() if next_idx < n else "", k8])
                fiscal_q += 1
                idx = next_idx
            for d, b in zip(days, bars):
                wb.writerow([sym, d.isoformat(), f"{b[0]:.2f}", f"{b[1]:.2f}", f"{b[2]:.2f}", f"{b[3]:.2f}", b[4]])
            for e in events:
                eid = hashlib.sha256(f"{e[0]}|{e[3]}|{e[1]}".encode()).hexdigest()[:32]
                we.writerow([eid] + e)
            # Point-in-time security rows: one per year, cap drifts with price.
            for y in range(start.year, end.year + 1):
                as_of = dt.date(y, 1, 1)
                cap = int(rng.uniform(3e9, 4e11) * (0.5 if i % 11 == 0 else 1.0))
                category = "ADR Common Stock" if i % 23 == 0 else "Domestic Common Stock"
                first_listed = (start - dt.timedelta(days=rng.randrange(400, 6000))).isoformat() if i % 17 else (start + dt.timedelta(days=500)).isoformat()
                ws.writerow([sym, f"{sym} Corp", sector, category, f"{cap}.00", first_listed, rng.randrange(2, 30), 1 if i % 13 else 0,
                             f"{rng.uniform(1.0, 7.0):.2f}", as_of.isoformat()])
    print(f"wrote {bars_path} ({n} sessions x {args.symbols + 1} symbols), {sec_path}, {earn_path}")


if __name__ == "__main__":
    main()
