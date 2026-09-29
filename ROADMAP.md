# AzerothPlex Roadmap

> Codex implementation roadmap for evolving AzerothPlex from a native in-game Plex viewer into a synchronized social cinema system for WarcraftXL / WotLK 3.3.5a.

## 1. Purpose

AzerothPlex already provides:

- Native Plex sign-in and server discovery.
- Plex library browsing, watchlist access, search, metadata views, and paging.
- Movie, episode, playlist, and other media playback through libmpv.
- Playback controls including play, pause, stop, seek, audio stream selection, and subtitle selection.
- Plex timeline reporting.
- A client-local world-space video surface.
- Screen placement, rotation, sizing, height adjustment, depth occlusion, and distance-based audio.
- A viewport-pinned playback mode.
- A host/extension architecture connected through shared memory.
- DPAPI-protected Plex credentials.
- A Win32 C++20 host with DX11, ImGui, WinHTTP, and libmpv.

The roadmap should preserve those foundations and expand them without turning AzerothPlex into a video relay or media redistribution service.

The central architectural rule is:

> **Synchronize playback state, not video frames.**

Every participant continues to stream media from a Plex server available to that participant. Multiplayer features exchange only session metadata, presence, media identity, playback state, timestamps, screen configuration, and optional social events.

---

# 2. Goals

## Primary goals

1. Add reliable multiplayer watch-party synchronization.
2. Make watch parties feel native to World of Warcraft rather than like a generic desktop watch-party application.
3. Preserve the existing separation between:
   - WarcraftXL extension
   - AzerothPlex native host
   - Plex authentication/playback
4. Keep Plex credentials local to each client.
5. Avoid sending media frames, media files, or Plex access tokens through multiplayer infrastructure.
6. Build all new systems in incremental, testable phases.
7. Keep the project usable in single-player mode even if multiplayer services are unavailable.

## Secondary goals

- Improve the browsing and playback experience.
- Add quality-of-life media controls.
- Introduce reusable cinema layouts.
- Add social features after core playback synchronization is stable.
- Create a foundation for future server-hosted public cinema rooms.

---

# 3. Non-goals

The following are explicitly out of scope unless separately approved:

- Relaying or restreaming Plex video through AzerothPlex servers.
- Sharing Plex authentication tokens between users.
- Sharing a Plex password.
- Downloading media for offline playback.
- Implementing DRM circumvention.
- Replacing Plex transcoding.
- Making the world-space screen a real AzerothCore GameObject during the first multiplayer milestone.
- Adding server-side AzerothCore dependencies for the first watch-party implementation.
- Requiring all users to be connected to the same Plex account.
- Automatically exposing a user's Plex library contents to other players.
- Public room discovery before private room synchronization is stable.

---

# 4. Current architecture

The existing high-level flow is:

```text
WarcraftXL / WoW Client
        |
        | shared memory ABI
        v
AzerothPlex Host
  - NativeUi
  - PlexClient
  - MpvPlayer
  - FramePublisher
  - TokenStore
        |
        +--> Plex server
        |
        +--> libmpv
```

Relevant existing source areas:

```text
extension/src/
  Module.cpp
  VideoShared.hpp
  VideoSurface.cpp
  VideoSurface.hpp

host/
  VideoHost.cpp
  VideoShared.hpp
  NativeUi.cpp
  NativeUi.hpp
  PlexClient.cpp
  PlexClient.hpp
  PlexTypes.hpp
  MpvPlayer.cpp
  MpvPlayer.hpp
  FramePublisher.cpp
  FramePublisher.hpp
  TokenStore.cpp
  TokenStore.hpp
  tests/
```

The multiplayer design should primarily live in the host process.

The WarcraftXL extension should remain responsible for:

- rendering
- world-space screen placement
- input bridge
- in-game UI bridge
- local screen configuration

The host should remain responsible for:

- Plex
- libmpv
- media identity
- multiplayer networking
- party state
- synchronization logic
- reconnect logic
- queue logic
- presence
- watch-party permissions

---

# 5. Proposed architecture

Add two primary host components:

```text
PartyClient
SyncController
```

Recommended expanded architecture:

