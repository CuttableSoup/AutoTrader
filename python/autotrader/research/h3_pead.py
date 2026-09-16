"""H3: earnings drift on the clean, relaxed universe. Spec: docs/prereg/H3-relaxed-pead.md.

Events are SF1 ARQ filings. The announcement date is the latest 8-K carrying item 2.02
(Sharadar event code 22) within the 60 days up to and including the filing date; if
there is none the filing date is used. BMO/AMC timing is unknown, so day0 is the first
session strictly after the announcement date and the score is formed at day0's close:
entry is the open of day0+1.

SUE  = (EPS_q - EPS_{q-4}) / sd(same difference over the prior 8 quarters, >= 4 present)
SRUE = the same construction on revenue
VOL  = volume[day0] / mean(volume[day0-20 .. day0-1])

Buckets are deciles with breakpoints taken from the previous four calendar quarters'
events in the same size band (fully point-in-time). Results are averaged per calendar
quarter of formation, and quarters are the time series for the Newey-West t (1 lag).

Bias check (reported, never gating): the v1 event_study.py EAR table re-run on this
panel, once with that script's timing (reaction measured through close(i0+1) while the
return starts at open(i0+1), which lets the sort see part of the measured return) and
once corrected (entry at open(i0+2)).
"""
from __future__ import annotations

from collections import defaultdict

import numpy as np
import pandas as pd

from autotrader.research import pretest, stats
from autotrader.research.panel import Panel, root

N_BUCKETS = 10
HORIZONS = [2, 5, 10, 20]
PRIMARY = 10
BANDS = ["5B+", "2B-5B", "500M-2B", "all"]
ANNOUNCE_LOOKBACK_DAYS = 60
MIN_PRIOR_EVENTS = 100
MIN_SD_QUARTERS = 4
SD_QUARTERS = 8
EAR_BUCKETS = [("< -5%", -1e9, -5), ("-5% to -2%", -5, -2), ("-2% to +2%", -2, 2), ("+2% to +5%", 2, 5), ("+5% to +10%", 5, 10), ("> +10%", 10, 1e9)]


# ------------------------------------------------------------------ events ---

def load_announcements(panel: Panel, events_csv=None) -> dict[str, np.ndarray]:
    """symbol -> sorted datetime64[D] of item 2.02 8-K dates."""
    path = events_csv or sorted((root() / "data" / "raw").glob("events-*.csv"))[-1]
    ev = pd.read_csv(path, dtype=str)
    ev = ev[ev["eventcodes"].fillna("").str.split("|").apply(lambda c: "22" in c) & ev["ticker"].isin(set(panel.symbols))]
    out: dict[str, np.ndarray] = {}
    for sym, g in ev.groupby("ticker"):
        out[sym] = np.sort(pd.to_datetime(g["date"]).to_numpy().astype("datetime64[D]"))
    return out


def surprise(values: pd.Series, cal: pd.Series) -> np.ndarray:
    """Seasonal random-walk standardised surprise for one ticker's quarterly series (sorted by calendardate)."""
    by_cal = dict(zip(cal.to_numpy().astype("datetime64[D]"), values.to_numpy(dtype=float)))
    cds = cal.to_numpy().astype("datetime64[D]")
    diffs = np.full(len(cds), np.nan)
    for i, c in enumerate(cds):
        prev = by_cal.get((pd.Timestamp(c) - pd.DateOffset(years=1) + pd.offsets.MonthEnd(0)).to_datetime64().astype("datetime64[D]"))
        if prev is not None and np.isfinite(prev) and np.isfinite(values.iloc[i]):
            diffs[i] = values.iloc[i] - prev
    out = np.full(len(cds), np.nan)
    for i in range(len(cds)):
        hist = diffs[max(0, i - SD_QUARTERS):i]
        hist = hist[np.isfinite(hist)]
        if len(hist) >= MIN_SD_QUARTERS and np.isfinite(diffs[i]):
            sd = hist.std(ddof=1)
            if sd > 0:
                out[i] = diffs[i] / sd
    return out


