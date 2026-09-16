#!/usr/bin/env python
"""Weekly watchdog drill (docs/DESIGN.md section 6: "Untested = nonexistent").

Runs the real kill path against the PAPER account through the watchdog's own
HTTP API, verifies the account is flat afterwards, and appends a record to
var/drills.jsonl. Schedule it weekly (cron / Task Scheduler) while in paper.

usage: python scripts/drill_watchdog.py --watchdog http://127.0.0.1:8787 --token <bearer>
exit code 0 = flat verified, 2 = kill path did not verify flat, 1 = watchdog unreachable
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import sys
import urllib.error
import urllib.request
from pathlib import Path


def post(url: str, token: str, body: dict) -> dict:
    req = urllib.request.Request(url, data=json.dumps(body).encode(), method="POST", headers={"Authorization": f"Bearer {token}", "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=120) as r:
        return json.loads(r.read())


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--watchdog", default="http://127.0.0.1:8787")
    ap.add_argument("--token", required=True)
    ap.add_argument("--log", default="var/drills.jsonl")
    a = ap.parse_args()
    try:
        report = post(a.watchdog + "/drill", a.token, {})
    except (urllib.error.URLError, OSError) as e:
        print("watchdog unreachable:", e)
        return 1
    rec = {"at": dt.datetime.now(dt.timezone.utc).isoformat(), "report": report}
    Path(a.log).parent.mkdir(parents=True, exist_ok=True)
    with open(a.log, "a", encoding="utf-8") as f:
        f.write(json.dumps(rec) + "\n")
    print(json.dumps(report, indent=1))
    if not report.get("flat"):
        print("DRILL FAILED: account not flat after kill path")
        return 2
    print("drill ok: flat in", report.get("attempts"), "attempt(s); bus announced =", report.get("bus_announced"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
