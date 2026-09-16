"""Gate G4 evaluation from the logger archive.

Joins signals.candidate, signals.validated and market.data.bar records from
var/log/topics/*.jsonl(.gz) and computes, for every labelled candidate, the
40-session forward return from the next open. Reports the approved-minus-
vetoed difference with a Welch t-stat and the promotion decision rule:

  promote to live veto     diff > 0 and t > 1.5 over >= 150 labelled candidates
  demote to tail-risk-only vetoes show negative selectivity (diff < 0, t < -1.5)
  remove the layer         no measurable lift after an adequate sample

Run: at-evaluate-claude --config config/paper.json [--min-labelled 150]
"""
from __future__ import annotations

import argparse
import gzip
import json
import logging
import math
import statistics
from pathlib import Path
from typing import Iterator

from ..config import Config

log = logging.getLogger("autotrader.evaluation")

DRIFT_WINDOW = 40


def read_topic(root: Path, subject: str) -> Iterator[dict]:
    d = root / subject
    if not d.exists():
        return
    for p in sorted(d.iterdir()):
        opener = gzip.open if p.suffix == ".gz" else open
        with opener(p, "rt", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if line:
                    yield json.loads(line)


def load_bars(root: Path) -> dict[str, list[tuple[str, int, int]]]:
    """symbol -> sorted [(date, open_cents, close_cents)] from market.data.bar.* archives."""
    bars: dict[str, dict[str, tuple[int, int]]] = {}
    for sub in (root).glob("market.data.bar.*"):
        for env in read_topic(root, sub.name):
            p = env["payload"]
            bars.setdefault(p["symbol"], {})[p["session_date"]] = (p["open_cents"], p["close_cents"])
    return {s: sorted((d, o, c) for d, (o, c) in m.items()) for s, m in bars.items()}


def forward_return(bars: list[tuple[str, int, int]], signal_date: str, window: int) -> float | None:
    idx = next((i for i, (d, _, _) in enumerate(bars) if d > signal_date), None)
    if idx is None or idx + window >= len(bars):
        return None
    o = bars[idx][1]
    c = bars[idx + window][2]
    return (c - o) / o * 100.0 if o else None


def welch_t(a: list[float], b: list[float]) -> float:
    if len(a) < 2 or len(b) < 2:
        return 0.0
    va, vb = statistics.variance(a), statistics.variance(b)
    se = math.sqrt(va / len(a) + vb / len(b))
    return (statistics.fmean(a) - statistics.fmean(b)) / se if se > 0 else 0.0


def evaluate(root: Path, min_labelled: int, window: int = DRIFT_WINDOW) -> dict:
    cands = {env["msg_id"]: env["payload"] for env in read_topic(root, "signals.candidate")}
    verdicts: dict[str, dict] = {}
    for env in read_topic(root, "signals.validated"):
        p = env["payload"]
        if p.get("mode") in ("SHADOW", "LIVE") and p["candidate_msg_id"] in cands:
            verdicts[p["candidate_msg_id"]] = p
    bars = load_bars(root)
    approved, vetoed, errors, pending = [], [], 0, 0
    rows = []
    for cid, v in verdicts.items():
        c = cands[cid]
        if v["verdict"] == "ERROR":
            errors += 1
            continue
        fr = forward_return(bars.get(c["symbol"], []), c["session_date"], window)
        if fr is None:
            pending += 1
            continue
        rows.append({"symbol": c["symbol"], "session_date": c["session_date"], "verdict": v["verdict"], "flags": v.get("flags", []), "fwd_return_pct": fr, "cost_usd": v.get("cost_usd", 0.0)})
        (approved if v["verdict"] == "APPROVE" else vetoed).append(fr)
    n = len(approved) + len(vetoed)
    diff = (statistics.fmean(approved) - statistics.fmean(vetoed)) if approved and vetoed else None
    t = welch_t(approved, vetoed)
    if n < min_labelled:
        decision = f"insufficient_sample ({n} < {min_labelled} labelled with realised forward returns)"
    elif diff is not None and diff > 0 and t > 1.5:
        decision = "PROMOTE: approved-minus-vetoed forward return positive with t > 1.5 -> enable live veto"
    elif diff is not None and diff < 0 and t < -1.5:
        decision = "DEMOTE: vetoes show negative selectivity -> tail-risk flags only"
    else:
        decision = "REMOVE: no measurable lift after an adequate sample -> drop the layer (cost and failure surface for no edge)"
    return {
        "labelled_total": len(verdicts), "errors": errors, "awaiting_forward_window": pending, "n_approved": len(approved), "n_vetoed": len(vetoed),
        "mean_fwd_approved_pct": statistics.fmean(approved) if approved else None, "mean_fwd_vetoed_pct": statistics.fmean(vetoed) if vetoed else None,
        "diff_pct": diff, "t_stat": t, "total_cost_usd": sum(r["cost_usd"] for r in rows), "decision": decision, "rows": rows,
    }


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", default=None)
    ap.add_argument("--min-labelled", type=int, default=150)
    ap.add_argument("--out", default=None)
    a = ap.parse_args()
    logging.basicConfig(level="INFO")
    cfg = Config.load(a.config)
    root = cfg.resolve(cfg.get("log_dir", "var/log")) / "topics"
    res = evaluate(root, a.min_labelled)
    out = Path(a.out) if a.out else cfg.resolve("var/reports") / "claude_counterfactual.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(res, indent=1), encoding="utf-8")
    summary = {k: v for k, v in res.items() if k != "rows"}
    print(json.dumps(summary, indent=1))
    print("wrote", out)


if __name__ == "__main__":
    main()
