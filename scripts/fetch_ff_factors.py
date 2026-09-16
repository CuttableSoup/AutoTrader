#!/usr/bin/env python
"""Download the Fama-French 3-factor DAILY file from the Ken French data library.

Writes data/factors/ff3_daily.csv (date, mkt_rf, smb, hml, rf as DECIMALS) and
data/factors/SOURCE.md with the URL, retrieval date, sha256 of the original zip and
the last date covered. Residual momentum (docs/prereg/H1-residual-momentum.md)
regresses on these factors, as Blitz, Huij and Martens (2011) do.

usage: python scripts/fetch_ff_factors.py
"""
from __future__ import annotations

import datetime as dt
import hashlib
import io
import sys
import zipfile
from pathlib import Path

import httpx

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from autotrader.research.factors import parse_french_csv  # noqa: E402

URL = "https://mba.tuck.dartmouth.edu/pages/faculty/ken.french/ftp/F-F_Research_Data_Factors_daily_CSV.zip"


def main() -> None:
    out_dir = Path(__file__).resolve().parents[1] / "data" / "factors"
    out_dir.mkdir(parents=True, exist_ok=True)
    r = httpx.get(URL, timeout=120, follow_redirects=True, headers={"User-Agent": "autotrader-research"})
    r.raise_for_status()
    sha = hashlib.sha256(r.content).hexdigest()
    with zipfile.ZipFile(io.BytesIO(r.content)) as z:
        name = next(n for n in z.namelist() if n.lower().endswith(".csv"))
        text = z.read(name).decode("latin-1")
    df = parse_french_csv(text)
    df.index = df.index.strftime("%Y-%m-%d")
    df.index.name = "date"
    df.to_csv(out_dir / "ff3_daily.csv", float_format="%.6f", lineterminator="\n")
    (out_dir / "SOURCE.md").write_text(
        "# Fama-French 3 factors (daily)\n\n"
        f"* Source: {URL}\n"
        f"* Retrieved: {dt.date.today().isoformat()}\n"
        f"* sha256 of the downloaded zip: `{sha}`\n"
        f"* Rows: {len(df)}, {df.index[0]} to {df.index[-1]}\n"
        "* Units: converted from percent to decimals by scripts/fetch_ff_factors.py\n\n"
        "The library publishes with a lag of roughly one to two months. Residual momentum skips the\n"
        "most recent month, so the lag does not block a live monthly rebalance, but check the last\n"
        "covered date before every live formation.\n",
        encoding="utf-8", newline="\n")
    print(f"wrote {len(df)} rows through {df.index[-1]}; sha256 {sha[:16]}")


if __name__ == "__main__":
    main()
