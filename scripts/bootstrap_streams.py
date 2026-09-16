#!/usr/bin/env python
"""Create or update every JetStream stream from schemas/topics.json (idempotent).

usage: python scripts/bootstrap_streams.py [--nats nats://127.0.0.1:4222]
"""
from __future__ import annotations

import argparse
import asyncio
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))

from autotrader.bus import Bus  # noqa: E402
from autotrader.schemas import SchemaRegistry  # noqa: E402


async def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--nats", default="nats://127.0.0.1:4222")
    a = ap.parse_args()
    reg = SchemaRegistry(Path(__file__).resolve().parents[1] / "schemas")
    bus = Bus(a.nats, "bootstrap", reg)
    await bus.connect()
    await bus.ensure_streams()
    for s in reg.streams:
        info = await bus.js.stream_info(s["name"])
        print(f"{s['name']:<10} subjects={s['subjects']} msgs={info.state.messages} max_age={s['max_age_days']}d max_msgs={s['max_msgs']}")
    await bus.close()


if __name__ == "__main__":
    asyncio.run(main())
