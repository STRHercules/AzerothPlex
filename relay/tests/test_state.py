from azerothplex_relay.state import RelayState


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


def test_create_join_and_host_migration():
    state = RelayState()
    created = state.handle("host", message("room_create", "host"))
    room_code = first_message(created)["room"]
    assert room_code.startswith("AZ-")

    joined = state.handle("guest", message("room_join", "guest", room_code))
    assert first_message(joined)["type"] == "room_joined"
    room = state.rooms[room_code]
    old_epoch = room.epoch

    deliveries = state.leave("host", explicit=True)
    assert room.host_id == "guest"
    assert room.epoch == old_epoch + 1
    assert any(item.message["type"] == "host_changed" for item in deliveries)


def test_host_only_rejects_guest_playback():
    state = RelayState()
    room_code = first_message(state.handle("host", message("room_create", "host")))["room"]
    state.handle("guest", message("room_join", "guest", room_code))
    result = state.handle("guest", message(
        "play", "guest", room_code,
        {"playback": {"playing": True, "paused": False, "stopped": False, "positionMs": 10}},
    ))
    assert first_message(result)["type"] == "error"


def test_shared_control_sequences_authoritatively():
    state = RelayState()
    created = state.handle("host", message(
        "room_create", "host", payload={"permissions": {"mode": 1}}
    ))
    room_code = first_message(created)["room"]
    state.handle("guest", message("room_join", "guest", room_code))
    room = state.rooms[room_code]
    before = room.sequence
    result = state.handle("guest", message(
        "seek", "guest", room_code,
        {"playback": {"playing": True, "paused": False, "stopped": False, "positionMs": 42000}},
    ))
    assert room.sequence == before + 1
    outbound = first_message(result)
    assert outbound["type"] == "seek"
    assert outbound["payload"]["playback"]["sequence"] == room.sequence
    assert outbound["payload"]["playback"]["roomEpoch"] == room.epoch
    assert set(result[0].recipients) == {"host", "guest"}


def test_public_room_listing_only_exposes_public_rooms():
    state = RelayState(public_rooms_enabled=True)
    private_code = first_message(state.handle("private", message("room_create", "private")))["room"]
    public_code = first_message(state.handle(
        "public", message("room_create", "public", payload={"visibility": "public"})
    ))["room"]
    response = first_message(state.handle("browser", message("public_rooms_request", "browser")))
    codes = {room["code"] for room in response["payload"]["publicRooms"]}
    assert public_code in codes
    assert private_code not in codes


def test_everyone_may_add_queue_does_not_grant_guest_reorder():
    state = RelayState()
    created = state.handle("host", message(
        "room_create", "host", payload={"permissions": {"queueMode": 1}}
    ))
    room_code = first_message(created)["room"]
    state.handle("guest", message("room_join", "guest", room_code))

    first_item = {"media": {"title": "Alien"}, "addedBy": "host", "order": 1}
    seeded = state.handle("host", message(
        "queue_update", "host", room_code, {"queue": [first_item]}
    ))
    assert first_message(seeded)["type"] == "queue_update"

    second_item = {"media": {"title": "The Thing"}, "addedBy": "spoofed", "order": 99}
    added = state.handle("guest", message(
        "queue_update", "guest", room_code, {"queue": [first_item, second_item]}
    ))
    outbound = first_message(added)
    assert outbound["type"] == "queue_update"
    assert outbound["payload"]["queue"][-1]["addedBy"] == "guest"
    assert outbound["payload"]["queue"][-1]["order"] == 2

    rejected = state.handle("guest", message(
        "queue_update", "guest", room_code, {"queue": [outbound["payload"]["queue"][-1]]}
    ))
    assert first_message(rejected)["type"] == "error"


def test_host_managed_queue_rejects_guest_add():
    state = RelayState()
    room_code = first_message(state.handle("host", message("room_create", "host")))["room"]
    state.handle("guest", message("room_join", "guest", room_code))
    result = state.handle("guest", message(
        "queue_update", "guest", room_code,
        {"queue": [{"media": {"title": "Alien"}, "addedBy": "guest", "order": 1}]},
    ))
    assert first_message(result)["type"] == "error"


def test_ready_check_waits_for_everyone_then_uses_shared_start_epoch():
    state = RelayState()
    room_code = first_message(state.handle("host", message("room_create", "host")))["room"]
    state.handle("guest", message("room_join", "guest", room_code))

    response = first_message(state.handle("host", message("ready_check", "host", room_code)))
    assert response["payload"]["readyCheckActive"] is True
    assert response["payload"]["readyStartAtMs"] == 0
    assert all(not member["ready"] for member in response["payload"]["members"])

    host_ready = first_message(state.handle(
        "host", message("ready_state", "host", room_code, {"ready": True})
    ))
    assert host_ready["payload"]["readyStartAtMs"] == 0

    guest_ready = first_message(state.handle(
        "guest", message("ready_state", "guest", room_code, {"ready": True})
    ))
    assert guest_ready["payload"]["readyCheckActive"] is True
    assert guest_ready["payload"]["readyStartAtMs"] > 0
    assert all(member["ready"] for member in guest_ready["payload"]["members"])

    started = first_message(state.handle(
        "host", message("play", "host", room_code, {"playback": {"playing": True}})
    ))
    assert started["payload"]["readyCheckActive"] is False
    assert started["payload"]["readyStartAtMs"] == 0


def test_reconnect_reuses_member():
    state = RelayState()
    room_code = first_message(state.handle("host", message("room_create", "host")))["room"]
    state.handle("guest", message("room_join", "guest", room_code))
    original_count = len(state.rooms[room_code].members)
    state.leave("guest", explicit=False)
    state.handle("guest", message("room_join", "guest", room_code))
    room = state.rooms[room_code]
    assert len(room.members) == original_count
    assert room.members["guest"].connected
