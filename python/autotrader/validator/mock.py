"""Deterministic paper-trading stand-in for the Claude validator.

Uses only what is in the candidate payload (no market data, no web). The
backtester's C++ mock (cpp/src/backtester/mock_validator.cpp) is the richer
one; this exists so the paper pipeline can run end to end with no API key.
"""
from __future__ import annotations

import time
from typing import Any

from .claude_validator import Verdict


class MockValidator:
    def __init__(self, over_optioned: set[str] | None = None):
        self.over_optioned = set(over_optioned or [])

    def validate(self, candidate: dict[str, Any]) -> Verdict:
        t0 = time.monotonic()
        flags: list[str] = []
        reasons: list[str] = []
        sym = candidate.get("symbol", "")
        if sym in self.over_optioned:
            flags.append("OVER_OPTIONED")
            reasons.append("symbol is in the over-optioned exclusion set")
        for fact in candidate.get("thesis_facts", []):
            if "8-K" in fact:
                flags.append("SECOND_8K_IN_WINDOW")
                reasons.append(fact)
                break
        verdict = "REJECT" if flags else "APPROVE"
        if not reasons:
            reasons.append("no mock rule triggered")
        return Verdict(verdict, 0.9 if flags else 0.5, reasons, flags, [], "mock", int((time.monotonic() - t0) * 1000), 0.0, prompt_version="mock-paper-v1")