def build_events(panel: Panel, announcements: dict[str, np.ndarray]) -> pd.DataFrame:
    f = panel.fundamentals
    f = f[f["dimension"] == "ARQ"].sort_values(["ticker", "calendardate", "date"]).drop_duplicates(["ticker", "calendardate"], keep="first")
    sym_idx = {s: i for i, s in enumerate(panel.symbols)}
    rows = []
    for sym, g in f.groupby("ticker"):
        j = sym_idx.get(sym)
        if j is None:
            continue
        sue = surprise(g["eps"], g["calendardate"])
        srue = surprise(g["revenue"], g["calendardate"])
        ann = announcements.get(sym, np.array([], dtype="datetime64[D]"))
        for k, filing in enumerate(g["date"].to_numpy().astype("datetime64[D]")):
            lo = filing - np.timedelta64(ANNOUNCE_LOOKBACK_DAYS, "D")
            hits = ann[(ann >= lo) & (ann <= filing)]
            rows.append((j, filing, hits[-1] if len(hits) else filing, sue[k], srue[k]))
    ev = pd.DataFrame(rows, columns=["sym", "filing", "announce", "sue", "srue"])
    return ev


def day0_index(panel: Panel, d: np.ndarray) -> np.ndarray:
    return np.searchsorted(panel.dates, d.astype("datetime64[D]"), side="right")


def event_forward_pct(panel: Panel, entry: np.ndarray, sym: np.ndarray, h: int) -> np.ndarray:
    exit_ = entry + h
    ok = (entry < panel.T) & (exit_ < panel.T)
    out = np.full(len(entry), np.nan)
    e, x, s = entry[ok], exit_[ok], sym[ok]
    with np.errstate(invalid="ignore", divide="ignore"):
        stock = pretest.close_tr_filled(panel)[x, s] / panel.open_tr()[e, s] - 1.0
    spy = panel.spy_close_tr[x] / panel.spy_open_tr[e] - 1.0
    out[ok] = (stock - spy) * 100.0
    return out


# ------------------------------------------------------------------- sorts ---

def quarterly_decile_sort(panel: Panel, ev: pd.DataFrame, score_col: str, date_col: str) -> dict[str, dict[int, pretest.BandSeries]]:
    """Formation at close of day0 (first session after date_col), entry at day0+1 open."""
    d0 = day0_index(panel, ev[date_col].to_numpy())
    valid = d0 + 1 < panel.T
    ev = ev[valid].copy()
    ev["f"] = d0[valid]
    ev = ev[np.isfinite(ev[score_col].to_numpy(dtype=float))]
    masks = panel.band_masks()
    ev["quarter"] = pd.PeriodIndex(panel.dates[ev["f"].to_numpy()].astype(str), freq="Q").astype(str)
    quarters = sorted(ev["quarter"].unique())
    fwd = {h: event_forward_pct(panel, ev["f"].to_numpy() + 1, ev["sym"].to_numpy(), h) for h in HORIZONS}
    out: dict[str, dict[int, pretest.BandSeries]] = {}
    for band in BANDS:
        in_band = masks[band][ev["f"].to_numpy(), ev["sym"].to_numpy()]
        evb = ev[in_band]
        fwdb = {h: v[in_band] for h, v in fwd.items()}
        per_h = {h: pretest.BandSeries([], np.full((len(quarters), N_BUCKETS), np.nan), np.zeros((len(quarters), N_BUCKETS)), [])
                 for h in HORIZONS}
        qarr = evb["quarter"].to_numpy()
        scores = evb[score_col].to_numpy(dtype=float)
        for qi, q in enumerate(quarters):
            prior = scores[np.isin(qarr, quarters[max(0, qi - 4):qi])]
            cur = qarr == q
            for h in HORIZONS:
                per_h[h].labels.append(q)
                per_h[h].turnover.append(float("nan"))
            if len(prior) < MIN_PRIOR_EVENTS or not cur.any():
                continue
            edges = np.percentile(prior, np.arange(1, N_BUCKETS) * 100.0 / N_BUCKETS)
            b = np.searchsorted(edges, scores[cur], side="right")
            for h in HORIZONS:
                vals_h = fwdb[h][cur]
                for k in range(N_BUCKETS):
                    v = vals_h[(b == k) & np.isfinite(vals_h)]
                    per_h[h].counts[qi, k] = len(v)
                    if len(v) >= pretest.MIN_PER_BUCKET:
                        per_h[h].means[qi, k] = float(v.mean())
        out[band] = per_h
    return out


def volume_ratio(panel: Panel, ev: pd.DataFrame, date_col: str) -> np.ndarray:
    d0 = day0_index(panel, ev[date_col].to_numpy())
    vol = panel.px["volume"]
    out = np.full(len(ev), np.nan)
    for i, (t, s) in enumerate(zip(d0, ev["sym"].to_numpy())):
        if 20 <= t < panel.T:
            base = np.nanmean(vol[t - 20:t, s])
            if base > 0:
                out[i] = vol[t, s] / base
    return out


