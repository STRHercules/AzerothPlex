import asyncio
import json

from azerothplex_relay.server import RelayServer, frame_is_audio
from azerothplex_relay.state import RelayState
from websockets.asyncio.client import connect
from websockets.asyncio.server import serve


def message(kind: str, sender: str, room: str = "", payload: dict | None = None) -> dict:
    return {
        "protocol": 1,
        "type": kind,
        "room": room,
        "sender": sender,
        "displayName": sender,
        "sentAtMs": 1000,
        "payload": payload or {},
    }


def first_message(deliveries):
    return deliveries[0].message


def test_only_host_may_start_live():
    state = RelayState()
    room_code = first_message(state.handle("host", message("room_create", "host")))["room"]
    state.handle("guest", message("room_join", "guest", room_code))
    result = state.handle("guest", message("live_start", "guest", room_code))
    assert first_message(result)["type"] == "error"


def test_live_start_broadcasts_and_room_payload_reports_active():
    state = RelayState()
    room_code = first_message(state.handle("host", message("room_create", "host")))["room"]
    state.handle("guest", message("room_join", "guest", room_code))
    result = state.handle("host", message("live_start", "host", room_code))
    outbound = first_message(result)
    assert outbound["type"] == "live_started"
    assert outbound["payload"]["live"]["active"] is True
    assert outbound["payload"]["live"]["broadcasterId"] == "host"
    assert set(result[0].recipients) == {"host", "guest"}

    snapshot = first_message(state.handle("guest", message("room_state", "guest", room_code)))
    assert snapshot["payload"]["live"]["active"] is True
    assert snapshot["payload"]["live"]["watcherCount"] == 0


def test_frames_route_only_to_watchers():
    state = RelayState()
    room_code = first_message(state.handle("host", message("room_create", "host")))["room"]
    state.handle("guest", message("room_join", "guest", room_code))
    state.handle("watcher", message("room_join", "watcher", room_code))
    state.handle("host", message("live_start", "host", room_code))

    missing = state.handle("watcher", message("live_watch", "watcher", room_code))
    assert first_message(missing)["payload"]["live"]["watcherCount"] == 1
    state.handle("guest", message("live_watch", "guest", room_code))

    routed = state.live_frame("host", b"frame-one")
    assert len(routed) == 1
    assert routed[0].binary == b"frame-one"
    assert set(routed[0].recipients) == {"guest", "watcher"}

    state.handle("watcher", message("live_unwatch", "watcher", room_code))
    routed = state.live_frame("host", b"frame-two")
    assert routed[0].recipients == ["guest"]

    stray = state.live_frame("guest", b"spoofed")
    assert first_message(stray)["type"] == "error"


def test_live_stops_when_broadcaster_leaves_or_disconnects():
    state = RelayState()
    room_code = first_message(state.handle("host", message("room_create", "host")))["room"]
    state.handle("guest", message("room_join", "guest", room_code))
    state.handle("host", message("live_start", "host", room_code))

    disconnected = state.leave("host", explicit=False)
    assert any(item.message and item.message["type"] == "live_stopped" for item in disconnected)
    assert "host" not in state.rooms[room_code].live_watchers

    state.handle("host", message("room_join", "host", room_code))
    state.handle("host", message("live_start", "host", room_code))
    explicit = state.leave("host", explicit=True)
    assert any(item.message and item.message["type"] == "live_stopped" for item in explicit)


def test_live_watch_requires_active_broadcast():
    state = RelayState()
    room_code = first_message(state.handle("host", message("room_create", "host")))["room"]
    state.handle("guest", message("room_join", "guest", room_code))
    result = state.handle("guest", message("live_watch", "guest", room_code))
    assert first_message(result)["type"] == "error"


def test_audio_frames_are_classified_by_header_flag():
    video = b"\x57\x58\x4c\x57\x01\x01\x00\x00\x00\x00\x00\x00"
    audio = b"\x57\x58\x4c\x57\x01\x02\x00\x00\x00\x00\x00\x00"
    assert not frame_is_audio(video)
    assert frame_is_audio(audio)
    assert not frame_is_audio(b"\x00\x01")


def test_server_forwards_binary_frames_between_clients():
    async def scenario() -> None:
        relay = RelayServer()
        async with serve(relay.handler, "127.0.0.1", 0, max_size=512 * 1024) as server:
            port = server.sockets[0].getsockname()[1]
            url = f"ws://127.0.0.1:{port}"
            async with connect(url) as host, connect(url) as guest:
                await host.send(json.dumps(message("room_create", "host")))
                created = json.loads(await host.recv())
                room_code = created["room"]
                assert created["type"] == "room_created"

                await guest.send(json.dumps(message("room_join", "guest", room_code)))
                assert json.loads(await guest.recv())["type"] == "room_joined"
                assert json.loads(await host.recv())["type"] == "member_joined"

                await host.send(json.dumps(message("live_start", "host", room_code)))
                assert json.loads(await host.recv())["type"] == "live_started"
                assert json.loads(await guest.recv())["type"] == "live_started"

                await guest.send(json.dumps(message("live_watch", "guest", room_code)))
                assert json.loads(await guest.recv())["type"] == "live_state"

                payload = b"\x00\x00\x00\x01live-bytes"
                await host.send(payload)
                assert await asyncio.wait_for(guest.recv(), timeout=2.0) == payload

                audio = b"\x57\x58\x4c\x57\x01\x02\x00\x00\x00\x00\x00\x00adts"
                await host.send(audio)
                assert await asyncio.wait_for(guest.recv(), timeout=2.0) == audio

                await host.send(json.dumps(message("live_stop", "host", room_code)))
                assert json.loads(await guest.recv())["type"] == "live_stopped"

    asyncio.run(scenario())
