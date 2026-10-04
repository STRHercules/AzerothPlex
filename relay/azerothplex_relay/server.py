from __future__ import annotations

import argparse
import asyncio
from collections import deque
import json
import logging
import ssl
import time
from typing import Any

from websockets.asyncio.server import ServerConnection, serve
from websockets.exceptions import ConnectionClosed

from . import PROTOCOL_VERSION
from .state import Delivery, MAX_LIVE_FRAME_BYTES, RelayState, is_live_audio_frame

LOGGER = logging.getLogger("azerothplex-relay")
MAX_MESSAGE_BYTES = 64 * 1024
RATE_WINDOW_SECONDS = 10.0
RATE_LIMIT_MESSAGES = 60
MAX_CONTROL_BACKLOG = 64
AUDIO_BACKLOG_FRAMES = 8


def frame_is_audio(data: bytes) -> bool:
    return is_live_audio_frame(data)


class RateLimiter:
    def __init__(self) -> None:
        self.timestamps: deque[float] = deque()

    def allow(self) -> bool:
        now = time.monotonic()
        while self.timestamps and now - self.timestamps[0] > RATE_WINDOW_SECONDS:
            self.timestamps.popleft()
        if len(self.timestamps) >= RATE_LIMIT_MESSAGES:
            return False
        self.timestamps.append(now)
        return True


class Outbound:
    """Per-connection writer.

    Control messages are queued in order. Live frames keep only the newest
    pending frame so a slow viewer skips ahead instead of accumulating latency.
    """

    def __init__(self, websocket: ServerConnection) -> None:
        self.websocket = websocket
        self.control: deque[str] = deque(maxlen=MAX_CONTROL_BACKLOG)
        self.frame: bytes | None = None
        self.audio: deque[bytes] = deque(maxlen=AUDIO_BACKLOG_FRAMES)
        self.changed = asyncio.Event()
        self.closed = False
        self.task = asyncio.create_task(self._run())

    def send_text(self, text: str) -> None:
        if self.closed:
            return
        self.control.append(text)
        self.changed.set()

    def send_frame(self, frame: bytes) -> None:
        if self.closed:
            return
        if frame_is_audio(frame):
            self.audio.append(frame)
        else:
            self.frame = frame
        self.changed.set()

    async def _run(self) -> None:
        try:
            while not self.closed:
                await self.changed.wait()
                self.changed.clear()
                while self.control:
                    await self.websocket.send(self.control.popleft())
                while self.audio:
                    await self.websocket.send(self.audio.popleft())
                frame = self.frame
                if frame is not None:
                    self.frame = None
                    await self.websocket.send(frame)
        except ConnectionClosed:
            pass
        except asyncio.CancelledError:
            pass

    async def kick(self, code: int, reason: str) -> None:
        self.closed = True
        self.task.cancel()
        try:
            await self.websocket.close(code=code, reason=reason)
        except ConnectionClosed:
            pass

    async def close(self) -> None:
        self.closed = True
        self.task.cancel()
        try:
            await self.task
        except asyncio.CancelledError:
            pass


