# AzerothPlex

A native Plex client for the WarcraftXL 3.3.5a build-12340 client. It uses a
small Win32/Dear ImGui helper with libmpv playback and mirrors the current
video onto a client-local, movable world-space screen.

## Install

1. Use a WarcraftXL-enabled 32-bit build-12340 client.
2. Install the release containing `wxl-video-screen.dll`,
   `wxl-video-host.exe`, `libmpv-2.dll`, `wxl.json`, and this documentation.
3. Restart the client when WarcraftXL requests it.
4. Press **Insert**, open **Video Cinema**, and choose **Open Plex player**.

No Microsoft Edge WebView2 runtime is required.

## First login and playback

1. Choose **Open Plex sign-in** in the native helper.
2. Complete Plex PIN authorization in the system browser.
3. Select a reachable Plex server and library.
4. Choose a movie, show, season, or episode and press **Play**.
5. Use the native helper for browsing, search, playback, seeking, tracks, and
   subtitles. Hiding the helper leaves playback and audio running.

The Plex token is protected with Windows DPAPI under
`%LOCALAPPDATA%\WarcraftXL\plex-player`. The account password is never entered
into AzerothPlex and tokens are not sent through WarcraftXL shared memory.

## World-space screen

The screen is client-local and is not a server GameObject or multiplayer
object. In the in-game **Video Cinema** panel:

- choose **Place / move screen in front of me**;
- change **Screen width** to resize the 16:9 screen;
- rotate, raise, lower, hide, or show the screen;
- enable character/world depth occlusion and adjust depth offset;
- enable distance-based audio and tune its range.

Placement is saved to
`Extensions/wxl-video-screen/world-screen.tsv` and restored per client.

## Requirements and limits

- WarcraftXL ABI 1.1 and the 32-bit build-12340 client;
- internet access for Plex authorization and server metadata;
- a Plex account with access to at least one reachable server;
- server-side Plex transcoding may be used when direct play is unavailable.

Media is played from Plex. AzerothPlex does not download, cache, locally
transcode, or redistribute media. The world-space screen is client-local.

For Plex `*.plex.direct` connections, login and metadata remain HTTPS, but the
native media stream uses the server's standard HTTP port to avoid a known
32-bit libmpv/OpenSSL crash while opening HTTPS media. This fallback is limited
to Plex direct-host names.

## Build

The helper is a Win32 C++20 target. The pinned x86 libmpv development package
must be supplied through `MPV_ROOT`; the build dynamically loads
`libmpv-2.dll` and does not require an MSVC import library.

```powershell
cmake -S host -B host/build -A Win32 `
  -DMPV_ROOT='C:/path/to/mpv-dev-i686-20260608-git-6444c05'
cmake --build host/build --config Release
ctest --test-dir host/build -C Release --output-on-failure
```

The current verified x86 runtime is mpv `v0.41.0-734-g6444c0505`, built from
the `20260608` Windows package. Newer i686 packages must be tested before
replacement because one newer package reproduced an upstream 32-bit OpenSSL
assertion during libmpv initialization.