```text
WarcraftXL Extension
        |
        | shared memory
        v
AzerothPlex Host
  - NativeUi
  - PlexClient
  - MpvPlayer
  - FramePublisher
  - TokenStore
  - PartyClient          NEW
  - SyncController       NEW
  - SessionStore         NEW
        |
        | WebSocket / HTTPS
        v
AzerothPlex Relay
  - room membership
  - playback state
  - presence
  - queue
  - control events
  - ready state
```

The relay must never receive:

- Plex tokens
- Plex passwords
- raw media URLs containing authentication tokens
- decoded video frames
- media files

---

# 6. New host components

## 6.1 `PartyClient`

Suggested files:

```text
host/PartyClient.hpp
host/PartyClient.cpp
```

Responsibilities:

- establish relay connection
- create a room
- join a room
- leave a room
- reconnect after transient network failure
- send local session events
- receive remote session events
- expose connection state
- expose room membership
- maintain relay clock offset if required
- validate protocol versions
- ignore stale sequence numbers
- surface errors to `NativeUi`

Suggested states:

```cpp
enum class PartyConnectionState
{
    Disconnected,
    Connecting,
    Connected,
    Reconnecting,
    Failed
};
```

Suggested party roles:

```cpp
enum class PartyRole
{
    None,
    Host,
    Guest
};
```

---

## 6.2 `SyncController`

Suggested files:

```text
host/SyncController.hpp
host/SyncController.cpp
```

Responsibilities:

- own authoritative synchronization logic
- convert local playback events into party events
- apply remote playback events to `MpvPlayer`
- compute expected playback position
- perform soft drift correction
- perform hard seek correction
- prevent sync loops
- handle late joins
- handle reconnect catch-up
- handle host changes
- apply party permission rules
- preserve local subtitle/audio preferences unless explicitly configured otherwise

The controller should be testable independently from the network transport.

---

## 6.3 `SessionStore`

Suggested files:

```text
host/SessionStore.hpp
host/SessionStore.cpp
```

Responsibilities:

- save recent room identifiers if desired
- save user watch-party preferences
- save local party display name
- save permission defaults
- save sync thresholds
- save cinema presets
- never store raw Plex credentials

Use a format appropriate for the current project style.

---

# 7. Multiplayer protocol

Use a versioned protocol from the first implementation.

Example envelope:

```json
{
  "protocol": 1,
  "type": "playback_state",
  "room": "AZ7K3F",
  "sender": "client-id",
  "sequence": 492,
  "sentAtMs": 1790468400123,
  "payload": {}
}
```

## Required event types

Initial protocol should support:

```text
hello
room_create
room_created
room_join
room_joined
room_leave
member_joined
member_left
room_state
media_changed
playback_state
play
pause
seek
stop
host_changed
ping
pong
error
```

Later phases can add:

```text
queue_update
ready_check
ready_state
reaction
chat
control_request
control_response
screen_preset
screen_transform
presence
vote
```

---

# 8. Media identity

A watch party must not assume every user has the same `ratingKey`.

Define a normalized media identity.

Suggested structure:

```cpp
struct MediaIdentity
{
    std::string plexGuid;
    std::string imdbId;
    std::string tmdbId;
    std::string tvdbId;

    std::string type;

    std::string title;
    std::string grandparentTitle;

    int season = 0;
    int episode = 0;

    int64_t durationMs = 0;
};
```

## Resolution order

When joining a room:

1. Match exact Plex server + `ratingKey` if the participant uses the same Plex server.
2. Match Plex GUID.
3. Match external GUID:
   - TMDB
   - TVDB
   - IMDb
4. Match episode identity:
   - series GUID
   - season number
   - episode number
5. Fall back to:
   - title
   - year
   - duration tolerance
6. If no reliable match exists, do not auto-play.

Surface:

```text
This party is watching media that could not be matched in your Plex library.
```

Never silently guess between multiple ambiguous matches.

---

# 9. Playback synchronization model

## 9.1 Authority

Phase 1 synchronization should use a single authoritative host.

The host owns:

- play
- pause
- seek
- stop
- media changes
- queue advancement

Guests may optionally request control later.

