"""Logger: durable consumer on every stream, JSONL per subject per day, gzip
after rotation. This is the >= 2 year archive (docs/DESIGN.md section 10) and
the raw material for post-mortems and the Claude counterfactual.

Run: at-logger --config config/paper.json
"""
from __future__ import annotations

import argparse
import asyncio
import gzip
import logging
import shutil
from pathlib import Path

from .bus import Bus
from .config import Config
from .envelope import Envelope
from .schemas import SchemaRegistry

log = logging.getLogger("autotrader.logger")


class JsonlArchive:
    def __init__(self, root: Path):
        self.root = root
        self._open: dict[tuple[str, str], object] = {}

    def _path(self, subject: str, day: str) -> Path:
        return self.root / subject / f"{day}.jsonl"

    def write(self, subject: str, env: Envelope) -> None:
        day = env.ts_utc[:10]
        key = (subject, day)
        fh = self._open.get(key)
        if fh is None:
            p = self._path(subject, day)
            p.parent.mkdir(parents=True, exist_ok=True)
            fh = open(p, "a", encoding="utf-8")  # noqa: SIM115
            self._open[key] = fh
            self._rotate(subject, day)
        fh.write(env.dumps() + "\n")
        fh.flush()

    def _rotate(self, subject: str, current_day: str) -> None:
        for key in [k for k in self._open if k[0] == subject and k[1] != current_day]:
            self._open.pop(key).close()
            p = self._path(*key)
            if p.exists():
                with open(p, "rb") as src, gzip.open(str(p) + ".gz", "wb") as dst:
                    shutil.copyfileobj(src, dst)
                p.unlink()

    def close(self) -> None:
        for fh in self._open.values():
            fh.close()
        self._open.clear()


async def run(cfg: Config) -> None:
    reg = SchemaRegistry(cfg.resolve(cfg.get("schemas_dir", "schemas")))
    archive = JsonlArchive(cfg.resolve(cfg.get("log_dir", "var/log")) / "topics")
    bus = Bus(cfg.get("nats_url", "nats://127.0.0.1:4222"), "logger", reg, validate_outgoing=False)
    await bus.connect()
    await bus.ensure_streams()
    counts: dict[str, int] = {}

    async def on_msg(subject: str, env: Envelope) -> None:
        errs = reg.validate(subject, env.to_dict())
        if errs:
            log.warning("schema violation on %s msg_id=%s: %s", subject, env.msg_id, errs[:3])
        archive.write(subject, env)
        counts[subject] = counts.get(subject, 0) + 1

    for s in reg.streams:
        for filt in s["subjects"]:
            await bus.subscribe(filt, f"logger-{s['name'].lower()}", on_msg)
    try:
        await bus.run()
    finally:
        archive.close()
        log.info("archived counts: %s", counts)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", default=None)
    ap.add_argument("--log-level", default="INFO")
    a = ap.parse_args()
    logging.basicConfig(level=a.log_level, format="%(asctime)s %(name)s %(levelname)s %(message)s")
    asyncio.run(run(Config.load(a.config)))


if __name__ == "__main__":
    main()
