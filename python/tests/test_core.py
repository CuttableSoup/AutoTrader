"""Unit tests for the Python sidecars that do not need NATS or network."""
from __future__ import annotations

import datetime as dt
import json
from pathlib import Path

import pytest

from autotrader.bus import Dedupe
from autotrader.envelope import Envelope, parse_iso_utc
from autotrader.events_feed.feed import FeedState, build_payload, content_hash, event_id, merge_vendor_events
from autotrader.events_feed.vendors import VendorEvent
from autotrader.evaluation.counterfactual import forward_return, welch_t
from autotrader.schemas import SchemaRegistry, find_project_root, subject_matches
from autotrader.universe.builder import build_snapshot, exclusion_reason
from autotrader.validator.claude_validator import Verdict, candidate_facts_json, error_verdict
from autotrader.validator.mock import MockValidator

ROOT = find_project_root(Path(__file__).parent)


def test_envelope_round_trip():
    e = Envelope("strategy", {"a": 1})
    d = json.loads(e.dumps())
    back = Envelope.from_dict(d)
    assert back.msg_id == e.msg_id and back.payload == {"a": 1}
    parse_iso_utc(back.ts_utc)
    with pytest.raises(ValueError):
        Envelope.from_dict({"schema_version": "1.0", "msg_id": "nope", "ts_utc": e.ts_utc, "producer": "p", "payload": {}})


def test_subject_wildcards():
    assert subject_matches("market.data.>", "market.data.bar.AAPL")
    assert subject_matches("market.data.bar.*", "market.data.bar.AAPL")
    assert not subject_matches("market.data.bar.*", "market.data.quote.AAPL")
    assert subject_matches("control.*", "control.halt")
    assert not subject_matches("orders.approved", "orders.approvedx")


def test_registry_validates_and_matches_cpp_registry():
    reg = SchemaRegistry(ROOT / "schemas")
    assert len(reg.streams) == 8
    assert reg.schema_for_subject("market.data.quote.MSFT") == "v1/market.data.schema.json"
    assert reg.stream_for_subject("orders.filled") == "ORDERS"
    good = Envelope("watchdog", {"command": "halt", "reason": "drill", "source": "drill", "issued_at_utc": "2026-09-15T00:00:00Z", "drill": True}).to_dict()
    assert reg.validate("control.halt", good) == []
    bad = dict(good, payload={"command": "explode"})
    assert reg.validate("control.halt", bad)
    with pytest.raises(ValueError):
        reg.validate_or_raise("control.halt", bad)


def test_validated_payload_matches_schema():
    reg = SchemaRegistry(ROOT / "schemas")
    v = Verdict("REJECT", 0.8, ["revenue missed"], ["REVENUE_MISS"], [{"url": "https://sec.gov/x", "title": "10-Q", "cited_text": "..."}], "claude-haiku-4-5-20251001", 1234, 0.012)
    env = Envelope("validator", v.to_payload("11111111-2222-4333-8444-555555555555", "ABC", "SHADOW")).to_dict()
    assert reg.validate("signals.validated", env) == []
    e = error_verdict("m", "TIMEOUT", "took too long", 20000)
    assert reg.validate("signals.validated", Envelope("validator", e.to_payload("11111111-2222-4333-8444-555555555555", "ABC", "LIVE")).to_dict()) == []


def test_candidate_facts_are_stable_and_neutral():
    c = {"symbol": "ABC", "sector": "Tech", "session_date": "2026-09-15", "ear_pct": 4.2, "vol_ratio": 2.5, "mom_pct": 77, "next_report_date": None, "thesis_facts": ["EPS 1.00 vs 0.90"], "data_as_of_utc": "now", "event_id": "x"}
    j1 = candidate_facts_json(c)
    c["data_as_of_utc"] = "later"
    assert candidate_facts_json(c) == j1          # volatile fields never reach the prompt
    assert "event_id" not in j1


def test_mock_validator():
    mv = MockValidator({"NVDA"})
    assert mv.validate({"symbol": "ABC", "thesis_facts": []}).verdict == "APPROVE"
    assert mv.validate({"symbol": "NVDA", "thesis_facts": []}).flags == ["OVER_OPTIONED"]
    assert mv.validate({"symbol": "ABC", "thesis_facts": ["1 other 8-K filing(s) known"]}).verdict == "REJECT"


def test_dedupe():
    d = Dedupe(2)
    assert d.first_time("a") and d.first_time("b") and not d.first_time("a")
    assert d.first_time("c") and d.first_time("a")


def _ve(vendor, sym, date, timing, eps_est=1.0, eps_act=None):
    return VendorEvent(vendor, sym, dt.date.fromisoformat(date), timing, "2026Q3", eps_act, eps_est, None, None, {})


