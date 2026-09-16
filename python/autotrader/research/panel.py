"""Point-in-time research panel built from the raw Sharadar bulk exports in data/raw/.

Why not data/sharadar/: that layout keeps only symbols whose market cap was ever above
the floor somewhere in the window (sharadar.build_from_bulk). Below the floor, every
name it carries is one that later grew, which flatters any small-cap result. Here the
full domestic common-stock population is loaded and membership is decided from data
available at each date only.

Conventions (verified against the raw files 2026-09-16):
  * SEP `close`/`open`/`volume` are split-adjusted; `closeadj` also folds in dividends.
    Returns use total-return prices: close_tr = closeadj, open_tr = open * closeadj / close.
  * SF1 `sharesbas` is RESTATED for later splits (AAPL's 2020-07-31 filing already shows
    post-4:1 shares, and SF1 marketcap = split-adjusted price x sharesbas). So market cap
    is split-adjusted close x sharesbas x sharefactor, and share-count changes need no
    split correction.
  * SF1 `date` is the filing date. A value is usable from the first session AFTER it.
  * Close x volume on split-adjusted series is the unadjusted dollar volume.
  * The trading calendar is SPY's (Sharadar `funds`, which also supplies SPY closeadj;
    the SPY rows in data/sharadar/bars.csv are price-only and would bias excess returns
    by the index dividend yield).
"""
from __future__ import annotations

import datetime as dt
import json
import logging
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
import pandas as pd

from autotrader.research.seal import SEAL_DATE
from autotrader.schemas import find_project_root

log = logging.getLogger("autotrader.research.panel")

COMMON_CATEGORIES = ("Domestic Common Stock", "Domestic Common Stock Primary Class", "Domestic Common Stock Secondary Class")
PRICE_FIELDS = ("open", "close", "closeadj", "closeunadj", "volume")
FUND_COLS = ["ticker", "dimension", "calendardate", "date", "fiscalperiod", "eps", "revenue", "gp", "assets", "sharesbas", "sharefactor"]

# Universe rules for the v2 pre-tests (docs/prereg/*). Deliberately NOT the v1 C++ rules.
MIN_MCAP = 5e8
MIN_PRICE = 5.0
MIN_ADV20_DOLLARS = 5e6
MIN_LISTING_DAYS = 365
BANDS = (("500M-2B", 5e8, 2e9), ("2B-5B", 2e9, 5e9), ("5B+", 5e9, float("inf")))
FF_STALE_SESSIONS = 300   # a fundamental older than ~14 months is treated as missing


def root() -> Path:
    return find_project_root(Path(__file__).parent)