## 9.2 Playback state

Recommended network state:

```cpp
struct PartyPlaybackState
{
    MediaIdentity media;

    bool playing;
    bool paused;

    int64_t positionMs;
    int64_t durationMs;

    int64_t authoritativeTimestampMs;
    uint64_t sequence;
};
```

## 9.3 Expected position

For a playing item:

```text
expected =
    reportedPosition +
    (localNow - authoritativeTimestamp)
```

For paused media:

```text
expected = reportedPosition
```

Do not continuously seek to every update.

---

# 10. Drift correction

Recommended initial thresholds:

```text
0 ms - 250 ms
    No correction.

250 ms - 1000 ms
    Soft correction.

> 1000 ms
    Hard seek.
```

## Soft correction

Use temporary mpv playback speed adjustment.

Example:

```text
client behind:
    1.02x

client ahead:
    0.98x
```

Return to:

```text
1.00x
```

once drift is inside tolerance.

Do not stack sync speed correction on top of a future user-selected playback speed without explicitly accounting for the base speed.

Recommended internal model:

```text
effectiveSpeed =
    userSpeed * syncCorrection
```

Clamp sync correction tightly.

## Hard correction

Hard seek when:

- drift exceeds threshold
- host explicitly seeks
- client rejoins
- media starts
- paused state changes and position is materially wrong

---

# 11. Milestone 0: Baseline and guardrails

## Objective

Document and protect existing behavior before multiplayer work begins.

## Tasks

- Add or update architecture comments for:
  - `NativeUi`
  - `MpvPlayer`
  - `PlexClient`
  - shared-memory UI bridge
- Add tests around:
  - playback state reporting
  - seek
  - play/pause transitions
  - media changes
  - timeline state
- Add an explicit protocol/version constant for future multiplayer networking.
- Confirm that Plex tokens remain isolated inside the host.
- Confirm that network logging cannot accidentally print Plex tokens or authenticated media URLs.

## Acceptance criteria

- Existing build passes.
- Existing tests pass.
- Single-player playback behavior is unchanged.
- No multiplayer code is active yet.

---

# 12. Milestone 1: Media UX polish

These features improve AzerothPlex without requiring multiplayer infrastructure.

## 12.1 Continue Watching

Add home sections:

```text
Continue Watching
Recently Added Movies
Recently Added TV
On Deck
Watchlist
Recently Watched
```

### Files likely involved

```text
host/PlexClient.cpp
host/PlexClient.hpp
host/PlexTypes.hpp
host/NativeUi.cpp
host/NativeUi.hpp
host/VideoShared.hpp
extension/src/VideoShared.hpp
extension/src/VideoSurface.cpp
```

### Acceptance criteria

- User can open each supported home section.
- Paging works.
- Existing library browsing still works.

---

## 12.2 Autoplay next episode

Behavior:

- detect the next episode
- display countdown
- allow:
  - Play Now
  - Cancel
- start the next episode when countdown expires

Acceptance criteria:

- does not autoplay after movies
- does not autoplay when no next episode exists
- respects Stop
- reports the new item correctly to Plex

---

## 12.3 Playback speed

Add:

```text
0.50x
0.75x
1.00x
1.25x
1.50x
2.00x
```

Acceptance criteria:

- speed survives UI close/reopen
- 1.00x remains default
- later sync code can apply a separate temporary drift modifier

---

## 12.4 Skip Intro / Skip Credits

If Plex marker metadata is available:

```text
Skip Intro
Skip Credits
```

Acceptance criteria:

- marker buttons appear only when a valid marker applies
- seek uses marker boundaries
- multiplayer integration is deferred until watch parties exist

---

# 13. Milestone 2: Local watch-party domain model

## Objective

Implement all party logic locally before connecting to a relay.

## Add

```text
host/PartyTypes.hpp
host/SyncController.hpp
host/SyncController.cpp
host/tests/SyncControllerTests.cpp
```

Suggested types:

```cpp
struct PartyMember;
struct PartyRoom;
struct PartyPlaybackState;
struct MediaIdentity;
struct PartyPermissions;
```

## Build a fake transport

Add:

```text
host/tests/FakePartyTransport.hpp
```

