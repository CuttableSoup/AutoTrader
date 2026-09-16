"""Two-step Claude validator (docs/DESIGN.md section 4).

Step 1: web search + reasoning with citations (server tool, max_uses=3, domain
        allowlist). Output: a factual summary + citations.
Step 2: strict-JSON verdict via output_config.format, no tools.

Any exception or the 20 s deadline -> verdict ERROR (the risk manager treats it
as a reject and pages after three in a row). This module never touches orders.
"""
from __future__ import annotations

import json
import logging
import time
from dataclasses import dataclass, field
from typing import Any

import anthropic

from . import prompts

log = logging.getLogger("autotrader.validator")

# USD per million tokens. Re-verify at signup (docs/DESIGN.md section 13).
PRICES = {
    "claude-haiku-4-5": {"in": 1.00, "out": 5.00, "cache_read": 0.10, "cache_write": 1.25},
    "claude-sonnet-5": {"in": 2.00, "out": 10.00, "cache_read": 0.20, "cache_write": 2.50},
    "claude-opus-5": {"in": 5.00, "out": 25.00, "cache_read": 0.50, "cache_write": 6.25},
}
WEB_SEARCH_USD_PER_CALL = 0.01  # $10 per 1,000 searches


def _price_key(model: str) -> str:
    for k in PRICES:
        if model.startswith(k):
            return k
    return "claude-haiku-4-5"


def _web_search_tool_type(model: str) -> str:
    # The dynamic-filtering variant needs Opus 4.6+/Sonnet 4.6+; Haiku 4.5 uses the basic tool.
    return "web_search_20250305" if model.startswith("claude-haiku") else "web_search_20260209"


@dataclass
class ValidatorConfig:
    model: str = "claude-haiku-4-5-20251001"
    timeout_s: float = 20.0
    max_search_uses: int = 3
    allowed_domains: list[str] = field(default_factory=lambda: ["sec.gov", "reuters.com", "bloomberg.com", "wsj.com", "ft.com", "apnews.com", "businesswire.com", "prnewswire.com", "globenewswire.com"])
    injection_prescreen: bool = True
    include_transcript: bool = False
    max_tokens_research: int = 2048
    max_tokens_verdict: int = 512
    workspace_id: str = ""            # required when the API key is org-level (not scoped to a workspace)

    @classmethod
    def from_config(cls, cfg: Any) -> "ValidatorConfig":
        c = cls()
        c.model = cfg.get("validator.model", c.model)
        c.workspace_id = str(cfg.get("validator.anthropic_workspace_id", "") or "")
        c.timeout_s = float(cfg.get("validator.timeout_s", c.timeout_s))
        c.max_search_uses = int(cfg.get("validator.max_search_uses", c.max_search_uses))
        doms = cfg.get("validator.allowed_domains", c.allowed_domains)
        c.allowed_domains = [d for d in doms if "*" not in d]  # the tool takes plain hostnames; subdomains match automatically
        c.injection_prescreen = bool(cfg.get("validator.injection_prescreen", c.injection_prescreen))
        c.include_transcript = bool(cfg.get("validator.include_transcript", c.include_transcript))
        return c


@dataclass
class Verdict:
    verdict: str                      # APPROVE | REJECT | ERROR
    confidence: float
    reasons: list[str]
    flags: list[str]
    citations: list[dict[str, str]]
    model: str
    latency_ms: int
    cost_usd: float
    research_summary: str = ""
    prompt_version: str = prompts.PROMPT_VERSION

    def to_payload(self, candidate_msg_id: str, symbol: str, mode: str) -> dict[str, Any]:
        return {
            "candidate_msg_id": candidate_msg_id,
            "symbol": symbol,
            "verdict": self.verdict,
            "confidence": max(0.0, min(1.0, float(self.confidence))),
            "reasons": [r[:500] for r in self.reasons][:10],
            "flags": self.flags[:20],
            "model": self.model,
            "latency_ms": int(self.latency_ms),
            "cost_usd": round(float(self.cost_usd), 6),
            "citations": self.citations[:20],
            "mode": mode,
            "prompt_version": self.prompt_version,
        }