@dataclass
class Panel:
    dates: np.ndarray                   # datetime64[D], (T,)
    symbols: np.ndarray                 # str, (N,)
    px: dict[str, np.ndarray]           # PRICE_FIELDS -> float64 (T, N), NaN where no bar
    spy_open_tr: np.ndarray             # (T,)
    spy_close_tr: np.ndarray            # (T,)
    first_price: np.ndarray             # datetime64[D], (N,)
    sector: np.ndarray                  # str, (N,)
    fundamentals: pd.DataFrame          # long SF1 rows (ARQ + ART) for the panel's symbols
    end: str
    _cache: dict = field(default_factory=dict, repr=False)

    # ------------------------------------------------------------ indexing ---
    @property
    def T(self) -> int:
        return len(self.dates)

    @property
    def N(self) -> int:
        return len(self.symbols)

    def date_index(self, d: str | np.datetime64) -> int:
        """Index of the first session on or after d."""
        return int(np.searchsorted(self.dates, np.datetime64(d, "D"), side="left"))

    def month_end_indices(self, start: str | None = None) -> np.ndarray:
        """Index of the last session of every calendar month (the formation dates)."""
        months = self.dates.astype("datetime64[M]")
        idx = np.nonzero(np.r_[months[1:] != months[:-1], True])[0]
        if start:
            idx = idx[self.dates[idx] >= np.datetime64(start, "D")]
        # The final month is only complete if the panel runs past it.
        return idx[:-1] if len(idx) and idx[-1] == self.T - 1 else idx

    # ------------------------------------------------------- derived series ---
    def close_tr(self) -> np.ndarray:
        return self.px["closeadj"]

    def open_tr(self) -> np.ndarray:
        if "open_tr" not in self._cache:
            with np.errstate(invalid="ignore", divide="ignore"):
                self._cache["open_tr"] = self.px["open"] * self.px["closeadj"] / self.px["close"]
        return self._cache["open_tr"]

    def daily_returns(self) -> np.ndarray:
        """Close-to-close total returns, (T, N); row 0 is NaN."""
        if "ret" not in self._cache:
            c = self.close_tr()
            r = np.full(c.shape, np.nan)
            with np.errstate(invalid="ignore", divide="ignore"):
                r[1:] = c[1:] / c[:-1] - 1.0
            self._cache["ret"] = r
        return self._cache["ret"]

    def spy_daily_returns(self) -> np.ndarray:
        r = np.full(self.T, np.nan)
        r[1:] = self.spy_close_tr[1:] / self.spy_close_tr[:-1] - 1.0
        return r

    def asof_matrix(self, dimension: str, column: str) -> np.ndarray:
        """(T, N) value of a fundamental as known at each session's close.

        A row filed on date F first appears at the first session strictly after F, then
        carries forward until the next filing or FF_STALE_SESSIONS sessions.
        """
        key = f"asof:{dimension}:{column}"
        if key in self._cache:
            return self._cache[key]
        f = self.fundamentals
        f = f[(f["dimension"] == dimension) & f[column].notna()]
        sym_idx = {s: i for i, s in enumerate(self.symbols)}
        out = np.full((self.T, self.N), np.nan)
        cols = f["ticker"].map(sym_idx)
        ok = cols.notna()
        f, cols = f[ok], cols[ok].astype(int).to_numpy()
        rows = np.searchsorted(self.dates, f["date"].to_numpy().astype("datetime64[D]"), side="right")
        keep = rows < self.T
        # Sort by filing date so a later filing mapping to the same session wins.
        order = np.argsort(f["date"].to_numpy()[keep], kind="stable")
        r, c, v = rows[keep][order], cols[keep][order], f[column].to_numpy(dtype=float)[keep][order]
        out[r, c] = v
        out = pd.DataFrame(out).ffill(limit=FF_STALE_SESSIONS).to_numpy()
        self._cache[key] = out
        return out

    def shares(self) -> np.ndarray:
        if "shares" not in self._cache:
            self._cache["shares"] = self.asof_matrix("ARQ", "sharesbas") * np.nan_to_num(self.asof_matrix("ARQ", "sharefactor"), nan=1.0)
        return self._cache["shares"]

    def market_cap(self) -> np.ndarray:
        if "mcap" not in self._cache:
            self._cache["mcap"] = self.px["close"] * self.shares()
        return self._cache["mcap"]

    def adv20_dollars(self) -> np.ndarray:
        if "adv20" not in self._cache:
            dv = self.px["close"] * self.px["volume"]
            self._cache["adv20"] = pd.DataFrame(dv).rolling(20, min_periods=15).mean().to_numpy()
        return self._cache["adv20"]

    def universe_mask(self) -> np.ndarray:
        """(T, N) bool: eligible at that session's close, from data known at that close."""
        if "mask" not in self._cache:
            age_ok = (self.dates[:, None] - self.first_price[None, :]) >= np.timedelta64(MIN_LISTING_DAYS, "D")
            with np.errstate(invalid="ignore"):
                m = ((self.market_cap() >= MIN_MCAP) & (self.px["closeunadj"] >= MIN_PRICE)
                     & (self.adv20_dollars() >= MIN_ADV20_DOLLARS) & age_ok & np.isfinite(self.close_tr()))
            self._cache["mask"] = m
        return self._cache["mask"]

    def band_masks(self) -> dict[str, np.ndarray]:
        mcap, base = self.market_cap(), self.universe_mask()
        with np.errstate(invalid="ignore"):
            out = {name: base & (mcap >= lo) & (mcap < hi) for name, lo, hi in BANDS}
        out["all"] = base
        return out