Simulate:

- delayed packets
- reordered packets
- duplicated packets
- reconnect
- host pause
- host seek
- host media change
- late join

## Acceptance criteria

Automated tests prove:

- stale sequences are ignored
- repeated events are idempotent
- explicit seek is applied once
- soft drift correction activates inside its intended threshold
- hard seek occurs only above threshold or on explicit seek
- reconnect catches the guest up
- a guest action cannot override host state in host-only mode

---

# 14. Milestone 3: Relay MVP

## Objective

Create the smallest possible external relay.

Recommended implementation:

```text
relay/
  README.md
  src/
  tests/
```

Technology may be selected independently, but requirements are more important than language.

## Relay requirements

The relay must:

- use TLS in production
- support WebSocket connections
- generate short join codes
- maintain room membership
- maintain authoritative room state
- forward host playback events
- support reconnect
- expire abandoned rooms
- rate-limit abusive clients
- enforce maximum message size
- validate protocol versions

The relay must not:

- accept Plex tokens
- accept raw video
- accept media files
- accept arbitrary large payloads
- become required for normal solo playback

## Recommended room ID

Human-readable code:

```text
AZ-7K3F
```

Avoid ambiguous characters if possible:

```text
0/O
1/I/L
```

## Acceptance criteria

- two test clients can create/join a room
- reconnect restores room state
- room is destroyed after configured inactivity
- malformed clients cannot crash the relay
- protocol version mismatch yields a clear error

---

# 15. Milestone 4: `PartyClient`

## Objective

Connect the native host to the relay.

Add:

```text
host/PartyClient.hpp
host/PartyClient.cpp
host/tests/PartyClientTests.cpp
```

Prefer using existing Windows networking dependencies where reasonable.

## Required operations

```cpp
CreateRoom()
JoinRoom(code)
LeaveRoom()

SendPlaybackState()
SendMediaChange()

RequestRoomState()

PollEvents()
```

## UI states

```text
Offline
Connecting
Connected
Reconnecting
Failed
```

## Acceptance criteria

- solo playback works with relay offline
- creating a room yields a code
- joining a valid code works
- joining invalid/expired code returns a clear error
- transient disconnect enters reconnect state
- rejoin restores current playback state

---

# 16. Milestone 5: Watch Together MVP

## Objective

Ship the first useful synchronized watch party.

## Required features

- Create Watch Party
- Join Watch Party
- Leave Watch Party
- display party code
- display members
- host-only controls
- synchronize:
  - media
  - play
  - pause
  - seek
  - stop
  - playback position
- late join
- reconnect catch-up

## UI

Add a Watch Together section to the existing in-game AzerothPlex panel.

Suggested layout:

```text
WATCH TOGETHER

[ Create Watch Party ]

Join Code
[ AZ-7K3F ] [ Join ]
```

Active room:

```text
WATCH PARTY

Code: AZ-7K3F

Host:
Nat

Members:
Nat
Zach
Playerthree

Now Watching:
The Thing

Sync:
Connected
Drift: 42 ms

[ Copy Code ]
[ Leave Party ]
```

## Acceptance criteria

Two real clients must be able to:

1. join the same room
2. resolve the same item
3. start within reasonable tolerance
4. stay synchronized for at least 30 minutes
5. survive:
   - pause
   - resume
   - repeated seeks
   - one client disconnect/reconnect
6. avoid oscillating seeks

Target normal steady-state drift:

```text
<= 250 ms
```

Temporary drift during recovery may be larger.

---

# 17. Milestone 6: Party permissions

Add modes:

```text
Host Only
Shared Control
Request Control
```

## Host Only

Only host actions affect session state.

## Shared Control

All members may control playback.

Relay or host must serialize state by sequence number.

## Request Control

Guests submit:

```text
Pause Request
Seek Request
Skip Request
```

Host receives:

```text
Zach wants to pause playback.

[ Allow ]
[ Ignore ]
```

## Acceptance criteria

- permission mode is visible
- guests cannot bypass host-only mode
- stale guest requests are discarded after state changes
- permissions survive reconnect

---

# 18. Milestone 7: Audio and subtitle policy

