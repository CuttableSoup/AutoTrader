#!/usr/bin/env python
"""One-off: bulk-download Sharadar SF2 (Form 4 insider transactions) into data/raw/.

Sharadar's SharadarClient.bulk() is already generic across tables (docs/DECISIONS.md's
Sharadar API migration section); this just calls it for "insiders", the one table the
research panel does not yet have (DECISIONS.md: "the Sharadar insiders table is entitled
... data/insiders returns Form 4 rows"). Written as insiders-<years>-<date>.csv, which
autotrader.research.panel.insiders_path() already looks for.

Verify the downloaded file's column names against panel.load_insider_transactions's
docstring before trusting the insider_buy arm of docs/prereg/BRUTEFORCE-v1.md: unlike
every other table this repo reads, SF2's field names here are asserted from public
Sharadar documentation, not yet measured against this account's live response.

usage: python scripts/fetch_insiders_bulk.py [--years 10]
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python"))

from autotrader.config import Config, Secrets  # noqa: E402
from autotrader.universe.sharadar import SharadarClient  # noqa: E402


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--years", default="10")
    ap.add_argument("--out", default=str(ROOT / "data" / "raw"))
    a = ap.parse_args()

    cfg = Config.load(ROOT / "config" / "paper.json")
    client = SharadarClient(Secrets.load(cfg).require("nasdaq_data_link_api_key"))
    path = client.bulk("insiders", a.years, Path(a.out))
    if path is None:
        raise SystemExit("bulk export of `insiders` is not available on this subscription; "
                         "fall back to paginated client.rows_safe('insiders', ticker=...) per symbol")
    print(f"wrote {path}")


if __name__ == "__main__":
    main()
