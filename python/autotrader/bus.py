"""JetStream bus for the Python sidecars.

Semantics match the C++ NatsBus: publish with Nats-Msg-Id = msg_id (server-side
dedupe inside the stream's duplicate window), durable pull consumers with
explicit ack, consumer-side dedupe on msg_id, nak on handler exception.
"""
from __future__ import annotations

import asyncio
import collections
import logging
from typing import Awaitable, Callable

import nats
from nats.js import JetStreamContext
from nats.js.api import AckPolicy, ConsumerConfig, DeliverPolicy, RetentionPolicy, StorageType, StreamConfig
from nats.js.errors import NotFoundError

from .envelope import Envelope
from .schemas import SchemaRegistry

log = logging.getLogger("autotrader.bus")

Handler = Callable[[str, Envelope], Awaitable[None]]


class Dedupe:
    def __init__(self, capacity: int = 100000):
        self.capacity = capacity
        self._seen: set[str] = set()
        self._order: collections.deque[str] = collections.deque()

    def first_time(self, msg_id: str) -> bool:
        if msg_id in self._seen:
            return False
        self._seen.add(msg_id)
        self._order.append(msg_id)
        while len(self._order) > self.capacity:
            self._seen.discard(self._order.popleft())
        return True


class Bus:
    def __init__(self, url: str, client_name: str, registry: SchemaRegistry | None = None, validate_outgoing: bool = True):
        self.url = url
        self.client_name = client_name
        self.registry = registry
        self.validate_outgoing = validate_outgoing
        self.nc: nats.NATS | None = None
        self.js: JetStreamContext | None = None
        self._subs: list[tuple[JetStreamContext.PullSubscription, str, Handler]] = []
        self._dedupe = Dedupe()
        self._closed = False

    async def connect(self) -> None:
        self.nc = await nats.connect(self.url, name=self.client_name, max_reconnect_attempts=-1, reconnect_time_wait=1)
        self.js = self.nc.jetstream(timeout=5)
        log.info("connected to %s as %s", self.url, self.client_name)

    async def close(self) -> None:
        self._closed = True
        if self.nc:
            try:
                await asyncio.wait_for(self.nc.drain(), timeout=3)
            except Exception:  # noqa: BLE001 - drain can time out on pull consumers; close hard
                await self.nc.close()
            self.nc = None

    async def ensure_streams(self) -> None:
        assert self.js and self.registry
        for s in self.registry.streams:
            cfg = StreamConfig(
                name=s["name"],
                subjects=list(s["subjects"]),
                max_age=s["max_age_days"] * 86400,
                max_msgs=s["max_msgs"],
                duplicate_window=s.get("duplicate_window_s", 120),
                storage=StorageType.FILE,
                retention=RetentionPolicy.LIMITS,
            )
            try:
                await self.js.stream_info(s["name"])
                await self.js.update_stream(cfg)
                log.debug("updated stream %s", s["name"])
            except NotFoundError:
                await self.js.add_stream(cfg)
                log.info("created stream %s subjects=%s max_age=%dd max_msgs=%d", s["name"], s["subjects"], s["max_age_days"], s["max_msgs"])

    async def publish(self, subject: str, env: Envelope) -> None:
        assert self.js
        d = env.to_dict()
        if self.validate_outgoing and self.registry:
            self.registry.validate_or_raise(subject, d)
        ack = await self.js.publish(subject, env.dumps().encode(), headers={"Nats-Msg-Id": env.msg_id})
        if ack.duplicate:
            log.debug("duplicate publish deduped by JetStream msg_id=%s", env.msg_id)

    async def subscribe(self, subject_filter: str, durable: str, handler: Handler) -> None:
        assert self.js and self.registry
        stream = self.registry.stream_for_subject(subject_filter.replace(">", "x").replace("*", "x"))
        cfg = ConsumerConfig(durable_name=durable, ack_policy=AckPolicy.EXPLICIT, deliver_policy=DeliverPolicy.ALL, ack_wait=30, max_deliver=10, max_ack_pending=1024, filter_subject=subject_filter)
        sub = await self.js.pull_subscribe(subject_filter, durable=durable, stream=stream, config=cfg)
        self._subs.append((sub, subject_filter, handler))
        log.info("subscribed %s durable=%s stream=%s", subject_filter, durable, stream)

    async def poll_once(self, batch: int = 64, timeout: float = 0.2) -> int:
        handled = 0
        for sub, filt, handler in self._subs:
            try:
                msgs = await sub.fetch(batch, timeout=timeout)
            except (TimeoutError, asyncio.TimeoutError):   # nats.errors.TimeoutError derives from the builtin
                continue
            except Exception as e:  # noqa: BLE001
                log.warning("fetch %s failed: %r", filt, e)
                continue
            for m in msgs:
                handled += 1
                try:
                    env = Envelope.loads(m.data)
                    if self._dedupe.first_time(env.msg_id):
                        await handler(m.subject, env)
                    else:
                        log.debug("duplicate msg_id %s on %s skipped", env.msg_id, m.subject)
                    await m.ack()
                except Exception as e:  # noqa: BLE001
                    log.exception("handler for %s failed: %s", m.subject, e)
                    await m.nak(delay=2)
        return handled

    async def run(self, idle_sleep: float = 0.05, on_idle: Callable[[], Awaitable[None]] | None = None) -> None:
        while not self._closed:
            n = await self.poll_once()
            if on_idle is not None:
                await on_idle()
            if n == 0:
                await asyncio.sleep(idle_sleep)