Default behavior:

```text
Playback position      synchronized
Play/Pause             synchronized
Media                  synchronized

Audio track            local
Subtitle track         local
Volume                 local
```

Optional party settings:

```text
Sync audio selection
Sync subtitle selection
```

Do not make these defaults.

Reason:

- users may need different languages
- accessibility preferences differ
- users may have different stream IDs for the same media

When synchronized, match streams semantically rather than by raw Plex stream ID.

Match using:

```text
language
codec
title
forced flag
hearing impaired flag
```

---

# 19. Milestone 8: Party queue

Add shared queue:

```text
Tonight's Queue

1. Alien
2. The Thing
3. Predator
```

Track:

```cpp
struct PartyQueueItem
{
    MediaIdentity media;
    std::string addedBy;
    uint64_t order;
};
```

Modes:

```text
Host managed
Everyone may add
```

Later:

```text
Vote-based ordering
Shuffle
```

## Acceptance criteria

- queue synchronizes for all clients
- unresolved media is visibly marked per client
- host can remove/reorder items
- queue advances into autoplay

---

# 20. Milestone 9: Ready checks

Add:

```text
Start Ready Check
```

Display:

```text
Nat      Ready
Zach     Ready
Jaina    Not Ready
Arthas   AFK
```

When all required members are ready:

```text
3
2
1
Play
```

The countdown must use synchronized time.

Do not use local timers independently without a shared start epoch.

---

# 21. Milestone 10: Group-oriented invitations

Potential integration paths:

```text
Invite Party
Invite Raid
Invite Guild
Invite Nearby Players
```

Implementation depends on what WarcraftXL exposes.

If direct WoW social APIs are unavailable, begin with:

- Copy Code
- pasteable chat invite message
- slash commands

Suggested commands:

```text
/plex
/plex play
/plex pause
/plex stop
/plex seek 1:35:00

/plex party create
/plex party join AZ-7K3F
/plex party leave
```

Do not block core watch-party delivery on deep WoW social integration.

---

# 22. Milestone 11: Mini player

Add an optional compact overlay:

```text
The Thing

[ -10 ] [ Play/Pause ] [ +10 ]

53:19 / 1:49:00
```

Requirements:

- movable
- optional
- does not interrupt world-space playback
- works during watch parties
- respects host-only control mode

---

# 23. Milestone 12: Cinema presets

Add user-defined screen presets.

Suggested data:

```cpp
struct CinemaPreset
{
    std::string name;

    bool pinned;
    float width;
    float heightOffset;
    float rotation;

    float pinnedX;
    float pinnedY;
    float pinnedWidth;

    bool depthOcclusion;
    float depthOffset;

    bool distanceAudio;
    float audioRange;
};
```

Built-in presets:

```text
Cozy
Movie Theater
Raid Night
Outdoor Screen
Picture in Picture
Desktop
Tavern TV
```

Support:

```text
Save Current Layout
Rename
Delete
Apply
```

---

# 24. Milestone 13: Shared cinema layout

## Phase A

Synchronize only logical screen settings:

```text
screen size
screen height
screen rotation
distance audio
depth settings
preset
```

Each client still places the screen relative to their own character.

## Phase B

If WarcraftXL exposes stable world transform APIs, support:

```text
map ID
instance ID
world X
world Y
world Z
orientation
screen width
```

Then participants can perceive the screen in the same world location.

Do not implement Phase B by guessing player/world coordinates.

---

# 25. Milestone 14: Cinema mode

Add an optional viewing mode that can request hiding or reducing WoW UI elements when supported.

Potential behaviors:

```text
Hide action bars
Hide minimap
Hide quest tracker
Dim UI
Keep chat optional
Unlock cursor
Restore previous UI state on exit
```

Requirements:

- always reversible
- never permanently modifies interface state
- pressing Escape should provide an obvious exit path

---

# 26. Milestone 18: Auto-pause on disconnect

Configurable host policy:

```text
Keep playing
Pause for 60 seconds
Pause until reconnect
Ask host
```

Avoid allowing a single unstable client to permanently grief a room.

Recommended safeguards:

- only apply to selected members
- timeout
- host override
- cooldown after repeated reconnects

