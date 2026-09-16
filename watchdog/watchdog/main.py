"""Watchdog / kill switch (docs/DESIGN.md section 6).

Runs on a different host/region with its OWN Alpaca key. Listens for
heartbeats from the portfolio service (HTTP POST /heartbeat every 10 s).
Triggers cancel-all + close-all through Alpaca REST, then announces
control.halt on the bus best-effort:

  * heartbeat loss     miss 6 beats (60 s)
  * drawdown breach    equity / high-water-mark - 1 <= -12% (own HWM, persisted)
  * reconcile mismatch POST /alert {"trigger": "RECONCILE_MISMATCH"} from the reconciler
  * validator streak   POST /alert {"trigger": "VALIDATOR_ERROR_STREAK"}  (pause only; no kill)
  * manual             POST /halt
  * drill              POST /drill  (weekly; runs the full kill path against the paper account)

Config JSON (path in argv or WATCHDOG_CONFIG):
{
  "listen": "127.0.0.1:8787", "token": "...", "alpaca_base_url": "https://paper-api.alpaca.markets",
  "alpaca_key_id": "...", "alpaca_secret_key": "...",         (or env WATCHDOG_ALPACA_KEY_ID / WATCHDOG_ALPACA_SECRET_KEY)
  "heartbeat_interval_s": 10, "miss_threshold": 6, "drawdown_flatten_pct": -12.0,
  "account_poll_s": 30, "state_file": "var/watchdog_state.json", "nats_url": "nats://127.0.0.1:4222",
  "require_reconciled_heartbeats": true
}
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import logging
import os
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from .alpaca import Alpaca
from .natspub import publish

log = logging.getLogger("watchdog")

KILL_TRIGGERS = {"HEARTBEAT_LOSS", "DRAWDOWN_FLATTEN", "RECONCILE_MISMATCH", "MANUAL", "DRILL"}
PAUSE_ONLY_TRIGGERS = {"VALIDATOR_ERROR_STREAK", "DATA_STALE", "REJECT_RATE", "DAILY_LOSS"}


def now_iso() -> str:
    return dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z"


class Watchdog:
    def __init__(self, cfg: dict, alpaca: Alpaca | None = None, clock=time.monotonic):
        self.cfg = cfg
        self.clock = clock
        self.alpaca = alpaca or Alpaca(cfg["alpaca_base_url"], cfg.get("alpaca_key_id") or os.environ["WATCHDOG_ALPACA_KEY_ID"], cfg.get("alpaca_secret_key") or os.environ["WATCHDOG_ALPACA_SECRET_KEY"])
        self.interval = float(cfg.get("heartbeat_interval_s", 10))
        self.miss_threshold = int(cfg.get("miss_threshold", 6))
        self.dd_flatten = float(cfg.get("drawdown_flatten_pct", -12.0))
        self.state_file = Path(cfg.get("state_file", "var/watchdog_state.json"))
        self.lock = threading.Lock()
        self.last_beat = clock()
        self.last_beat_payload: dict = {}
        self.beats = 0
        self.armed = False           # arms on the first heartbeat, so a cold start does not kill
        self.killed: dict | None = None
        self.hwm = 0.0
        self.equity = 0.0
        self.events: list[dict] = []
        self._load()

    # ---- persistence ----
    def _load(self) -> None:
        try:
            s = json.loads(self.state_file.read_text(encoding="utf-8"))
            self.hwm = float(s.get("hwm", 0.0))
            self.killed = s.get("killed")
        except FileNotFoundError:
            pass

    def _save(self) -> None:
        self.state_file.parent.mkdir(parents=True, exist_ok=True)
        self.state_file.write_text(json.dumps({"hwm": self.hwm, "killed": self.killed, "saved_at": now_iso()}), encoding="utf-8")

    # ---- inputs ----
    def heartbeat(self, payload: dict) -> None:
        with self.lock:
            self.last_beat = self.clock()
            self.last_beat_payload = payload
            self.beats += 1
            self.armed = True

    def alert(self, trigger: str, reason: str, source: str = "unknown") -> dict:
        self._event("alert", trigger, reason, source)
        if trigger in KILL_TRIGGERS:
            return self.kill(trigger, reason, source)
        if trigger in PAUSE_ONLY_TRIGGERS:
            publish(self.cfg.get("nats_url", ""), "control.pause_new", self._control("pause_new", trigger, reason, source))
            return {"action": "pause_new", "trigger": trigger}
        return {"action": "ignored", "trigger": trigger}

    # ---- checks (called by the monitor thread) ----
    def check_heartbeat(self) -> dict | None:
        with self.lock:
            if not self.armed or self.killed:
                return None
            silence = self.clock() - self.last_beat
        if silence > self.interval * self.miss_threshold:
            return self.kill("HEARTBEAT_LOSS", f"no heartbeat for {silence:.0f}s (> {self.interval * self.miss_threshold:.0f}s)", "watchdog")
        return None

    def check_account(self) -> dict | None:
        try:
            acct = self.alpaca.account()
            eq = float(acct["equity"])
        except Exception as e:  # noqa: BLE001
            log.warning("account poll failed: %s", e)
            return None
        with self.lock:
            self.equity = eq
            if eq > self.hwm:
                self.hwm = eq
            dd = (eq / self.hwm - 1.0) * 100.0 if self.hwm > 0 else 0.0
        self._save()
        if dd <= self.dd_flatten and not self.killed:
            return self.kill("DRAWDOWN_FLATTEN", f"drawdown {dd:.2f}% <= {self.dd_flatten}% (equity {eq:.2f}, hwm {self.hwm:.2f})", "watchdog")
        return None

    # ---- action path: Alpaca REST first, bus second ----
    def kill(self, trigger: str, reason: str, source: str, drill: bool = False) -> dict:
        log.critical("KILL trigger=%s reason=%s source=%s drill=%s", trigger, reason, source, drill)
        report = self.alpaca.kill()
        report.update({"trigger": trigger, "reason": reason, "source": source, "at": now_iso(), "drill": drill})
        with self.lock:
            if not drill:
                self.killed = report
        self._save()
        self._event("kill", trigger, reason, source, report)
        ctl = self._control("halt", trigger, reason, source, drill)
        ctl["details"] = {"kill_report": {k: v for k, v in report.items() if k != "errors"} | {"errors": report.get("errors", [])[:5]}}
        report["bus_announced"] = publish(self.cfg.get("nats_url", ""), "control.halt", ctl)
        if not report["flat"]:
            log.critical("KILL DID NOT VERIFY FLAT after %d attempts: %s", report["attempts"], report["errors"])
        return report

    def drill(self) -> dict:
        """Weekly paper drill: exercise the real kill path, then report. Untested = nonexistent."""
        return self.kill("DRILL", "scheduled watchdog drill", "drill", drill=True)

    def resume(self) -> None:
        with self.lock:
            self.killed = None
            self.last_beat = self.clock()
        self._save()

    # ---- misc ----
    def _control(self, command: str, trigger: str, reason: str, source: str, drill: bool = False) -> dict:
        return {"command": command, "reason": reason[:500], "source": "drill" if drill else "watchdog", "issued_at_utc": now_iso(), "trigger": trigger, "details": {"origin": source}, "drill": drill}

    def _event(self, kind: str, trigger: str, reason: str, source: str, extra: dict | None = None) -> None:
        self.events.append({"kind": kind, "trigger": trigger, "reason": reason, "source": source, "at": now_iso(), **({"report": extra} if extra else {})})
        self.events = self.events[-200:]

    def status(self) -> dict:
        with self.lock:
            return {"armed": self.armed, "beats": self.beats, "seconds_since_beat": round(self.clock() - self.last_beat, 1), "last_beat": self.last_beat_payload,
                    "killed": self.killed, "equity": self.equity, "hwm": self.hwm, "drawdown_pct": (self.equity / self.hwm - 1) * 100 if self.hwm else 0.0, "events": self.events[-20:]}


def make_handler(wd: Watchdog, token: str):
    class H(BaseHTTPRequestHandler):
        def _auth(self) -> bool:
            return self.headers.get("Authorization", "") == f"Bearer {token}"

        def _json(self, code: int, obj) -> None:
            body = json.dumps(obj).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def _body(self) -> dict:
            n = int(self.headers.get("Content-Length", "0") or 0)
            return json.loads(self.rfile.read(n) or b"{}")

        def do_GET(self):  # noqa: N802
            if self.path == "/status":
                return self._json(200, wd.status())
            if self.path == "/healthz":
                return self._json(200, {"ok": True})
            self._json(404, {"error": "not found"})

        def do_POST(self):  # noqa: N802
            if not self._auth():
                return self._json(401, {"error": "unauthorized"})
            try:
                body = self._body()
            except json.JSONDecodeError:
                return self._json(400, {"error": "bad json"})
            if self.path == "/heartbeat":
                wd.heartbeat(body)
                return self._json(200, {"ok": True, "beats": wd.beats})
            if self.path == "/alert":
                # Acknowledge immediately; the kill path runs on its own thread so the caller never blocks on it.
                threading.Thread(target=wd.alert, args=(body.get("trigger", ""), body.get("reason", ""), body.get("source", "unknown")), daemon=True).start()
                return self._json(202, {"accepted": True, "trigger": body.get("trigger", "")})
            if self.path == "/halt":
                threading.Thread(target=wd.kill, args=("MANUAL", body.get("reason", "manual halt"), body.get("source", "operator")), daemon=True).start()
                return self._json(202, {"accepted": True, "trigger": "MANUAL"})
            if self.path == "/drill":
                return self._json(200, wd.drill())
            if self.path == "/resume":
                wd.resume()
                return self._json(200, {"ok": True})
            self._json(404, {"error": "not found"})

        def log_message(self, fmt, *args):  # quieter
            log.debug("http %s", fmt % args)

    return H


def monitor_loop(wd: Watchdog, stop: threading.Event, account_poll_s: float) -> None:
    last_acct = 0.0
    while not stop.is_set():
        try:
            wd.check_heartbeat()
            if time.monotonic() - last_acct >= account_poll_s:
                last_acct = time.monotonic()
                wd.check_account()
        except Exception as e:  # noqa: BLE001
            log.exception("monitor loop error: %s", e)
        stop.wait(1.0)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("config", nargs="?", default=os.environ.get("WATCHDOG_CONFIG", "config/watchdog.json"))
    ap.add_argument("--log-level", default="INFO")
    a = ap.parse_args()
    logging.basicConfig(level=a.log_level, format="%(asctime)s %(name)s %(levelname)s %(message)s")
    cfg = json.loads(Path(a.config).read_text(encoding="utf-8"))
    wd = Watchdog(cfg)
    host, port = cfg.get("listen", "127.0.0.1:8787").split(":")
    srv = ThreadingHTTPServer((host, int(port)), make_handler(wd, cfg["token"]))
    stop = threading.Event()
    t = threading.Thread(target=monitor_loop, args=(wd, stop, float(cfg.get("account_poll_s", 30))), daemon=True)
    t.start()
    log.info("watchdog listening on %s:%s (heartbeat every %ss, kill after %d misses, flatten at %s%%)", host, port, wd.interval, wd.miss_threshold, wd.dd_flatten)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        srv.server_close()
    sys.exit(0)


if __name__ == "__main__":
    main()