def test_feed_merge_and_timing_cross_check(tmp_path):
    fmp = [_ve("fmp", "ABC", "2026-10-28", "AMC"), _ve("fmp", "XYZ", "2026-10-29", "BMO")]
    fh = [_ve("finnhub", "ABC", "2026-10-28", "AMC"), _ve("finnhub", "XYZ", "2026-10-30", "AMC")]
    groups = merge_vendor_events(fmp, fh)
    assert len(groups) == 2
    assert len(groups[("ABC", dt.date(2026, 10, 28))]) == 2
    assert len(groups[("XYZ", dt.date(2026, 10, 29))]) == 2   # +/-1 day tolerated, FMP date wins
    state = FeedState(tmp_path / "s.json")
    today = dt.date(2026, 10, 1)
    p_abc = build_payload("ABC", dt.date(2026, 10, 28), groups[("ABC", dt.date(2026, 10, 28))], state, today, None)
    p_xyz = build_payload("XYZ", dt.date(2026, 10, 29), groups[("XYZ", dt.date(2026, 10, 29))], state, today, dt.date(2027, 1, 28))
    assert p_abc["timing"] == "AMC"
    assert p_xyz["timing"] == "UNKNOWN"      # vendors disagree -> untrusted
    # FMP's stable calendar has no time field: a silent vendor does not veto the other one's timing...
    silent = [_ve("fmp", "QQQ1", "2026-10-28", "UNKNOWN"), _ve("finnhub", "QQQ1", "2026-10-28", "BMO")]
    assert build_payload("QQQ1", dt.date(2026, 10, 28), silent, state, today, None)["timing"] == "BMO"
    # ...but a single vendor alone is never trusted for day 0.
    alone = [_ve("finnhub", "QQQ2", "2026-10-28", "BMO")]
    assert build_payload("QQQ2", dt.date(2026, 10, 28), alone, state, today, None)["timing"] == "UNKNOWN"
    assert p_xyz["next_report_date"] == "2027-01-28"
    assert p_abc["event_id"] == event_id("ABC", "2026Q3", dt.date(2026, 10, 28))
    reg = SchemaRegistry(ROOT / "schemas")
    assert reg.validate("events.earnings", Envelope("events-feed", p_abc).to_dict()) == []


def test_feed_consensus_is_as_of_day_before_and_actuals_never_restated(tmp_path):
    state = FeedState(tmp_path / "s.json")
    src = [_ve("fmp", "ABC", "2026-10-28", "AMC", eps_est=1.00), _ve("finnhub", "ABC", "2026-10-28", "AMC", eps_est=1.00)]
    p1 = build_payload("ABC", dt.date(2026, 10, 28), src, state, dt.date(2026, 10, 20), None)
    assert p1["eps_consensus"] == 1.00 and p1["eps_consensus_asof"] == "2026-10-20"
    src2 = [_ve("fmp", "ABC", "2026-10-28", "AMC", eps_est=1.10)]
    build_payload("ABC", dt.date(2026, 10, 28), src2, state, dt.date(2026, 10, 27), None)
    src3 = [_ve("fmp", "ABC", "2026-10-28", "AMC", eps_est=1.50, eps_act=1.30)]   # estimate moved AFTER the report: must not be used
    p3 = build_payload("ABC", dt.date(2026, 10, 28), src3, state, dt.date(2026, 10, 29), None)
    assert p3["eps_consensus"] == 1.10 and p3["eps_consensus_asof"] == "2026-10-27"
    assert p3["eps_actual"] == 1.30
    src4 = [_ve("fmp", "ABC", "2026-10-28", "AMC", eps_est=1.50, eps_act=1.45)]   # restated actual: first-reported wins
    p4 = build_payload("ABC", dt.date(2026, 10, 28), src4, state, dt.date(2026, 11, 15), None)
    assert p4["eps_actual"] == 1.30
    assert content_hash(p3) == content_hash(p4)


def test_universe_rules_mirror_cpp():
    u = {"min_market_cap_cents": 500000000000, "min_adv20_dollars_cents": 5000000000, "max_median_spread_bps": 5.0, "min_analyst_coverage": 5, "require_transcript": True, "min_listing_age_days": 365, "exclude_etfs": True, "exclude_adrs": True, "exclude_over_optioned": ["NVDA"]}
    ok = {"symbol": "OK", "category": "Domestic Common Stock", "market_cap": "20000000000", "first_listed": "2010-01-04", "analyst_coverage": 12, "transcript_available": 1, "median_spread_bps": 2.0, "sector": "Tech"}
    as_of = dt.date(2026, 9, 15)
    assert exclusion_reason(ok, 200_000_000, as_of, u) == ""
    assert exclusion_reason(dict(ok, symbol="NVDA"), 2e8, as_of, u) == "over_optioned"
    assert exclusion_reason(dict(ok, category="ETF"), 2e8, as_of, u) == "etf"
    assert exclusion_reason(dict(ok, category="ADR Common Stock"), 2e8, as_of, u) == "adr"
    assert exclusion_reason(dict(ok, market_cap="1000000000"), 2e8, as_of, u) == "market_cap"
    assert exclusion_reason(dict(ok, first_listed="2026-06-01"), 2e8, as_of, u) == "ipo_age"
    assert exclusion_reason(dict(ok, analyst_coverage=2), 2e8, as_of, u) == "analyst_coverage"
    assert exclusion_reason(dict(ok, transcript_available=0), 2e8, as_of, u) == "no_transcript"
    assert exclusion_reason(dict(ok, median_spread_bps=9), 2e8, as_of, u) == "spread"
    assert exclusion_reason(ok, 10_000_000, as_of, u) == "adv"
    assert exclusion_reason(ok, None, as_of, u) == "insufficient_price_history"
    snap = build_snapshot([ok, dict(ok, symbol="NVDA")], {"OK": 2e8, "NVDA": 2e8}, as_of, u)
    assert snap["symbols"] == ["OK"] and len(snap["id"]) == 16 and snap["exclusion_reasons"]["NVDA"] == "over_optioned"


def test_counterfactual_math():
    bars = [(f"2026-01-{d:02d}", 10000 + d, 10000 + d * 2) for d in range(1, 30)]
    fr = forward_return(bars, "2026-01-05", 5)
    assert fr == pytest.approx((bars[10][2] - bars[5][1]) / bars[5][1] * 100)
    assert forward_return(bars, "2026-01-28", 5) is None
    assert welch_t([1, 2, 3, 4], [1, 2, 3, 4]) == 0.0
    assert welch_t([5, 6, 7, 8], [1, 2, 3, 4]) > 3
