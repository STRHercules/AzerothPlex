from __future__ import annotations

from dataclasses import dataclass, field
import secrets
import time
from typing import Any, Iterable

from . import PROTOCOL_VERSION

ALPHABET = "23456789ABCDEFGHJKMNPQRSTUVWXYZ"
MAX_MEMBERS = 32
MAX_QUEUE = 64
MAX_LIVE_FRAME_BYTES = 512 * 1024
LIVE_FRAME_FLAG_AUDIO = 0x02
ROOM_IDLE_SECONDS = 60 * 30
DISCONNECT_GRACE_SECONDS = 30


def is_live_audio_frame(data: bytes) -> bool:
    return len(data) > 5 and (data[5] & LIVE_FRAME_FLAG_AUDIO) != 0


def now_ms() -> int:
    return int(time.time() * 1000)


def clean_text(value: Any, maximum: int) -> str:
    if not isinstance(value, str):
        return ""
    return value[:maximum]


def room_code() -> str:
    return "AZ-" + "".join(secrets.choice(ALPHABET) for _ in range(4))


def permissions(payload: dict[str, Any] | None) -> dict[str, Any]:
    source = payload if isinstance(payload, dict) else {}
    mode = source.get("mode", 0)
    if mode not in (0, 1, 2):
        mode = 0
    queue_mode = source.get("queueMode", 0)
    if queue_mode not in (0, 1):
        queue_mode = 0
    return {
        "mode": mode,
        "syncAudio": bool(source.get("syncAudio", False)),
        "syncSubtitles": bool(source.get("syncSubtitles", False)),
        "queueMode": queue_mode,
    }


@dataclass
class Member:
    client_id: str
    display_name: str
    joined_order: int
    connected: bool = True
    ready: bool = False
    disconnected_at: float | None = None

    def wire(self, host_id: str) -> dict[str, Any]:
        return {
            "id": self.client_id,
            "displayName": self.display_name,
            "role": "host" if self.client_id == host_id else "guest",
            "connected": self.connected,
            "ready": self.ready,
            "joinedOrder": self.joined_order,
        }


@dataclass
class Room:
    code: str
    host_id: str
    visibility: str = "private"
    epoch: int = 1
    sequence: int = 0
    permissions: dict[str, Any] = field(default_factory=lambda: permissions(None))
    playback: dict[str, Any] = field(default_factory=dict)
    queue: list[dict[str, Any]] = field(default_factory=list)
    cinema: dict[str, Any] = field(default_factory=dict)
    ready_active: bool = False
    ready_start_at_ms: int = 0
    live_id: str = ""
    live_watchers: set[str] = field(default_factory=set)
    members: dict[str, Member] = field(default_factory=dict)
    updated_at: float = field(default_factory=time.monotonic)

    def next_sequence(self) -> int:
        self.sequence += 1
        self.updated_at = time.monotonic()
        return self.sequence

    def member_list(self) -> list[dict[str, Any]]:
        return [
            member.wire(self.host_id)
            for member in sorted(self.members.values(), key=lambda item: item.joined_order)
        ]

    def title(self) -> str:
        media = self.playback.get("media", {}) if isinstance(self.playback, dict) else {}
        title = clean_text(media.get("title"), 256) if isinstance(media, dict) else ""
        return title or "Watch Party"

    def host_name(self) -> str:
        member = self.members.get(self.host_id)
        return member.display_name if member else "Host"


@dataclass
class Delivery:
    recipients: list[str]
    message: dict[str, Any] | None = None
    binary: bytes | None = None