class RelayServer:
    def __init__(self, *, public_rooms_enabled: bool = False) -> None:
        self.state = RelayState(public_rooms_enabled=public_rooms_enabled)
        self.connections: dict[str, Outbound] = {}

    async def deliver(self, deliveries: list[Delivery]) -> None:
        for delivery in deliveries:
            if delivery.binary is not None:
                for client_id in delivery.recipients:
                    outbound = self.connections.get(client_id)
                    if outbound is not None:
                        outbound.send_frame(delivery.binary)
                continue
            if delivery.message is None:
                continue
            text = json.dumps(delivery.message, separators=(",", ":"))
            if len(text.encode("utf-8")) > MAX_MESSAGE_BYTES:
                LOGGER.warning("dropping oversized outbound message type=%s", delivery.message.get("type"))
                continue
            for client_id in delivery.recipients:
                outbound = self.connections.get(client_id)
                if outbound is not None:
                    outbound.send_text(text)

    async def handler(self, websocket: ServerConnection) -> None:
        client_id = ""
        limiter = RateLimiter()
        outbound = Outbound(websocket)
        try:
            async for raw in websocket:
                if isinstance(raw, (bytes, bytearray)):
                    if len(raw) > MAX_LIVE_FRAME_BYTES:
                        await websocket.close(code=1009, reason="live frame too large")
                        return
                    if not client_id:
                        await websocket.close(code=1008, reason="missing client id")
                        return
                    await self.deliver(self.state.live_frame(client_id, bytes(raw)))
                    continue
                if len(raw.encode("utf-8")) > MAX_MESSAGE_BYTES:
                    await websocket.close(code=1009, reason="message too large")
                    return
                if not limiter.allow():
                    await websocket.close(code=1008, reason="rate limit")
                    return
                try:
                    message: dict[str, Any] = json.loads(raw)
                except json.JSONDecodeError:
                    await websocket.send(json.dumps({
                        "protocol": PROTOCOL_VERSION,
                        "type": "error",
                        "room": "",
                        "sender": "relay",
                        "memberId": "",
                        "hostMemberId": "",
                        "roomEpoch": 0,
                        "sequence": 0,
                        "sentAtMs": int(time.time() * 1000),
                        "payload": {"error": "Invalid JSON."},
                    }, separators=(",", ":")))
                    continue
                if not isinstance(message, dict):
                    continue
                sender = message.get("sender")
                if not isinstance(sender, str) or not sender or len(sender) > 128:
                    await websocket.close(code=1008, reason="missing client id")
                    return
                if client_id and sender != client_id:
                    await websocket.close(code=1008, reason="client id changed")
                    return
                if not client_id:
                    client_id = sender
                    old = self.connections.get(client_id)
                    self.connections[client_id] = outbound
                    if old is not None and old is not outbound:
                        await old.kick(1000, "reconnected")
                await self.deliver(self.state.handle(client_id, message))
        except ConnectionClosed:
            pass
        finally:
            if client_id and self.connections.get(client_id) is outbound:
                self.connections.pop(client_id, None)
                await self.deliver(self.state.leave(client_id, explicit=False))
            await outbound.close()

    async def cleanup_loop(self) -> None:
        while True:
            await asyncio.sleep(5)
            await self.deliver(self.state.cleanup())


async def run(host: str, port: int, ssl_context: ssl.SSLContext | None,
              public_rooms_enabled: bool = False) -> None:
    relay = RelayServer(public_rooms_enabled=public_rooms_enabled)
    cleanup = asyncio.create_task(relay.cleanup_loop())
    try:
        async with serve(
            relay.handler,
            host,
            port,
            ssl=ssl_context,
            max_size=MAX_LIVE_FRAME_BYTES,
            compression=None,
            ping_interval=20,
            ping_timeout=20,
        ):
            LOGGER.info("AzerothPlex relay listening on %s:%d%s", host, port,
                        " with TLS" if ssl_context else "")
            await asyncio.Future()
    finally:
        cleanup.cancel()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="AzerothPlex Watch Together relay")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=17777)
    parser.add_argument("--cert", help="TLS certificate path")
    parser.add_argument("--key", help="TLS private key path")
    parser.add_argument("--log-level", default="INFO")
    parser.add_argument(
        "--enable-public-rooms", action="store_true",
        help="Enable experimental public room creation/discovery (disabled by default)",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    logging.basicConfig(level=getattr(logging, args.log_level.upper(), logging.INFO),
                        format="%(asctime)s %(levelname)s %(message)s")
    context: ssl.SSLContext | None = None
    if args.cert or args.key:
        if not args.cert or not args.key:
            raise SystemExit("--cert and --key must be provided together")
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(args.cert, args.key)
    asyncio.run(run(args.host, args.port, context, args.enable_public_rooms))


if __name__ == "__main__":
    main()
