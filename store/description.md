# Azeroth Plex

Bring your Plex library into WarcraftXL. Azeroth Plex is a native Plex client
for the WarcraftXL 3.3.5a build-12340 client, with browsing and playback built
into the in-game Video Cinema panel.

## Features

- Sign in with Plex PIN authorization, discover a reachable server, and choose
  a library without entering your Plex password into the game.
- Browse Watchlist, libraries, search results, movies, shows, seasons, and
  episodes. Navigate through large lists with built-in paging.
- Play Plex media with direct playback when available and Plex transcoding as
  a fallback. Control playback, seeking, audio tracks, and subtitles from
  inside WarcraftXL.
- Keep playback running while the panel is hidden.
- Place the video on a client-local world-space screen. Move, resize, rotate,
  hide, show, and pin it to the game viewport, with optional depth occlusion
  and distance-based audio.
- Use native playback without requiring the Microsoft Edge WebView2 runtime.

## Privacy and requirements

Plex tokens are protected with Windows DPAPI, and account passwords are never
stored by Azeroth Plex or sent through WarcraftXL shared memory. Media stays
in Plex; Azeroth Plex does not download, cache, locally transcode, or
redistribute it.

Requires a WarcraftXL-enabled 32-bit build-12340 client, WarcraftXL ABI 1.1,
internet access, and a Plex account with access to a reachable server.
