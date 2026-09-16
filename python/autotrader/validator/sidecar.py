"""Validator sidecar: signals.candidate -> signals.validated.

Modes (config validator.mode):
  SHADOW  Claude labels every candidate; the risk manager records and ignores the label (mandatory first phase).
  LIVE    anything that is not APPROVE is a reject at the risk manager.
  MOCK    deterministic stand-in, no API key needed.

After `risk.validator_error_streak_pause` consecutive ERROR verdicts the
sidecar publishes control.pause_new (the risk manager does the same on its own
side; both fire so a dead sidecar and a broken API are both loud).

Run: at-validator --config config/paper.json
"""
from __future__ import annotations

import argparse
import asyncio
import json
import logging
from pathlib import Path

from ..bus import Bus
from ..config import Config, Secrets
from ..envelope import Envelope, now_utc_iso
from ..schemas import SchemaRegistry
from .claude_validator import ClaudeValidator, ValidatorConfig, Verdict
from .mock import MockValidator

log = logging.getLogger("autotrader.validator.sidecar")


class Sidecar:
    def __init__(self, cfg: Config, secrets: Secrets | None):
        self.cfg = cfg
        self.mode = str(cfg.get("validator.mode", "SHADOW")).upper()
        if self.mode not in ("SHADOW", "LIVE", "MOCK"):
            raise ValueError(f"validator.mode must be SHADOW|LIVE|MOCK, got {self.mode}")
        self.reg = SchemaRegistry(cfg.resolve(cfg.get("schemas_dir", "schemas")))
        self.bus = Bus(cfg.get("nats_url", "nats://127.0.0.1:4222"), "validator", self.reg)
        risk = json.loads(cfg.resolve(cfg.get("risk_config", "config/risk.v1.json")).read_text(encoding="utf-8"))
        strat = json.loads(cfg.resolve(cfg.get("strategy_config", "config/strategy.v1.json")).read_text(encoding="utf-8"))
        self.error_streak_pause = int(risk.get("validator_error_streak_pause", 3))
        self.daily_budget_usd = float(cfg.get("validator.daily_budget_usd", 5.0))
        self.error_streak = 0
        self.paused_emitted = False
        self.spent_today = 0.0
        self.spent_day = ""
        self.state_path: Path = cfg.resolve(cfg.get("state_dir", "var/state")) / "validator_state.json"
        if self.mode == "MOCK":
            self.impl: ClaudeValidator | MockValidator = MockValidator(set(strat.get("universe", {}).get("exclude_over_optioned", [])))
        else:
            if secrets is None:
                raise RuntimeError("validator: secrets required for SHADOW/LIVE")
            self.impl = ClaudeValidator(secrets.require("anthropic_api_key"), ValidatorConfig.from_config(cfg))
        self._load_state()

    # ---- state ----
    def _load_state(self) -> None:
        try:
            s = json.loads(self.state_path.read_text(encoding="utf-8"))
            self.error_streak = int(s.get("error_streak", 0))
            self.spent_today = float(s.get("spent_today", 0.0))
            self.spent_day = s.get("spent_day", "")
        except FileNotFoundError:
            pass

    def _save_state(self) -> None:
        self.state_path.parent.mkdir(parents=True, exist_ok=True)
        self.state_path.write_text(json.dumps({"error_streak": self.error_streak, "spent_today": self.spent_today, "spent_day": self.spent_day}), encoding="utf-8")

    # ---- handlers ----
    async def on_candidate(self, subject: str, env: Envelope) -> None:
        today = now_utc_iso()[:10]
        if today != self.spent_day:
            self.spent_day, self.spent_today = today, 0.0
        cand = env.payload
        if self.spent_today >= self.daily_budget_usd and self.mode != "MOCK":
            verdict = Verdict("ERROR", 0.0, [f"daily validator budget ${self.daily_budget_usd:.2f} exhausted"], ["API_ERROR"], [], getattr(self.impl, "cfg", None).model if hasattr(self.impl, "cfg") else "mock", 0, 0.0)
        else:
            verdict = await asyncio.to_thread(self.impl.validate, cand)
        self.spent_today += verdict.cost_usd
        payload = verdict.to_payload(env.msg_id, cand.get("symbol", ""), self.mode)
        await self.bus.publish("signals.validated", Envelope("validator", payload, correlation_id=env.msg_id))
        log.info("%s %s verdict=%s flags=%s latency=%dms cost=$%.4f", self.mode, cand.get("symbol"), verdict.verdict, verdict.flags, verdict.latency_ms, verdict.cost_usd)

        if verdict.verdict == "ERROR":
            self.error_streak += 1
            if self.error_streak >= self.error_streak_pause and not self.paused_emitted:
                self.paused_emitted = True
                await self.bus.publish("control.pause_new", Envelope("validator", {
                    "command": "pause_new", "reason": f"validator returned ERROR {self.error_streak} times in a row: {verdict.reasons[0] if verdict.reasons else ''}",
                    "source": "validator", "issued_at_utc": now_utc_iso(), "trigger": "VALIDATOR_ERROR_STREAK", "details": {"flags": verdict.flags}, "drill": False,
                }))
        else:
            self.error_streak = 0
            self.paused_emitted = False
        self._save_state()

    async def on_control(self, subject: str, env: Envelope) -> None:
        if env.payload.get("command") == "resume":
            self.error_streak = 0
            self.paused_emitted = False
            self._save_state()

    async def run(self) -> None:
        await self.bus.connect()
        await self.bus.subscribe("signals.candidate", "validator-candidates", self.on_candidate)
        await self.bus.subscribe("control.resume", "validator-control", self.on_control)
        log.info("validator sidecar up: mode=%s impl=%s", self.mode, type(self.impl).__name__)
        await self.bus.run()


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", default=None)
    ap.add_argument("--log-level", default="INFO")
    a = ap.parse_args()
    logging.basicConfig(level=a.log_level, format="%(asctime)s %(name)s %(levelname)s %(message)s")
    cfg = Config.load(a.config)
    mode = str(cfg.get("validator.mode", "SHADOW")).upper()
    secrets = None if mode == "MOCK" else Secrets.load(cfg)
    asyncio.run(Sidecar(cfg, secrets).run())


if __name__ == "__main__":
    main()
