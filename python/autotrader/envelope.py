"""Message envelope (schemas/v1/envelope.schema.json)."""
from __future__ import annotations

import datetime as dt
import json
import re
import uuid
from dataclasses import dataclass, field
from typing import Any

_UUID_RE = re.compile(r"^[0-9a-f]{8}-[0-9a-f]{4}-[1-8][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$")


def now_utc_iso() -> str:
    return dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z"


def parse_iso_utc(s: str) -> dt.datetime:
    if s.endswith("Z"):
        s = s[:-1] + "+00:00"
    return dt.datetime.fromisoformat(s).astimezone(dt.timezone.utc)


@dataclass
class Envelope:
    producer: str
    payload: dict[str, Any]
    schema_version: str = "1.0"
    msg_id: str = field(default_factory=lambda: str(uuid.uuid4()))
    ts_utc: str = field(default_factory=now_utc_iso)
    correlation_id: str | None = None

    def to_dict(self) -> dict[str, Any]:
        d = {
            "schema_version": self.schema_version,
            "msg_id": self.msg_id,
            "ts_utc": self.ts_utc,
            "producer": self.producer,
            "payload": self.payload,
        }
        if self.correlation_id:
            d["correlation_id"] = self.correlation_id
        return d

    def dumps(self) -> str:
        return json.dumps(self.to_dict(), separators=(",", ":"), sort_keys=False)

    @classmethod
    def from_dict(cls, d: dict[str, Any]) -> "Envelope":
        for k in ("schema_version", "msg_id", "ts_utc", "producer"):
            if not isinstance(d.get(k), str):
                raise ValueError(f"envelope: missing {k}")
        if not _UUID_RE.match(d["msg_id"]):
            raise ValueError("envelope: msg_id is not a uuid")
        parse_iso_utc(d["ts_utc"])
        if not isinstance(d.get("payload"), dict):
            raise ValueError("envelope: payload missing")
        return cls(
            producer=d["producer"],
            payload=d["payload"],
            schema_version=d["schema_version"],
            msg_id=d["msg_id"],
            ts_utc=d["ts_utc"],
            correlation_id=d.get("correlation_id"),
        )

    @classmethod
    def loads(cls, raw: bytes | str) -> "Envelope":
        return cls.from_dict(json.loads(raw))
