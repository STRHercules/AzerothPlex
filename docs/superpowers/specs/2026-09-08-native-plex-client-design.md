# Native Plex Client for WarcraftXL

## Status

Draft for user review.

## Summary

Replace the current WebView2/YouTube player with a native Plex client hosted by
`wxl-video-host.exe`. The client will use Dear ImGui for its desktop interface,
libmpv for media playback, WinHTTP for Plex API access, and the existing
WarcraftXL extension for the movable world-space display.

The world-space screen remains client-local. It will continue to be movable,
resizable, rotatable, depth-aware, and persisted per client; only its frame
source changes from WebView2 capture to native playback output.

## Goals

- Authenticate any Plex account using Plex PIN/OAuth without collecting or
  storing the account password.
- Discover the account's reachable Plex servers and allow server selection.
- Browse Continue Watching, Recently Added, libraries, movies, shows, seasons,
  episodes, collections, playlists, and search results.
- Play and resume media with direct play when supported and Plex transcoding as
  fallback.
- Support seeking, volume, audio tracks, subtitles, fullscreen helper-window
  playback, and Plex timeline/scrobble updates.
- Keep helper-window playback working while the helper is hidden.
- Render the current video onto the WarcraftXL world-space screen.
- Preserve current placement controls: move in front of the player, width/size,
  16:9 aspect ratio, height, rotation, visibility, depth testing, depth offset,
  spatial audio, and `world-screen.tsv` persistence.

## Non-goals

- Running or modifying a Plex server.
- Locally downloading, caching, transcoding, or redistributing media. Plex
  server-side transcoding remains supported as the playback fallback.
- Rebuilding the full Plex web application.
- Making the world-space screen server-synchronized or visible to other
  players.
- Adding arbitrary aspect-ratio distortion to the world-space screen.
- Optimizing GPU inter-process texture sharing before profiling the simpler raw
  frame transport.

## Chosen approach

The native helper is preferred over retaining WebView2 because the existing
transport captures WebView2 as PNG every 67 ms, decodes and scales it with WIC,
then copies it to shared memory before WarcraftXL uploads it into a D3D9
texture. Native playback removes the browser compositor and PNG/WIC pipeline.

Dear ImGui with a Direct3D 11 backend provides the helper UI. libmpv is loaded
dynamically as the media engine and uses its software render API to produce the
fixed 640x360 video frame consumed by both the helper UI and WarcraftXL. Plex
API calls use WinHTTP. The existing D3D9 world-screen renderer remains the
consumer of video frames.

## Components

### Native helper

`wxl-video-host.exe` remains the process entry point and owns:

- the Win32 helper window;
- Dear ImGui rendering and input;
- Plex authentication, server discovery, metadata, and timeline requests;
- libmpv lifecycle, playback properties, and event handling;
- native frame conversion and the shared-memory producer.

The UI is divided by responsibility rather than by a new framework layer:

- authentication/server selection;
- home/library/search/detail views;
- playback controls and track selection;
- status/error presentation.

### Plex API and authentication

The helper creates a stable Plex client identifier and starts the Plex PIN
login flow. It opens the Plex authorization page in the user's default browser,
polls the PIN endpoint, and stores the returned token protected with Windows
DPAPI under the existing WarcraftXL local application-data area. Logout deletes
the protected token.

With a valid token, the helper discovers account resources, tests available
connections, and selects a reachable server URI. API requests include the
required Plex product, platform, version, device, and client identifier
headers. Managed Plex Home profiles are supported when exposed by the account.

Credentials and tokens are never written to logs, shared memory, or the
WarcraftXL extension.

### Playback

The selected Plex metadata determines the direct-play URL and available media
tracks. libmpv opens the direct stream first when compatible. If direct play is
not viable, the helper starts the Plex universal transcoder and opens its HLS
output.

Playback events update the native UI and periodically report position/state to
Plex. Stop, pause, seek, and completion use the corresponding Plex timeline
state. Expired sessions and failed transcodes are surfaced as actionable UI
errors.

### Frame bridge

