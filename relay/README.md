# AzerothPlex Relay

The relay serves AzerothPlex Watch Together. Playback synchronization remains
state-only: the relay never receives Plex credentials or authenticated media
URLs.

Live Broadcast is an explicit opt-in exception: when a room host starts a live
broadcast, the relay blindly forwards the host's encoded video and audio frames
as binary WebSocket messages to room members who pressed **Watch Live**. Frames
are held in memory only, never written to disk. A slow viewer skips video
frames instead of buffering latency, while audio keeps a short bounded backlog
so it is not dropped per-frame. Viewers do not need a Plex account or Plex
access for the broadcast media.

## Development

```powershell
cd relay
py -m venv .venv
.venv\Scripts\pip install -e . pytest
.venv\Scripts\python -m azerothplex_relay.server --host 127.0.0.1 --port 17777
```

The native host defaults to `ws://127.0.0.1:17777/ws` for local development.

## Production TLS

Production clients should use `wss://`. Supply a certificate and private key to the relay:

```powershell
python -m azerothplex_relay.server --host 0.0.0.0 --port 17777 --cert fullchain.pem --key privkey.pem
```

The service supports private rooms, host migration, reconnect grace, authoritative playback sequencing, permissions, shared queues, ready checks, shared cinema settings, relay clock pings, message-size limits, room/member caps, and per-connection rate limiting.

Public room creation and discovery are implemented as an experimental feature but are **disabled by default**. The roadmap requires relay authentication and moderation/abuse controls before public discovery is considered production-ready. For local development only, opt in explicitly:

```powershell
python -m azerothplex_relay.server --host 127.0.0.1 --port 17777 --enable-public-rooms
```

The relay keeps room state only in memory. Abandoned rooms expire automatically.