def ear_bias_check(panel: Panel, ev: pd.DataFrame) -> dict:
    """event_study.py's EAR table on the clean panel: its timing vs corrected timing, 40 sessions."""
    i0 = day0_index(panel, ev["filing"].to_numpy())
    s = ev["sym"].to_numpy()
    ok = (i0 >= 25) & (i0 + 2 + 40 < panel.T)
    i0, s = i0[ok], s[ok]
    c = panel.close_tr()
    with np.errstate(invalid="ignore", divide="ignore"):
        stock = c[i0 + 1, s] / c[i0 - 1, s] - 1.0
    mkt = panel.spy_close_tr[i0 + 1] / panel.spy_close_tr[i0 - 1] - 1.0
    ear = (stock - mkt) * 100.0
    mcap = panel.market_cap()[i0 - 1, s]
    elig = panel.universe_mask()[i0 - 1, s]
    as_script = event_forward_pct(panel, i0 + 1, s, 40)       # entry open(i0+1): same session the reaction closes on
    corrected = event_forward_pct(panel, i0 + 2, s, 40)       # entry open(i0+2): after the reaction is known
    out = {}
    for label, lo, hi in [("all", 0, np.inf), ("500M-2B", 5e8, 2e9), ("2B-5B", 2e9, 5e9), ("5B+", 5e9, np.inf)]:
        band = elig & (mcap >= lo) & (mcap < hi)
        rows = []
        for name, blo, bhi in EAR_BUCKETS:
            sel = band & (ear >= blo) & (ear < bhi)
            a, b = as_script[sel], corrected[sel]
            a, b = a[np.isfinite(a)], b[np.isfinite(b)]
            rows.append({"bucket": name, "n": int(len(b)),
                         "as_script_mean_pct": float(a.mean()) if len(a) else float("nan"),
                         "corrected_mean_pct": float(b.mean()) if len(b) else float("nan"),
                         "corrected_t": stats.newey_west_t(b, 0) if len(b) > 2 else 0.0})
        out[label] = rows
    return out


def run(panel: Panel, events_csv=None) -> tuple[dict, str, int]:
    ann = load_announcements(panel, events_csv)
    ev = build_events(panel, ann)
    matched = float((ev["announce"] != ev["filing"]).mean())
    ev["vol_ann"] = volume_ratio(panel, ev, "announce")
    arms = [("sue_at_announcement", "sue", "announce", "SUE at the 2.02 8-K date (gating arm)"),
            ("sue_at_filing", "sue", "filing", "SUE at the SF1 filing date (conservative timing, not gating)"),
            ("srue_at_announcement", "srue", "announce", "Revenue surprise at the 8-K date (not gating)"),
            ("volume_at_announcement", "vol_ann", "announce", "Announcement-day volume ratio (not gating)")]
    result = {"hypothesis": "H3", "n_events": int(len(ev)), "share_with_8k_announcement": matched}
    parts = [f"## H3 relaxed-universe earnings drift ({len(ev)} ARQ events; {matched:.0%} matched to an item 2.02 8-K)"]
    for key, col, date_col, title in arms:
        series = quarterly_decile_sort(panel, ev, col, date_col)
        result[key] = pretest.summarize_sort(series, PRIMARY, 63, lags=1)
        if key != "sue_at_announcement":
            for band in result[key].values():
                for r in band.values():
                    r["primary"] = False
                    r.pop("pass", None)
                    r.pop("fail_reasons", None)
        parts.append(pretest.render_sort_markdown(title, result[key], N_BUCKETS))
    bias = ear_bias_check(panel, ev)
    result["ear_bias_check"] = bias
    lines = ["### Bias check: v1 event-study EAR buckets, 40 sessions, on the clean panel (not gating)", "",
             "`as script` enters at the open of the session whose close completes the reaction measurement, as scripts/event_study.py did. `corrected` enters one session later.", ""]
    for band, rows in bias.items():
        lines += [f"**{band}**", "", "| EAR bucket | n | as script | corrected | corrected t |", "|---|---|---|---|---|"]
        lines += [f"| {r['bucket']} | {r['n']} | {r['as_script_mean_pct']:+.2f} | {r['corrected_mean_pct']:+.2f} | {r['corrected_t']:+.2f} |" for r in rows]
        lines.append("")
    parts.append("\n".join(lines))
    n_configs = len(arms) * len(BANDS) * len(HORIZONS) + 2 * 4 * len(EAR_BUCKETS)
    return result, "\n\n".join(parts), n_configs
