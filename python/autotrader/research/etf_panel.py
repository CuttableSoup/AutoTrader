"""ETF price panel from Sharadar `funds`, for time-series trend-following research
(docs/prereg/TSMOM-v1.md).

Why not panel.py's Panel: Panel answers "which of ~2,000+ stocks was point-in-time
eligible, with survivorship and fundamentals joined in" -- membership, delisting,
SF1 joins, "excess over SPY" as a stock-picking benchmark. None of that applies to a
small, fixed, currently-live ETF list: there is no membership question, no
survivorship risk, no fundamentals analog, and a trend-following book's edge is its
OWN directional exposure, not a spread against a benchmark. This is a much smaller,
bespoke loader instead of forcing ETF data through machinery built for a different
question.

`funds` is a general Sharadar table (verified 2026-09-16: NOT SPY-special-cased,
returns ticker/date/open/high/low/close/volume/closeadj/closeunadj for any symbol).
Probed 2026-09-16: all 18 tickers in TSMOM-v1's universe return data, but every one
starts exactly 2016-09-19 -- the Sharadar subscription's 10-year entitlement window
(docs/DECISIONS.md), not each ETF's real listing date. The usable window is therefore
~9 years (2016-09-19 to seal.SEAL_DATE), a real limitation stated in
docs/prereg/TSMOM-v1.md, not silently absorbed.
"""
from __future__ import annotations

import hashlib
import json
import logging
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
import pandas as pd

from autotrader.research.seal import SEAL_DATE
from autotrader.schemas import find_project_root

log = logging.getLogger("autotrader.research.etf_panel")

FIELDS = ("open", "high", "low", "close", "closeadj", "volume")


def root() -> Path:
    return find_project_root(Path(__file__).parent)


@dataclass
class EtfPanel:
    dates: np.ndarray                   # datetime64[D], (T,)
    symbols: np.ndarray                 # str, (N,)
    px: dict[str, np.ndarray]           # FIELDS -> float64 (T, N), NaN where no bar
    end: str
    _cache: dict = field(default_factory=dict, repr=False)

    @property
    def T(self) -> int:
        return len(self.dates)

    @property
    def N(self) -> int:
        return len(self.symbols)

    def date_index(self, d: str) -> int:
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


def _manifest(symbols: list[str], end: str) -> dict:
    # version 2: cache stores full float64 precision (v1 downcast to float32 on write,
    # which a cache-hit run silently never recovered -- bumped so any v1 cache is rebuilt).
    return {"symbols": list(symbols), "end": end, "version": 2}


def build_etf_panel(symbols: list[str], end: str = SEAL_DATE, cache_dir: Path | None = None,
                    rebuild: bool = False) -> EtfPanel:
    cache_dir = cache_dir or root() / "data" / "cache" / "research"
    cache_dir.mkdir(parents=True, exist_ok=True)
    # Hash the symbol list rather than concatenating names -- 18 tickers makes a long filename.
    tag = hashlib.sha256("|".join(symbols).encode()).hexdigest()[:12] + "-" + end.replace("-", "")
    npz, mf = cache_dir / f"etf_panel-{tag}.npz", cache_dir / f"etf_panel-{tag}.json"
    manifest = _manifest(symbols, end)
    if not rebuild and npz.exists() and mf.exists() and json.loads(mf.read_text()) == manifest:
        z = np.load(npz, allow_pickle=False)
        log.info("etf panel cache hit: %s", npz.name)
        return EtfPanel(dates=z["dates"], symbols=z["symbols"], px={k: z[k].astype(np.float64) for k in FIELDS}, end=end)

    from autotrader.config import Config, Secrets
    from autotrader.universe.sharadar import SharadarClient

    cfg = Config.load(root() / "config" / "paper.json")
    client = SharadarClient(Secrets.load(cfg).require("nasdaq_data_link_api_key"))

    per_symbol: dict[str, pd.DataFrame] = {}
    all_dates: set[str] = set()
    for sym in symbols:
        rows = sorted(client.rows_safe("funds", ticker=sym, **{"from": "1990-01-01", "to": end}), key=lambda r: r["date"])
        if not rows:
            raise RuntimeError(f"sharadar funds returned no rows for {sym}")
        df = pd.DataFrame(rows)
        per_symbol[sym] = df
        all_dates.update(df["date"].tolist())
        log.info("etf panel: %s %d rows, %s..%s", sym, len(df), df["date"].iloc[0], df["date"].iloc[-1])

    dates = np.array(sorted(all_dates), dtype="datetime64[D]")
    symbols_arr = np.array(symbols)
    date_idx = {d: i for i, d in enumerate(dates.astype(str))}
    px = {k: np.full((len(dates), len(symbols)), np.nan, dtype=np.float64) for k in FIELDS}
    for j, sym in enumerate(symbols):
        df = per_symbol[sym]
        rows_i = df["date"].map(date_idx).to_numpy()
        for k in FIELDS:
            px[k][rows_i, j] = df[k].to_numpy(dtype=np.float64)

    # Stored at full float64 precision -- a cache-hit run must reproduce a cache-miss run
    # exactly, or a "frozen" pre-registered spec's PASS/FAIL could depend on cache state.
    np.savez(npz, dates=dates, symbols=symbols_arr, **px)
    mf.write_text(json.dumps(manifest))
    log.info("etf panel built: %d sessions x %d symbols (end %s)", len(dates), len(symbols), end)
    return EtfPanel(dates=dates, symbols=symbols_arr, px=px, end=end)