def error_verdict(model: str, flag: str, reason: str, latency_ms: int, cost_usd: float = 0.0) -> Verdict:
    return Verdict("ERROR", 0.0, [reason[:500]], [flag], [], model, latency_ms, cost_usd)


def _usage_cost(usage: Any, model: str) -> float:
    p = PRICES[_price_key(model)]
    cost = 0.0
    cost += (getattr(usage, "input_tokens", 0) or 0) / 1e6 * p["in"]
    cost += (getattr(usage, "output_tokens", 0) or 0) / 1e6 * p["out"]
    cost += (getattr(usage, "cache_read_input_tokens", 0) or 0) / 1e6 * p["cache_read"]
    cost += (getattr(usage, "cache_creation_input_tokens", 0) or 0) / 1e6 * p["cache_write"]
    stu = getattr(usage, "server_tool_use", None)
    if stu is not None:
        cost += (getattr(stu, "web_search_requests", 0) or 0) * WEB_SEARCH_USD_PER_CALL
    return cost


def candidate_facts_json(candidate: dict[str, Any]) -> str:
    """JSON-encode the neutral facts only (no ids, no timestamps -> stable, safe)."""
    keep = {k: candidate.get(k) for k in ("symbol", "sector", "session_date", "ear_pct", "vol_ratio", "mom_pct", "next_report_date", "thesis_facts")}
    return json.dumps(keep, sort_keys=True, ensure_ascii=True)