---

# 27. Milestone 19: Public cinema rooms

This is a later-stage feature.

Possible room types:

```text
Private code room
Friends room
Guild room
Public server room
Scheduled screening
```

Public room browsing should not ship until:

- private rooms are stable
- relay authentication exists
- abuse controls exist
- moderation controls exist
- version compatibility is reliable

---

# 33. Milestone 22: Aspect ratio support

Current rendering should be generalized beyond fixed 16:9.

Support:

```text
Original
16:9
16:10
4:3
21:9
```

Fit modes:

```text
Fit
Fill
Stretch
Crop
```

Preferred design:

- framebuffer size and display aspect ratio should be separate concepts
- preserve media aspect ratio by default
- do not distort subtitles

---

# 34. Milestone 23: Decorative screen frames

Optional cosmetic geometry:

```text
None
Classic Television
Goblin Television
Gnomish Projector
Orcish Cinema
Blood Elf Arcane Screen
Titan Hologram
Tavern Projection Screen
Outdoor Cinema
```

Requirements:

- video surface remains independent
- cosmetic frame must not affect sync
- frame selection may optionally be included in shared cinema presets

---

# 35. Milestone 24: Ambient lighting

Experimental.

Sample frame color data and use it for local visual ambience if the renderer supports it safely.

Rules:

- disabled by default
- no gameplay effect
- local only
- low update frequency
- no expensive per-frame CPU readback if avoidable

Do not block higher-priority features on this.

---

# 36. Host migration

Initial implementation may end the room when the host leaves.

Later implementation should support deterministic host migration.

Suggested rules:

1. current host leaves
2. relay selects longest-connected eligible member
3. new host receives room state
4. sequence epoch increments
5. clients discard old-host control packets

The migration must not reuse old sequence numbers.

Possible model:

```text
roomEpoch
sequence
```

Comparison:

```text
newer roomEpoch always wins
same roomEpoch -> higher sequence wins
```

---

# 37. Reconnection

Every client should maintain:

```text
room code
client ID
member ID
last accepted epoch
last accepted sequence
```

On reconnect:

1. reconnect transport
2. rejoin room
3. request authoritative room state
4. resolve current media if needed
5. compare playback position
6. hard-seek if necessary
7. resume drift correction

Never assume local state is authoritative after reconnect.

---

# 38. Clock synchronization

Avoid relying purely on the local system clocks of different players.

Options:

## Preferred

Relay provides timing reference through ping/pong.

Track approximate:

```text
round-trip time
clock offset
```

Then interpret server-timestamped room events.

## Minimum viable fallback

For explicit playback state events:

- measure arrival time
- estimate one-way delay as half RTT
- use conservative correction thresholds

Keep timing implementation encapsulated inside synchronization code.

---

# 39. Security requirements

## Client

- Plex tokens stay local.
- Do not include Plex tokens in logs.
- Do not send authenticated stream URLs to relay.
- Validate all network payload sizes.
- Validate enum values.
- Validate string lengths.
- Reject incompatible protocol versions.
- Use TLS for production relay connections.
- Do not execute commands received from peers.

## Relay

- rate-limit clients
- cap room size
- cap message size
- expire idle rooms
- prevent room-code brute-force abuse
- do not persist unnecessary media activity
- sanitize user-provided display names
- log protocol errors without logging private credentials

---

# 40. Privacy

Default relay metadata should be minimal.

Recommended transient data:

```text
room ID
client ID
display name
media identity
playback state
queue
ready state
connection state
```

Avoid sending:

```text
Plex token
Plex password
local filesystem paths
raw media URL
full Plex library
Windows username
machine hostname
IP address to other clients
```

---

# 41. Shared-memory ABI strategy

The current extension/host bridge uses a fixed shared-memory ABI.

Do not casually append fields without versioning.

For any ABI change:

1. increment version constant
2. update both:
   - `host/VideoShared.hpp`
   - `extension/src/VideoShared.hpp`
3. preserve static assertions
4. update UI bridge tests
5. fail safely on mismatch
6. document migration

Prefer exposing only UI state through shared memory.