class RelayState:
    def __init__(self, *, public_rooms_enabled: bool = False) -> None:
        self.rooms: dict[str, Room] = {}
        self.client_room: dict[str, str] = {}
        self.join_counter = 0
        self.public_rooms_enabled = public_rooms_enabled

    def _envelope(
        self,
        message_type: str,
        *,
        room: Room | None = None,
        member_id: str = "",
        sender: str = "relay",
        payload: dict[str, Any] | None = None,
        sent_at_ms: int | None = None,
    ) -> dict[str, Any]:
        return {
            "protocol": PROTOCOL_VERSION,
            "type": message_type,
            "room": room.code if room else "",
            "sender": sender,
            "memberId": member_id,
            "hostMemberId": room.host_id if room else "",
            "roomEpoch": room.epoch if room else 0,
            "sequence": room.sequence if room else 0,
            "sentAtMs": now_ms() if sent_at_ms is None else sent_at_ms,
            "payload": payload or {},
        }

    @staticmethod
    def _live_payload(room: Room) -> dict[str, Any]:
        return {
            "active": bool(room.live_id),
            "broadcasterId": room.live_id,
            "watcherCount": len(room.live_watchers),
        }

    def _room_payload(self, room: Room) -> dict[str, Any]:
        return {
            "visibility": room.visibility,
            "members": room.member_list(),
            "permissions": room.permissions,
            "playback": room.playback,
            "queue": room.queue,
            "cinema": room.cinema,
            "readyCheckActive": room.ready_active,
            "readyStartAtMs": room.ready_start_at_ms,
            "live": self._live_payload(room),
        }

    def _error(self, client_id: str, text: str, room: Room | None = None) -> Delivery:
        return Delivery([client_id], self._envelope("error", room=room, payload={"error": text}))

    def _broadcast(self, room: Room, message: dict[str, Any], *, exclude: Iterable[str] = ()) -> Delivery:
        excluded = set(exclude)
        return Delivery(
            [member.client_id for member in room.members.values() if member.connected and member.client_id not in excluded],
            message,
        )

    def _find_room(self, client_id: str, code: str = "") -> Room | None:
        target = code or self.client_room.get(client_id, "")
        return self.rooms.get(target)

    @staticmethod
    def _update_ready_countdown(room: Room) -> None:
        if not room.ready_active:
            room.ready_start_at_ms = 0
            return
        connected = [member for member in room.members.values() if member.connected]
        all_ready = bool(connected) and all(member.ready for member in connected)
        if all_ready:
            if room.ready_start_at_ms <= 0:
                room.ready_start_at_ms = now_ms() + 3000
        else:
            room.ready_start_at_ms = 0

    def create_room(self, client_id: str, message: dict[str, Any]) -> list[Delivery]:
        self.leave(client_id, explicit=True)
        code = room_code()
        while code in self.rooms:
            code = room_code()
        payload = message.get("payload") if isinstance(message.get("payload"), dict) else {}
        if payload.get("visibility") == "public" and not self.public_rooms_enabled:
            return [self._error(client_id, "Public rooms are disabled on this relay.")]
        self.join_counter += 1
        member = Member(client_id, clean_text(message.get("displayName"), 64) or "Player", self.join_counter)
        room = Room(
            code=code,
            host_id=client_id,
            visibility="public" if payload.get("visibility") == "public" else "private",
            permissions=permissions(payload.get("permissions")),
        )
        room.members[client_id] = member
        room.next_sequence()
        self.rooms[code] = room
        self.client_room[client_id] = code
        response = self._envelope(
            "room_created", room=room, member_id=client_id, payload=self._room_payload(room)
        )
        return [Delivery([client_id], response)]

    def join_room(self, client_id: str, message: dict[str, Any]) -> list[Delivery]:
        code = clean_text(message.get("room"), 16).upper()
        room = self.rooms.get(code)
        if room is None:
            return [self._error(client_id, "Watch-party code is invalid or expired.")]

        current = self.client_room.get(client_id)
        if current and current != code:
            self.leave(client_id, explicit=True)
        if client_id not in room.members:
            if len(room.members) >= MAX_MEMBERS:
                return [self._error(client_id, "This watch party is full.", room)]
            self.join_counter += 1
            room.members[client_id] = Member(
                client_id,
                clean_text(message.get("displayName"), 64) or "Player",
                self.join_counter,
            )
        else:
            member = room.members[client_id]
            member.connected = True
            member.disconnected_at = None
            new_name = clean_text(message.get("displayName"), 64)
            if new_name:
                member.display_name = new_name
        self.client_room[client_id] = code
        self._update_ready_countdown(room)
        room.next_sequence()

        joined = self._envelope(
            "room_joined", room=room, member_id=client_id, payload=self._room_payload(room)
        )
        member_update = self._envelope(
            "member_joined", room=room, member_id=client_id,
            payload={"members": room.member_list()},
        )
        return [Delivery([client_id], joined), self._broadcast(room, member_update, exclude=[client_id])]

    def _stop_live(self, room: Room, broadcaster_id: str) -> list[Delivery]:
        if not broadcaster_id or room.live_id != broadcaster_id:
            return []
        room.live_id = ""
        room.live_watchers.clear()
        room.next_sequence()
        event = self._envelope(
            "live_stopped", room=room,
            payload={"live": self._live_payload(room), "members": room.member_list()},
        )
        return [self._broadcast(room, event)]

    def live_frame(self, client_id: str, data: bytes) -> list[Delivery]:
        code = self.client_room.get(client_id, "")
        room = self.rooms.get(code)
        if room is None or room.live_id != client_id:
            return [self._error(client_id, "Only the active broadcaster may send live frames.")]
        room.updated_at = time.monotonic()
        recipients = [
            member_id for member_id in room.live_watchers
            if member_id != client_id and member_id in room.members
            and room.members[member_id].connected
        ]
        if not recipients:
            return []
        return [Delivery(recipients, binary=data)]

    def leave(self, client_id: str, *, explicit: bool) -> list[Delivery]:
        code = self.client_room.pop(client_id, "")
        room = self.rooms.get(code)
        if room is None or client_id not in room.members:
            return []

        if not explicit:
            member = room.members[client_id]
            member.connected = False
            member.disconnected_at = time.monotonic()
            self._update_ready_countdown(room)
            room.next_sequence()
            update = self._envelope(
                "member_left", room=room, member_id=client_id,
                payload={"members": room.member_list()},
            )
            self.client_room[client_id] = code
            deliveries = self._stop_live(room, client_id)
            deliveries.append(self._broadcast(room, update, exclude=[client_id]))
            return deliveries

        del room.members[client_id]
        self._update_ready_countdown(room)
        room.next_sequence()
        deliveries: list[Delivery] = self._stop_live(room, client_id)
        if not room.members:
            del self.rooms[code]
            return deliveries
        if room.host_id == client_id:
            self._migrate_host(room)
            host_changed = self._envelope(
                "host_changed", room=room, payload={"members": room.member_list()}
            )
            deliveries.append(self._broadcast(room, host_changed))
        member_update = self._envelope(
            "member_left", room=room, member_id=client_id,
            payload={"members": room.member_list()},
        )
        deliveries.append(self._broadcast(room, member_update))
        return deliveries

    def _migrate_host(self, room: Room) -> None:
        eligible = sorted(room.members.values(), key=lambda item: item.joined_order)
        room.host_id = eligible[0].client_id
        room.epoch += 1
        room.sequence = 0
        room.next_sequence()

    def cleanup(self) -> list[Delivery]:
        current = time.monotonic()
        deliveries: list[Delivery] = []
        for code, room in list(self.rooms.items()):
            expired_members = [
                member.client_id for member in room.members.values()
                if not member.connected and member.disconnected_at is not None
                and current - member.disconnected_at >= DISCONNECT_GRACE_SECONDS
            ]
            for client_id in expired_members:
                self.client_room.pop(client_id, None)
                room.members.pop(client_id, None)
                self._update_ready_countdown(room)
                deliveries.extend(self._stop_live(room, client_id))
                if room.host_id == client_id and room.members:
                    self._migrate_host(room)
                    deliveries.append(self._broadcast(
                        room, self._envelope("host_changed", room=room,
                                             payload={"members": room.member_list()})
                    ))
            if not room.members or current - room.updated_at >= ROOM_IDLE_SECONDS:
                for member_id in room.members:
                    self.client_room.pop(member_id, None)
                del self.rooms[code]
        return deliveries

    def handle(self, client_id: str, message: dict[str, Any]) -> list[Delivery]:
        if message.get("protocol") != PROTOCOL_VERSION:
            return [self._error(client_id, "Party protocol version mismatch.")]
        message_type = clean_text(message.get("type"), 64)
        payload = message.get("payload") if isinstance(message.get("payload"), dict) else {}

        if message_type == "hello":
            return [Delivery([client_id], self._envelope("hello", member_id=client_id,
                                                         payload={"serverTimeMs": now_ms()}))]
        if message_type == "room_create":
            message = dict(message)
            message["payload"] = payload
            return self.create_room(client_id, message)
        if message_type == "room_join":
            return self.join_room(client_id, message)
        if message_type == "room_leave":
            return self.leave(client_id, explicit=True)
        if message_type == "public_rooms_request":
            if not self.public_rooms_enabled:
                return [self._error(client_id, "Public room discovery is disabled on this relay.")]
            rooms = [
                {
                    "code": room.code,
                    "title": room.title(),
                    "hostName": room.host_name(),
                    "memberCount": sum(1 for member in room.members.values() if member.connected),
                }
                for room in self.rooms.values()
                if room.visibility == "public"
            ]
            return [Delivery([client_id], self._envelope("public_rooms", payload={"publicRooms": rooms}))]
        if message_type == "ping":
            sent = message.get("sentAtMs") if isinstance(message.get("sentAtMs"), int) else 0
            return [Delivery([client_id], self._envelope(
                "pong", sent_at_ms=sent, payload={"serverTimeMs": now_ms()}
            ))]

        room = self._find_room(client_id, clean_text(message.get("room"), 16).upper())
        if room is None or client_id not in room.members:
            return [self._error(client_id, "Join a watch party before sending party state.")]
        room.members[client_id].connected = True
        room.members[client_id].disconnected_at = None
        is_host = room.host_id == client_id
        mode = room.permissions.get("mode", 0)
        can_control = is_host or mode == 1

        if message_type == "room_state":
            response = self._envelope(
                "room_state", room=room, member_id=client_id, payload=self._room_payload(room)
            )
            return [Delivery([client_id], response)]

        if message_type in {"playback_state", "media_changed", "play", "pause", "seek", "stop"}:
            if not can_control:
                return [self._error(client_id, "Only the watch-party host can control playback.", room)]
            playback = payload.get("playback") if isinstance(payload.get("playback"), dict) else {}
            playback = dict(playback)
            if message_type in {"play", "media_changed"}:
                room.ready_active = False
                room.ready_start_at_ms = 0
            room.next_sequence()
            playback["roomEpoch"] = room.epoch
            playback["sequence"] = room.sequence
            playback["authoritativeTimestampMs"] = now_ms()
            room.playback = playback
            event = self._envelope(
                message_type, room=room, member_id=client_id,
                payload={
                    "playback": room.playback,
                    "readyCheckActive": room.ready_active,
                    "readyStartAtMs": room.ready_start_at_ms,
                },
            )
            # Echo authoritative state to the sender as well. PartyClient consumes this to keep
            # the host's room epoch/sequence aligned with the relay without replaying its own action.
            return [self._broadcast(room, event)]

        if message_type == "permissions":
            if not is_host:
                return [self._error(client_id, "Only the watch-party host can change permissions.", room)]
            room.permissions = permissions(payload.get("permissions"))
            room.next_sequence()
            event = self._envelope(
                "permissions", room=room, payload={"permissions": room.permissions}
            )
            return [self._broadcast(room, event)]

        if message_type == "control_request":
            if mode != 2 or is_host:
                return [self._error(client_id, "Control requests are not enabled for this room.", room)]
            request = clean_text(payload.get("request"), 64)
            request_id = clean_text(payload.get("requestId"), 128)
            event = self._envelope(
                "control_request", room=room, member_id=client_id, sender=client_id,
                payload={"request": request, "requestId": request_id},
            )
            return [Delivery([room.host_id], event)]

        if message_type == "control_response":
            if not is_host:
                return [self._error(client_id, "Only the host can answer control requests.", room)]
            request_id = clean_text(payload.get("requestId"), 128)
            target = request_id.split("-", 2)[0:2]
            # Request IDs generated by clients begin with their stable ap-... ID. Find the longest member ID prefix.
            target_id = next((member_id for member_id in room.members if request_id.startswith(member_id + "-")), "")
            if not target_id:
                return [self._error(client_id, "The requesting member is no longer in this room.", room)]
            event = self._envelope(
                "control_response", room=room, member_id=target_id,
                payload={"requestId": request_id, "allowed": bool(payload.get("allowed", False))},
            )
            return [Delivery([target_id], event)]

        if message_type == "queue_update":
            queue = payload.get("queue") if isinstance(payload.get("queue"), list) else []
            queue = [item for item in queue[:MAX_QUEUE] if isinstance(item, dict)]
            if is_host:
                room.queue = queue
                for index, item in enumerate(room.queue):
                    item["order"] = index + 1
            else:
                if room.permissions.get("queueMode", 0) != 1:
                    return [self._error(client_id, "Only the host may add to this party queue.", room)]
                if len(queue) != len(room.queue) + 1 or queue[:-1] != room.queue:
                    return [self._error(client_id, "Guests may add one item but cannot remove or reorder the party queue.", room)]
                appended = dict(queue[-1])
                appended["addedBy"] = client_id
                appended["order"] = len(room.queue) + 1
                room.queue = [*room.queue, appended]
            room.next_sequence()
            event = self._envelope("queue_update", room=room, payload={"queue": room.queue})
            return [self._broadcast(room, event)]

        if message_type == "ready_check":
            if not is_host:
                return [self._error(client_id, "Only the host can start a ready check.", room)]
            room.ready_active = True
            room.ready_start_at_ms = 0
            for member in room.members.values():
                member.ready = False
            room.next_sequence()
            event = self._envelope(
                "ready_check", room=room,
                payload={
                    "readyCheckActive": True,
                    "readyStartAtMs": 0,
                    "members": room.member_list(),
                },
            )
            return [self._broadcast(room, event)]

        if message_type == "ready_state":
            if not room.ready_active:
                return [self._error(client_id, "There is no active ready check.", room)]
            room.members[client_id].ready = bool(payload.get("ready", False))
            self._update_ready_countdown(room)
            room.next_sequence()
            event = self._envelope(
                "ready_state", room=room, member_id=client_id,
                payload={
                    "ready": room.members[client_id].ready,
                    "readyCheckActive": room.ready_active,
                    "readyStartAtMs": room.ready_start_at_ms,
                    "members": room.member_list(),
                },
            )
            return [self._broadcast(room, event)]

        if message_type == "screen_preset":
            if not is_host:
                return [self._error(client_id, "Only the host can share cinema settings.", room)]
            cinema = payload.get("cinema") if isinstance(payload.get("cinema"), dict) else {}
            room.cinema = dict(cinema)
            room.next_sequence()
            event = self._envelope("screen_preset", room=room, payload={"cinema": room.cinema})
            return [self._broadcast(room, event)]

        if message_type == "live_start":
            if not is_host:
                return [self._error(client_id, "Only the watch-party host can start a live broadcast.", room)]
            if room.live_id:
                return [self._error(client_id, "A live broadcast is already active in this room.", room)]
            room.live_id = client_id
            room.live_watchers = set()
            room.next_sequence()
            event = self._envelope(
                "live_started", room=room, member_id=client_id,
                payload={"live": self._live_payload(room), "members": room.member_list()},
            )
            return [self._broadcast(room, event)]

        if message_type == "live_stop":
            if client_id != room.live_id and not is_host:
                return [self._error(client_id, "Only the broadcaster or host can stop the live broadcast.", room)]
            return self._stop_live(room, room.live_id)

        if message_type == "live_watch":
            if not room.live_id:
                return [self._error(client_id, "No live broadcast is active in this room.", room)]
            room.live_watchers.add(client_id)
            room.next_sequence()
            event = self._envelope(
                "live_state", room=room, member_id=client_id,
                payload={"live": self._live_payload(room)},
            )
            return [self._broadcast(room, event)]

        if message_type == "live_unwatch":
            room.live_watchers.discard(client_id)
            room.next_sequence()
            event = self._envelope(
                "live_state", room=room, member_id=client_id,
                payload={"live": self._live_payload(room)},
            )
            return [self._broadcast(room, event)]

        return [self._error(client_id, "Unsupported party message type.", room)]