class ClaudeValidator:
    def __init__(self, api_key: str, cfg: ValidatorConfig):
        self.cfg = cfg
        headers = {"anthropic-workspace-id": cfg.workspace_id} if cfg.workspace_id else None
        self.client = anthropic.Anthropic(api_key=api_key, timeout=cfg.timeout_s, max_retries=0, default_headers=headers)

    # ---- step 1 -------------------------------------------------------------
    def research(self, facts_json: str, deadline: float) -> tuple[str, list[dict[str, str]], float]:
        tool: dict[str, Any] = {"type": _web_search_tool_type(self.cfg.model), "name": "web_search", "max_uses": self.cfg.max_search_uses}
        if self.cfg.allowed_domains:
            tool["allowed_domains"] = self.cfg.allowed_domains
        messages: list[dict[str, Any]] = [{"role": "user", "content": prompts.research_user_message(facts_json)}]
        summary_parts: list[str] = []
        citations: list[dict[str, str]] = []
        cost = 0.0
        for _ in range(4):  # pause_turn restarts
            remaining = deadline - time.monotonic()
            if remaining <= 0.5:
                raise TimeoutError("research: deadline exhausted")
            resp = self.client.with_options(timeout=remaining).messages.create(
                model=self.cfg.model,
                max_tokens=self.cfg.max_tokens_research,
                system=[{"type": "text", "text": prompts.RESEARCH_SYSTEM, "cache_control": {"type": "ephemeral"}}],
                tools=[tool],
                messages=messages,
            )
            cost += _usage_cost(resp.usage, self.cfg.model)
            for block in resp.content:
                if block.type == "text":
                    summary_parts.append(block.text)
                    for c in getattr(block, "citations", None) or []:
                        if getattr(c, "type", "") == "web_search_result_location":
                            citations.append({"url": c.url, "title": (c.title or "")[:200], "cited_text": (c.cited_text or "")[:1000]})
                elif block.type == "web_search_tool_result":
                    content = block.content
                    if not isinstance(content, list):  # error object, not a result list
                        summary_parts.append(f"[web search error: {getattr(content, 'error_code', 'unknown')}]")
            if resp.stop_reason == "pause_turn":
                messages.append({"role": "assistant", "content": resp.content})
                continue
            if resp.stop_reason == "refusal":
                raise RuntimeError("research: model refused")
            break
        return "\n".join(summary_parts).strip(), citations, cost

    # ---- optional pre-screen ------------------------------------------------
    def prescreen(self, summary: str, deadline: float) -> tuple[bool, float]:
        remaining = deadline - time.monotonic()
        if remaining <= 0.5:
            raise TimeoutError("prescreen: deadline exhausted")
        resp = self.client.with_options(timeout=remaining).messages.create(
            model=self.cfg.model,
            max_tokens=256,
            system=[{"type": "text", "text": prompts.PRESCREEN_SYSTEM, "cache_control": {"type": "ephemeral"}}],
            messages=[{"role": "user", "content": "<text>\n" + summary + "\n</text>"}],
            output_config={"format": {"type": "json_schema", "schema": prompts.PRESCREEN_SCHEMA}},
        )
        text = next(b.text for b in resp.content if b.type == "text")
        return bool(json.loads(text)["injection_suspected"]), _usage_cost(resp.usage, self.cfg.model)

    # ---- step 2 -------------------------------------------------------------
    def verdict(self, facts_json: str, summary: str, deadline: float) -> tuple[dict[str, Any], float]:
        remaining = deadline - time.monotonic()
        if remaining <= 0.5:
            raise TimeoutError("verdict: deadline exhausted")
        resp = self.client.with_options(timeout=remaining).messages.create(
            model=self.cfg.model,
            max_tokens=self.cfg.max_tokens_verdict,
            system=[{"type": "text", "text": prompts.VERDICT_SYSTEM, "cache_control": {"type": "ephemeral"}}],
            messages=[{"role": "user", "content": prompts.verdict_user_message(facts_json, summary)}],
            output_config={"format": {"type": "json_schema", "schema": prompts.VERDICT_SCHEMA}},
        )
        if resp.stop_reason == "refusal":
            raise RuntimeError("verdict: model refused")
        text = next(b.text for b in resp.content if b.type == "text")
        return json.loads(text), _usage_cost(resp.usage, self.cfg.model)

    # ---- full pipeline ------------------------------------------------------
    def validate(self, candidate: dict[str, Any]) -> Verdict:
        t0 = time.monotonic()
        deadline = t0 + self.cfg.timeout_s
        facts = candidate_facts_json(candidate)
        cost = 0.0
        try:
            summary, citations, c1 = self.research(facts, deadline)
            cost += c1
            if self.cfg.injection_prescreen and summary:
                suspected, c_pre = self.prescreen(summary, deadline)
                cost += c_pre
                if suspected:
                    ms = int((time.monotonic() - t0) * 1000)
                    return Verdict("REJECT", 0.9, ["research text failed the injection pre-screen"], ["INJECTION_SUSPECTED"], citations, self.cfg.model, ms, cost, summary)
            data, c2 = self.verdict(facts, summary or "No research summary was produced.", deadline)
            cost += c2
            ms = int((time.monotonic() - t0) * 1000)
            flags = list(data.get("flags", []))
            verdict = data["verdict"]
            if data.get("injection_suspected"):
                flags.append("INJECTION_SUSPECTED")
                verdict = "REJECT"
            return Verdict(verdict, float(data.get("confidence", 0.0)), list(data.get("reasons", [])), flags, citations, self.cfg.model, ms, cost, summary)
        except (TimeoutError, anthropic.APITimeoutError) as e:
            return error_verdict(self.cfg.model, "TIMEOUT", f"timeout: {e}", int((time.monotonic() - t0) * 1000), cost)
        except anthropic.RateLimitError as e:
            return error_verdict(self.cfg.model, "API_ERROR", f"rate limited: {e}", int((time.monotonic() - t0) * 1000), cost)
        except anthropic.APIStatusError as e:
            return error_verdict(self.cfg.model, "API_ERROR", f"api status {e.status_code}: {e.message}", int((time.monotonic() - t0) * 1000), cost)
        except anthropic.APIConnectionError as e:
            return error_verdict(self.cfg.model, "API_ERROR", f"connection: {e}", int((time.monotonic() - t0) * 1000), cost)
        except (json.JSONDecodeError, KeyError, StopIteration, ValueError) as e:
            return error_verdict(self.cfg.model, "SCHEMA_ERROR", f"bad model output: {e}", int((time.monotonic() - t0) * 1000), cost)
        except Exception as e:  # noqa: BLE001 - the sidecar must never die on one candidate
            log.exception("validator failure")
            return error_verdict(self.cfg.model, "API_ERROR", f"unexpected: {e}", int((time.monotonic() - t0) * 1000), cost)
