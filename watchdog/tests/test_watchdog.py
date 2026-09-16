"""Watchdog unit tests with a stub Alpaca (no network)."""
from __future__ import annotations

import json
from pathlib import Path

from watchdog.main import Watchdog


class StubAlpaca:
    def __init__(self, equity=1000.0, flat_after=1):
        self.equity = equity
        self.calls = []
        self.flat_after = flat_after
        self._kill_calls = 0

    def account(self):
        self.calls.append("account")
        return {"equity": str(self.equity)}

    def kill(self, attempts=5):
        self._kill_calls += 1
        self.calls.append("kill")
        return {"attempts": 1, "flat": self._kill_calls >= self.flat_after, "errors": [], "positions_left": 0, "orders_left": 0}


class FakeClock:
    def __init__(self):
        self.t = 1000.0

    def __call__(self):
        return self.t


def make(tmp_path: Path, **over):
    cfg = {"alpaca_base_url": "http://x", "alpaca_key_id": "k", "alpaca_secret_key": "s", "heartbeat_interval_s": 10, "miss_threshold": 6, "drawdown_flatten_pct": -12.0,
           "state_file": str(tmp_path / "state.json"), "nats_url": "nats://127.0.0.1:1"}
    cfg.update(over)
    clock = FakeClock()
    stub = StubAlpaca()
    return Watchdog(cfg, alpaca=stub, clock=clock), stub, clock


def test_cold_start_does_not_kill(tmp_path):
    wd, stub, clock = make(tmp_path)
    clock.t += 3600
    assert wd.check_heartbeat() is None
    assert "kill" not in stub.calls


def test_heartbeat_loss_kills_once(tmp_path):
    wd, stub, clock = make(tmp_path)
    wd.heartbeat({"seq": 1})
    clock.t += 59
    assert wd.check_heartbeat() is None
    clock.t += 2
    rep = wd.check_heartbeat()
    assert rep and rep["trigger"] == "HEARTBEAT_LOSS" and rep["flat"]
    assert wd.killed is not None
    clock.t += 100
    assert wd.check_heartbeat() is None          # latched; no kill loop
    assert stub.calls.count("kill") == 1
    st = json.loads((tmp_path / "state.json").read_text())
    assert st["killed"]["trigger"] == "HEARTBEAT_LOSS"


def test_drawdown_uses_own_hwm(tmp_path):
    wd, stub, clock = make(tmp_path)
    stub.equity = 1000.0
    assert wd.check_account() is None
    stub.equity = 900.0
    assert wd.check_account() is None            # -10%: above the -12% flatten line
    stub.equity = 879.0
    rep = wd.check_account()
    assert rep and rep["trigger"] == "DRAWDOWN_FLATTEN"
    assert wd.hwm == 1000.0


def test_alerts_route_correctly(tmp_path):
    wd, stub, clock = make(tmp_path)
    assert wd.alert("VALIDATOR_ERROR_STREAK", "3 errors")["action"] == "pause_new"
    assert "kill" not in stub.calls
    rep = wd.alert("RECONCILE_MISMATCH", "qty differs", "reconciler")
    assert rep["trigger"] == "RECONCILE_MISMATCH" and "kill" in stub.calls
    assert wd.alert("SOMETHING_ELSE", "x")["action"] == "ignored"


def test_drill_runs_kill_path_without_latching(tmp_path):
    wd, stub, clock = make(tmp_path)
    wd.heartbeat({"seq": 1})
    rep = wd.drill()
    assert rep["drill"] and rep["flat"] and rep["trigger"] == "DRILL"
    assert wd.killed is None
    assert wd.status()["events"][-1]["kind"] == "kill"


def test_resume_clears_latch(tmp_path):
    wd, stub, clock = make(tmp_path)
    wd.kill("MANUAL", "op", "operator")
    assert wd.killed
    wd.resume()
    assert wd.killed is None