# ------------------------------------------------------------------ building ---

def _raw_files(raw_dir: Path) -> dict[str, Path]:
    def one(prefix: str) -> Path:
        hits = sorted(raw_dir.glob(f"{prefix}-*.csv"))
        if not hits:
            raise FileNotFoundError(f"no {prefix}-*.csv in {raw_dir}; run `at-universe backtest --bulk` first")
        return hits[-1]
    return {"tickers": one("tickers"), "stocks": one("stocks"), "fundamentals": one("fundamentals")}


def _manifest(files: dict[str, Path], end: str, spy: Path) -> dict:
    m = {k: [p.name, p.stat().st_size, int(p.stat().st_mtime)] for k, p in files.items()}
    m["spy"] = [spy.name, spy.stat().st_size, int(spy.stat().st_mtime)]
    m["end"] = end
    m["version"] = 1
    return m


def fetch_spy_funds(dest: Path, start: str = "2016-01-01", end: str | None = None) -> Path:
    """SPY OHLC + closeadj from Sharadar `funds`, once, into the research cache."""
    from autotrader.config import Config, Secrets
    from autotrader.universe.sharadar import SharadarClient

    cfg = Config.load(root() / "config" / "paper.json")
    client = SharadarClient(Secrets.load(cfg).require("nasdaq_data_link_api_key"))
    rows = sorted(client.rows_safe("funds", ticker="SPY", **{"from": start, "to": end or dt.date.today().isoformat()}),
                  key=lambda r: r["date"])
    if not rows or "closeadj" not in rows[0]:
        raise RuntimeError("sharadar funds returned no SPY rows with closeadj")
    dest.parent.mkdir(parents=True, exist_ok=True)
    pd.DataFrame(rows)[["date", "open", "close", "closeadj"]].to_csv(dest, index=False)
    log.info("SPY from sharadar funds: %d rows -> %s", len(rows), dest)
    return dest