Do not push full multiplayer networking into the extension unless WarcraftXL integration requires it.

---

# 42. UI additions

Recommended new views:

```cpp
enum class UiView
{
    Login,
    Servers,
    Home,
    Library,
    Search,
    Details,
    Player,
    Party,
    Queue,
    Settings
};
```

Potential commands:

```cpp
enum class UiCommand
{
    // existing commands...

    CreateParty,
    JoinParty,
    LeaveParty,
    CopyPartyCode,

    StartReadyCheck,
    SetReadyState,

    AddQueueItem,
    RemoveQueueItem,
    MoveQueueItem,

    RequestPause,
    RequestSeek,

    SetPartyPermissionMode,
    ApplyCinemaPreset
};
```

Keep the command ABI bounded.

If one-command-per-frame becomes unreliable for bursty party interactions, replace the current latest-command slot with a small bounded command queue.

---

# 43. Slash commands

If WarcraftXL exposes a safe command registration mechanism, add:

```text
/plex
/plex open
/plex play
/plex pause
/plex stop
/plex seek <time>

/plex party create
/plex party join <code>
/plex party leave

/plex screen place
/plex screen show
/plex screen hide
```

Command handlers should call the same domain operations as UI buttons.

Do not duplicate playback logic inside command parsing.

---

# 44. Testing strategy

## Unit tests

Add or expand tests for:

```text
MediaIdentity matching
Party protocol parsing
Sequence ordering
Room epoch handling
Drift calculation
Soft correction
Hard correction
Late join
Reconnect
Permission enforcement
Queue ordering
Ready checks
Host migration
```

## Integration tests

Create two local simulated clients.

Scenarios:

```text
create room
join room
play
pause
resume
seek forward
seek backward
change media
guest reconnect
host reconnect
packet delay
packet duplication
packet reordering
```

## Manual two-client test matrix

Every watch-party release should be manually tested using two real WotLK clients.

Minimum:

```text
same machine if supported
two machines on LAN
two machines over internet
same Plex server
different Plex accounts with shared library access
host transcode + guest direct play
host direct play + guest transcode
subtitles different
audio tracks different
```

---

# 45. Logging and diagnostics

Add a dedicated optional diagnostic view.

Display:

```text
Party connection
Relay latency
Room epoch
Last sequence
Playback drift
Correction mode
Media match status
Reconnect count
```

Example:

```text
Sync: Connected
RTT: 62 ms
Drift: -84 ms
Correction: None
Room Epoch: 4
Sequence: 1852
Media: Exact GUID Match
```

Logging levels:

```text
Error
Info
Debug
Trace
```

Never log tokens or authenticated URLs.

---

# 46. Feature flags

Use compile-time or runtime feature flags during development.

Recommended:

```text
watchTogether
sharedQueue
readyChecks
reactions
partyChat
sharedCinema
publicRooms
ambientLighting
multipleScreens
```

Experimental features should not destabilize core playback.

---

# 47. Build and CI expectations

Every implementation milestone should end with:

```powershell
cmake -S host -B host/build -A Win32 -DMPV_ROOT='...'
cmake --build host/build --config Release
ctest --test-dir host/build -C Release --output-on-failure
```

Codex should not mark a milestone complete unless:

- build succeeds
- tests pass
- new tests cover the feature
- manual validation requirements are documented
- no existing single-player regression is knowingly introduced

---

# 48. Codex implementation rules

When Codex works from this roadmap:

1. Read repository instructions before editing.
2. Inspect relevant existing code before designing replacements.
3. Prefer extending current abstractions over duplicating them.
4. Keep changes small and reviewable.
5. Implement one milestone or tightly-related milestone group at a time.
6. Add tests in the same change as the feature.
7. Preserve 32-bit WotLK compatibility.
8. Preserve Win32 host compatibility.
9. Preserve existing libmpv behavior.
10. Preserve Plex token isolation.
11. Do not introduce a new networking framework unless justified.
12. Do not silently change the shared-memory ABI.
13. Do not mix relay transport logic directly into playback rendering code.
14. Do not put party synchronization logic directly in `NativeUi`.
15. Keep `SyncController` transport-independent.
16. Keep `PartyClient` playback-independent.
17. Keep UI commands thin.
18. Do not claim real multiplayer validation unless two clients were actually tested.
19. Document any WarcraftXL API limitation encountered.
20. If implementation requires a WarcraftXL core change, stop at a clearly documented boundary instead of inventing an unavailable API.