The current PNG/WIC capture path is removed. The helper asks libmpv's software
render API for a fixed 640x360 BGRA frame, uploads that frame to the native
Dear ImGui texture, and publishes the same raw pixels through a versioned
double buffer or ring buffer. The first implementation targets a 30 FPS
world-screen update rate. The extension copies the active frame into its
existing D3D9 texture and draws it with the current world projection/depth
logic.

The helper UI may render at its native window size; the video texture and
shared world-screen output remain independent fixed-size 640x360 targets.
Direct GPU inter-process texture sharing and a libmpv OpenGL interop backend
are deferred until profiling proves the raw software-render path is the
remaining bottleneck.

### WarcraftXL extension

`VideoSurface` remains responsible for:

- opening and controlling the helper;
- reading the shared frame transport;
- D3D9 texture lifetime and device-reset recovery;
- world-space projection, depth testing, outline, and drawing;
- placement, movement, rotation, sizing, visibility, and persistence;
- distance-based audio volume and in-game quick controls.

The world-space screen remains 16:9. Its width changes the rendered size while
height is derived from the aspect ratio, matching the current YouTube player.
The native client does not own world placement.

## Data flow

1. The user signs in through Plex OAuth in the system browser.
2. The helper discovers servers and loads Plex library metadata.
3. The user selects media in the native Dear ImGui interface.
4. Plex metadata selects direct play or starts a transcoder session.
5. libmpv decodes and renders the media in the native helper.
6. The helper publishes raw RGBA frames to shared memory.
7. WarcraftXL uploads the latest frame to its D3D9 texture and draws the
   movable/resizable world-space screen.
8. Playback state is reported to Plex and displayed in both the helper and the
   in-game status panel.

## Error handling

- OAuth cancellation, timeout, and expired tokens return to the login view.
- Server discovery shows only reachable connections and explains when none are
  available.
- A direct-play failure retries through Plex transcoding once before reporting
  failure.
- Missing media, subtitle, or audio tracks are shown without terminating the
  player.
- libmpv end/error events update playback state and stop timeline updates.
- WarcraftXL exit, map changes, and D3D device resets release or reconnect the
  frame bridge without invalid texture use.
- Network and server errors remain in the helper UI; sensitive headers and
  tokens are excluded from diagnostics.

## Build and packaging

- Update the host CMake target for dynamically loaded libmpv, Dear ImGui, WinHTTP, D3D11, and the
  required Windows libraries.
- Pin or otherwise identify redistributable libmpv and UI sources in the build
  and update `THIRD_PARTY_NOTICES.md`.
- Remove WebView2 SDK/runtime requirements and `player.html` from the release
  manifest once the native client is functional.
- Bump the extension version and shared transport ABI version together.
- Keep the existing WarcraftXL ABI, 32-bit build target, extension ID, and
  helper executable name stable unless a build constraint requires otherwise.

## Verification

Automated/build checks:

- 32-bit Release build of the helper and extension.
- Shared transport ABI size/version checks on both sides.
- Plex API fixture checks for login polling, resource selection, metadata
  parsing, direct-play URL construction, transcode fallback, and timeline
  requests.
- libmpv event/state checks using a local test stream or controlled fixture.
- Existing source and package checks updated for the removed WebView2 assets.

Manual acceptance:

- Sign in, sign out, switch profile, and select a server.
- Browse libraries, search, open details, resume media, and play an episode or
  movie.
- Verify direct play and a server-transcoded stream.
- Change audio/subtitle tracks, seek, pause, resume, and confirm Plex progress.
- Hide the helper while audio/video continues.
- Place the world screen, move it, resize it, rotate it, change depth/visibility,
  leave and re-enter a world, and confirm `world-screen.tsv` persistence.
- Confirm the world screen remains smooth enough at the target frame rate and
  survives device reset and helper shutdown.

## Deliberate limits

The first native transport uses libmpv's CPU-visible software render buffer
and a raw shared frame buffer rather than cross-API GPU handles. This removes
the known PNG/WIC latency with a small, portable ABI. GPU texture sharing or
libmpv OpenGL interop is an optimization follow-up only if profiling shows the
software conversion or D3D9 upload is still material.