def build_panel(raw_dir: Path | None = None, end: str = SEAL_DATE, cache_dir: Path | None = None, rebuild: bool = False) -> Panel:
    raw_dir = raw_dir or root() / "data" / "raw"
    cache_dir = cache_dir or root() / "data" / "cache" / "research"
    cache_dir.mkdir(parents=True, exist_ok=True)
    files = _raw_files(raw_dir)
    spy_path = cache_dir / "spy_funds.csv"
    if not spy_path.exists():
        fetch_spy_funds(spy_path)
    manifest = _manifest(files, end, spy_path)
    tag = end.replace("-", "")
    npz, fpk, mf = cache_dir / f"panel-{tag}.npz", cache_dir / f"fundamentals-{tag}.pkl", cache_dir / f"panel-{tag}.json"
    if not rebuild and npz.exists() and fpk.exists() and mf.exists() and json.loads(mf.read_text()) == manifest:
        z = np.load(npz, allow_pickle=False)
        log.info("panel cache hit: %s", npz.name)
        return Panel(dates=z["dates"], symbols=z["symbols"], px={k: z[k].astype(np.float64) for k in PRICE_FIELDS},
                     spy_open_tr=z["spy_open_tr"], spy_close_tr=z["spy_close_tr"], first_price=z["first_price"],
                     sector=z["sector"], fundamentals=pd.read_pickle(fpk), end=end)

    # ---- securities
    t = pd.read_csv(files["tickers"], dtype=str)
    t = t[(t["table"] == "SEP") & t["category"].isin(COMMON_CATEGORIES)]
    universe = set(t["ticker"])
    log.info("tickers: %d domestic common stocks (delisted included)", len(universe))

    # ---- calendar from SPY
    spy = pd.read_csv(spy_path, dtype={"date": str})
    spy = spy[spy["date"] <= end].sort_values("date").drop_duplicates("date")

    # ---- prices, streamed
    parts = []
    for ch in pd.read_csv(files["stocks"], usecols=["ticker", "date", *PRICE_FIELDS], dtype={"ticker": str, "date": str},
                          chunksize=4_000_000):
        ch = ch[ch["ticker"].isin(universe) & (ch["date"] <= end)]
        parts.append(ch)
        log.info("stocks: %d rows kept so far", sum(len(p) for p in parts))
    bars = pd.concat(parts, ignore_index=True)
    bars = bars[bars["close"] > 0].drop_duplicates(["ticker", "date"], keep="last")
    bars = bars[bars["date"] >= spy["date"].iloc[0]]

    dates = pd.to_datetime(spy["date"]).to_numpy().astype("datetime64[D]")
    symbols = np.array(sorted(bars["ticker"].unique()))
    di = np.searchsorted(dates, pd.to_datetime(bars["date"]).to_numpy().astype("datetime64[D]"))
    on_cal = (di < len(dates))
    on_cal[on_cal] = dates[di[on_cal]] == pd.to_datetime(bars["date"][on_cal]).to_numpy().astype("datetime64[D]")
    if (~on_cal).any():
        log.warning("dropping %d bars on dates SPY did not trade", int((~on_cal).sum()))
    bars, di = bars[on_cal], di[on_cal]
    si = np.searchsorted(symbols, bars["ticker"].to_numpy())
    px = {}
    for k in PRICE_FIELDS:
        a = np.full((len(dates), len(symbols)), np.nan, dtype=np.float32)
        a[di, si] = bars[k].to_numpy(dtype=np.float32)
        px[k] = a

    tt = t.set_index("ticker").reindex(symbols)
    first_price = pd.to_datetime(tt["firstpricedate"], errors="coerce").fillna(pd.Timestamp("1900-01-01")).to_numpy().astype("datetime64[D]")
    sector = tt["sector"].fillna("").to_numpy().astype(str)

    # ---- fundamentals (ARQ + ART) for these symbols, up to `end` by filing date
    fparts = []
    symset = set(symbols)
    for ch in pd.read_csv(files["fundamentals"], usecols=FUND_COLS, dtype={"ticker": str, "dimension": str, "date": str,
                                                                           "calendardate": str, "fiscalperiod": str},
                          chunksize=1_000_000):
        fparts.append(ch[ch["dimension"].isin(["ARQ", "ART"]) & ch["ticker"].isin(symset) & (ch["date"] <= end)])
    fund = pd.concat(fparts, ignore_index=True)
    fund["date"] = pd.to_datetime(fund["date"])
    fund["calendardate"] = pd.to_datetime(fund["calendardate"])

    so, sc = spy["open"].to_numpy(float), spy["close"].to_numpy(float)
    spy_close_tr = spy["closeadj"].to_numpy(float)
    spy_open_tr = so * spy_close_tr / sc

    np.savez(npz, dates=dates, symbols=symbols, spy_open_tr=spy_open_tr, spy_close_tr=spy_close_tr,
             first_price=first_price, sector=sector, **px)
    fund.to_pickle(fpk)
    mf.write_text(json.dumps(manifest))
    log.info("panel built: %d sessions x %d symbols, %d fundamental rows (end %s)", len(dates), len(symbols), len(fund), end)
    return Panel(dates=dates, symbols=symbols, px={k: v.astype(np.float64) for k, v in px.items()}, spy_open_tr=spy_open_tr,
                 spy_close_tr=spy_close_tr, first_price=first_price, sector=sector, fundamentals=fund, end=end)