---

# 49. Recommended implementation order

## Phase A: Playback polish

```text
M0 Baseline and guardrails
M1 Continue Watching
M1 Autoplay
M1 Playback speed
M1 Skip Intro/Credits
```

## Phase B: Watch Together core

```text
M2 Local synchronization domain
M3 Relay MVP
M4 PartyClient
M5 Watch Together MVP
M6 Permissions
M7 Local audio/subtitle preferences
```

## Phase C: Social watch parties

```text
M8 Shared queue
M9 Ready checks
M10 Group-oriented invitations
M11 Mini player
```

## Phase D: Azeroth Cinema

```text
M12 Cinema presets
M13 Shared cinema layout
M14 Cinema mode
M18 Disconnect policy
```

## Phase E: Server-scale cinema

```text
M19 Public cinema rooms
M22 Aspect ratios
M23 Decorative frames
M24 Ambient lighting
```

---

## Implementation checkpoint — 2026-09-27

The canonical repo now contains implementation paths for the retained milestones in this roadmap: M0–M14, M18, and M22–M24. M19 public room creation/discovery is implemented behind explicit client/relay feature gates and remains disabled by default until the production authentication, abuse-control, and moderation prerequisites above are satisfied.

The removed M15–M17 and M20–M21 features are intentionally not part of the implementation order.

Three items remain validation/API boundaries rather than missing feature code:

- the Watch Together v1 definition below still requires a real two-client WoW test before it may be called multiplayer-validated;
- WarcraftXL does not currently expose a safe action-bar/minimap hide/restore API, so Cinema Mode uses the documented reversible compact-control fallback instead of mutating WoW interface state;
- WarcraftXL ABI 1.1 does not expose a safe slash-command or WoW social-command registration API, so M10 ships the copyable party invite/code path and does not invent `/plex` command hooks.

The retained M8 queue implementation now has its own **Host managed / Everyone may add** permission independent of playback control. In Everyone-may-add mode guests may append items, while host-only removal and reordering remain enforced by both the host UI and relay.

Automated coverage includes Plex fixtures, player/frame helpers, UI bridge/state tests, party identity/synchronization tests including duplicate and reordered packets, persisted session settings, queue permission enforcement, and relay room-state tests.

---

# 50. Definition of Watch Together v1

Watch Together v1 is complete when all of the following are true:

- user can create a room
- another user can join using a short code
- both users resolve the same Plex media
- host can play
- host can pause
- host can seek
- host can stop
- host can change media
- guest follows those actions
- guest playback remains within target drift during normal playback
- guest can disconnect and reconnect
- late join works
- host-only control works
- each user can keep local volume, audio, and subtitle settings
- relay never receives Plex credentials or video frames
- solo playback works without the relay
- automated tests cover core synchronization
- a real two-client test has been completed

---

# 51. Definition of Azeroth Cinema v1

Azeroth Cinema v1 is complete when:

- Watch Together v1 is stable
- cinema presets exist
- room members can optionally share a screen preset
- ready checks exist
- shared queue exists
- reconnect works
- mini player exists
- users can enter a distraction-reduced cinema mode
- room diagnostics are available
- all social features are optional

World-coordinate shared screen placement is not required for Azeroth Cinema v1 unless WarcraftXL already exposes stable world transform APIs.

---

# 52. Long-term vision

The intended end state is:

```text
A player enters WoW.

They open AzerothPlex.

They create a cinema room.

Their party joins.

Everyone resolves the same movie from their own Plex access.

The host places a giant screen.

The room performs a ready check.

3
2
1

Playback starts.

Everyone stays synchronized.

Subtitles and audio remain personal.

Players can react, queue the next movie, chat, and continue watching without leaving Azeroth.
```

The core differentiator is not merely "Plex inside WoW."

It is:

> **A synchronized social cinema system that happens to live inside WotLK.**
