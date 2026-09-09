# Native Plex Client Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the WebView2/YouTube helper with a native Dear ImGui/libmpv Plex client while preserving the movable, resizable WarcraftXL world-space screen.

**Architecture:** Keep `wxl-video-host.exe` as the native helper process. WinHTTP and DPAPI handle Plex authentication/API access, Dear ImGui with Direct3D 11 renders the helper UI, libmpv decodes playback, and a versioned raw-RGBA shared-memory bridge feeds the existing D3D9 WarcraftXL surface. WarcraftXL remains the owner of world placement, size, rotation, depth, persistence, and spatial audio.

**Tech Stack:** C++20, Win32, Direct3D 11 in the helper, Direct3D 9 in WarcraftXL, Dear ImGui, dynamically loaded libmpv, WinHTTP, Windows DPAPI, MSXML 6 for Plex XML responses, nlohmann/json for Plex OAuth JSON, CMake, CTest.

**Spec:** `docs/superpowers/specs/2026-09-08-native-plex-client-design.md`

## Global Constraints

- Build the helper for the existing 32-bit Win32 target and keep the helper executable name `wxl-video-host.exe` stable.
- Remove WebView2 and `player.html`; no browser runtime is required by the finished package.
- Use Plex PIN/OAuth; never collect or log the Plex password, token, or authenticated request URL.
- Support Plex server-side transcoding as fallback; do not locally download, cache, transcode, or redistribute media.
- Keep the WarcraftXL extension client-local and server-independent.
- Keep the world screen 16:9; `width_` controls size and height remains `width_ * 9 / 16`.
- Keep the initial libmpv software-render/shared output at 640x360 raw BGRA with a 30 FPS target; defer cross-API GPU handles and OpenGL interop until profiling justifies them.
- Preserve placement fields, `world-screen.tsv`, spatial audio, device-reset recovery, and parent-process shutdown behavior.
- Use the smallest dependency surface that supplies native UI and playback; do not add a second UI framework or a custom media decoder.

## File Map

Create these focused host files:

- `host/PlexTypes.hpp` — Plex account, server, library, media, stream, playback, and event value types.
- `host/TokenStore.hpp` / `host/TokenStore.cpp` — DPAPI-protected token persistence and logout deletion.
- `host/PlexClient.hpp` / `host/PlexClient.cpp` — WinHTTP requests, OAuth PIN polling, resource discovery, metadata, stream selection, and timeline updates.
- `host/FramePublisher.hpp` / `host/FramePublisher.cpp` — producer-side shared-memory ABI and raw frame publication.
- `host/MpvPlayer.hpp` / `host/MpvPlayer.cpp` — libmpv lifecycle, D3D11 render target, playback properties, events, and staging readback.
- `host/NativeUi.hpp` / `host/NativeUi.cpp` — Dear ImGui screens and user actions; no Plex transport or media decoding logic.
- `host/tests/FramePublisherTests.cpp` — deterministic shared-frame sequence and torn-read checks.
- `host/tests/PlexFixtureTests.cpp` — deterministic Plex response parsing, URL construction, and timeline request checks.
- `host/tests/MpvProbe.cpp` — 32-bit libmpv load/initialize smoke test.

Modify these existing files:

- `host/CMakeLists.txt` — replace WebView2/WIC build inputs with native dependencies and tests.
- `host/VideoHost.cpp` — reduce to the Win32 composition root, message loop, D3D11 setup, control processing, and component wiring.
- `host/VideoShared.hpp` — update the host-side ABI to version 2 and remove the YouTube URL payload.
- `extension/src/VideoShared.hpp` — mirror the exact host-side ABI version 2.
- `extension/src/VideoSurface.hpp` — replace URL-entry methods/state with native-helper commands while retaining placement and rendering methods.
- `extension/src/VideoSurface.cpp` — consume the v2 raw frame bridge, update labels/commands, and preserve placement behavior.
- `extension/src/Module.cpp` — update the module description/comment only if needed for the new Plex name.
- `README.md` — native Plex installation, login, playback, and world-screen operation.
- `THIRD_PARTY_NOTICES.md` — Dear ImGui, libmpv, and JSON/XML dependency notices.
- `wxl.json` — remove `player.html`, update listing text and version.
- `player.html` — delete after the native helper is functional and packaging no longer references it.

---

### Task 1: Establish the 32-bit native dependency and test build

**Files:**
- Modify: `host/CMakeLists.txt`
- Create: `host/tests/MpvProbe.cpp`
- Add or vendor: `host/third_party/imgui/` at one pinned Dear ImGui revision, including `imgui.cpp`, core headers, `imgui_draw.cpp`, `imgui_tables.cpp`, `imgui_widgets.cpp`, `backends/imgui_impl_win32.cpp`, and `backends/imgui_impl_dx11.cpp`
- Add or vendor: `host/third_party/json.hpp` from one pinned nlohmann/json release

**Interfaces:**
- Consumes: an x86 libmpv SDK directory containing `include/mpv/client.h`, an import library, and `libmpv-2.dll`.
- Produces: a reproducible `wxl-video-host` CMake target with `MPV_ROOT`, `IMGUI_ROOT`, and `NLOHMANN_JSON_ROOT` cache inputs and an `mpv_probe` test executable. libmpv is loaded at runtime from `libmpv-2.dll`; no MSVC import library is required.

- [ ] **Step 1: Add the failing x86 libmpv probe.**

  Implement `host/tests/MpvProbe.cpp` with this behavior:

  ```cpp
  #include <mpv/client.h>
  #include <windows.h>

  int main()
  {
      HMODULE module = LoadLibraryW(L"libmpv-2.dll");
      if (!module) return 1;
      auto create = reinterpret_cast<mpv_handle* (*)()>(GetProcAddress(module, "mpv_create"));
      auto initialize = reinterpret_cast<int (*)(mpv_handle*)>(GetProcAddress(module, "mpv_initialize"));
      auto destroy = reinterpret_cast<void (*)(mpv_handle*)>(GetProcAddress(module, "mpv_destroy"));
      if (!create || !initialize || !destroy) return 2;
      mpv_handle* handle = create();
      if (!handle) return 1;
      const int initialized = initialize(handle);
      destroy(handle);
      FreeLibrary(module);
      return initialized >= 0 ? 0 : 3;
  }
  ```

- [ ] **Step 2: Add and run the probe target before wiring the application.**

  Add `include(CTest)`, an `mpv_probe` executable linked only to Windows libraries, and an `add_test(NAME mpv_probe COMMAND mpv_probe)` entry without changing the existing helper target yet.

  Run:

  ```powershell
  rtk cmake -S host -B host/build -A Win32 -DMPV_ROOT='C:/deps/libmpv-x86'
  rtk cmake --build host/build --config Release --target mpv_probe
  rtk ctest --test-dir host/build -C Release -R mpv_probe --output-on-failure
  ```

  Expected: the probe loads the x86 `libmpv-2.dll`, starts libmpv, and exits with code 0. Do not substitute a 64-bit DLL into this 32-bit target.

- [ ] **Step 3: Add the native dependency targets alongside the old host inputs.**

  Add the native cache variables below and keep the existing WebView2 include/library variables temporarily so the current host remains buildable until Task 6 removes its source references:

  ```cmake
  set(MPV_ROOT "" CACHE PATH "x86 libmpv SDK root")
  set(IMGUI_ROOT "${CMAKE_CURRENT_LIST_DIR}/third_party/imgui" CACHE PATH "Dear ImGui root")
  set(NLOHMANN_JSON_ROOT "${CMAKE_CURRENT_LIST_DIR}/third_party" CACHE PATH "nlohmann/json include root")
  ```

  Add the ImGui core/backend sources, include the libmpv and JSON roots, and link `d3d11`, `dxgi`, `d3dcompiler`, `dwmapi`, `winhttp`, `crypt32`, `ole32`, `oleaut32`, `user32`, `gdi32`, `shell32`, and `shlwapi`. Copy `libmpv-2.dll` from `${MPV_ROOT}` beside the helper after the build.

- [ ] **Step 4: Build the existing host target against the new dependency surface.**

  Run:

  ```powershell
  rtk cmake --build host/build --config Release --target wxl-video-host
  ```

  Expected: the target compiles with the new libraries even before the old WebView2 implementation is removed. Keep the old source temporarily only to establish dependency availability; Task 6 removes its includes and link requirements.

- [ ] **Step 5: Commit the dependency/build checkpoint.**

  ```powershell
  rtk git add host/CMakeLists.txt host/tests/MpvProbe.cpp host/third_party
  rtk git commit -m "build: add native plex dependencies"
  ```

### Task 2: Replace PNG capture with a versioned raw-frame bridge

**Files:**
- Modify: `host/VideoShared.hpp`
- Modify: `extension/src/VideoShared.hpp`
- Create: `host/FramePublisher.hpp`
- Create: `host/FramePublisher.cpp`
- Create: `host/tests/FramePublisherTests.cpp`
- Modify: `host/CMakeLists.txt`

**Interfaces:**
- Consumes: decoded BGRA frames from `MpvPlayer::CopyFrameTo`.
- Produces: `FramePublisher::Open()`, `FramePublisher::Publish(const uint8_t*, uint32_t)`, `FramePublisher::Close()`, and an ABI-v2 mapping accepted by `VideoSurface::ConnectFrames()`.

- [ ] **Step 1: Write the ABI and sequence tests first.**

  Add tests that publish two known 4-byte-per-pixel patterns, assert the active sequence advances monotonically, assert the active slot contains the complete pattern, and assert a publisher close clears `ready` and `hostPid`.

  The test-facing helper must be:

  ```cpp
  bool PublishFrameForTest(FrameHeader& header, std::vector<std::byte>& mapping,
                           std::span<const uint8_t> bgra, uint32_t sourceStride);
  ```

  The first assertion sequence should be:

  ```cpp
  std::vector<std::byte> mapping(kMappingBytes);
  std::vector<uint8_t> first(kFrameBytes, 0x11);
  std::vector<uint8_t> second(kFrameBytes, 0x22);
  assert(PublishFrameForTest(header, mapping, first, kStride));
  assert(PublishFrameForTest(header, mapping, second, kStride));
  assert(header.sequence == 2);
  assert(header.ready == 1);
  ```

- [ ] **Step 2: Run the transport test to verify it fails.**

  Run:

  ```powershell
  rtk cmake --build host/build --config Release --target frame_publisher_tests
  rtk ctest --test-dir host/build -C Release -R frame_publisher_tests --output-on-failure
  ```

  Expected: FAIL because the v2 publisher and test helper do not exist yet.

- [ ] **Step 3: Define the v2 shared ABI in both headers.**

  Keep the existing mapping names so an extension update can reconnect, but set `kVersion = 2`. Preserve 640x360, 4-byte pixels, two slots, `activeIndex`, `sequence`, `slotSequence`, `hostPid`, and `ready`. Remove `kUrlBytes` and the URL array from `ControlBlock`. Keep commands for `Show`, `Play`, `Pause`, `Stop`, and `Hide`, plus the existing volume sequence/percentage fields.

  The host and extension copies must remain byte-for-byte equivalent except for include order.

- [ ] **Step 4: Implement the producer with the existing torn-read protocol.**

  `FramePublisher::Publish` must:

  1. Clamp/reject a source stride smaller than `kStride`.
  2. Select the inactive slot.
  3. Store a negative sequence before copying.
  4. Copy each row into the fixed-size slot.
  5. Issue `MemoryBarrier()`.
  6. Store the positive slot sequence, active index, and global sequence.
  7. Set `ready` only after the first complete frame.

  Do not encode, decode, resize, or allocate per frame.

- [ ] **Step 5: Run the transport test to verify it passes.**

  ```powershell
  rtk cmake --build host/build --config Release --target frame_publisher_tests
  rtk ctest --test-dir host/build -C Release -R frame_publisher_tests --output-on-failure
  ```

  Expected: PASS with complete-frame and shutdown assertions.

- [ ] **Step 6: Commit the ABI checkpoint.**

  ```powershell
  rtk git add host/VideoShared.hpp extension/src/VideoShared.hpp host/FramePublisher.* host/tests/FramePublisherTests.cpp host/CMakeLists.txt
  rtk git commit -m "perf: replace png frame transport"
  ```

### Task 3: Implement DPAPI token storage and Plex API client

**Files:**
- Create: `host/PlexTypes.hpp`
- Create: `host/TokenStore.hpp`
- Create: `host/TokenStore.cpp`
- Create: `host/PlexClient.hpp`
- Create: `host/PlexClient.cpp`
- Create: `host/tests/PlexFixtureTests.cpp`
- Modify: `host/CMakeLists.txt`

**Interfaces:**
- Consumes: local application-data path, stable client identifier, and Plex HTTP responses.
- Produces: `PlexClient::StartLogin()`, `PlexClient::PollLogin()`, `PlexClient::LoadResources()`, `PlexClient::LoadHome()`, `PlexClient::LoadChildren()`, `PlexClient::Search()`, `PlexClient::BuildPlayback()`, `PlexClient::ReportTimeline()`, `PlexClient::SignOut()`, and `PlexClient::DrainEvents()`.

  Define these core values in `PlexTypes.hpp`:

  ```cpp
  struct PlexServer { std::string name; std::string uri; std::string accessToken; };
  struct PlexSection { std::string key; std::string title; std::string type; };
  struct PlexItem { std::string ratingKey; std::string title; std::string type; std::string grandparentTitle; std::string thumb; int viewOffsetMs = 0; bool viewed = false; };
  struct PlexPlayback { std::string uri; std::string sessionId; std::vector<int> audioStreams; std::vector<int> subtitleStreams; };
  ```

  `PlexEvent` is a `std::variant` of login state, server list, section list, item list, playback result, timeline result, and error values. The UI consumes events only through `DrainEvents()` on the window thread.

- [ ] **Step 1: Write deterministic fixture tests.**

  Add fixture strings for one OAuth PIN response, one resource list, one library section list, one movie metadata response, one episode response, and one unavailable direct-play response. Test:

  - OAuth `id`, `code`, and `authToken` extraction.
  - selecting the first reachable HTTPS connection and falling back to HTTP only when HTTPS is absent.
  - extracting library, movie, show, season, episode, audio, and subtitle fields.
  - building a tokenized media URL without logging it.
  - producing a transcode request when direct-play capability is false.
  - producing `playing`, `paused`, `stopped`, and `watched` timeline states.

  Use framework-free assertions such as:

  ```cpp
  const auto movie = ParseMetadata(movieXml);
  assert(movie.title == "Test Movie");
  assert(BuildDirectPlayback(movie, server).uri == "https://plex.test/library/parts/7/file");
  assert(BuildTimelineRequest(movie, TimelineState::Paused, 42000).find("state=paused") != std::string::npos);
  ```

- [ ] **Step 2: Run the fixture tests to verify they fail.**

  ```powershell
  rtk cmake --build host/build --config Release --target plex_fixture_tests
  rtk ctest --test-dir host/build -C Release -R plex_fixture_tests --output-on-failure
  ```

  Expected: FAIL because the Plex types, parsers, and client do not exist yet.

- [ ] **Step 3: Implement `TokenStore` with DPAPI.**

  Store one binary protected blob beneath `%LOCALAPPDATA%\WarcraftXL\plex-player\token.dat`. Use `CryptProtectData` with the current user scope, write through a `.tmp` file followed by `MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`, and delete the token on logout. Return an empty token for missing/corrupt data without logging decrypted contents.

- [ ] **Step 4: Implement WinHTTP request and response handling.**

  Centralize `WinHttpOpen`, `WinHttpConnect`, request headers, response-body collection, status-code checks, timeout values, and HTTPS certificate errors in `PlexClient.cpp`. Redact query parameters and `X-Plex-Token` from errors. Keep network work off the UI thread using one `std::jthread` job queue and a thread-safe event queue drained by the UI.

- [ ] **Step 5: Implement OAuth PIN login and resource discovery.**

  `StartLogin()` creates a PIN, builds the Plex authorization URL, and opens it with `ShellExecuteW`. `PollLogin()` polls until authorized, persists the DPAPI token, then requests `/api/resources?includeHttps=1`. Probe each advertised connection with a lightweight authenticated request and emit only reachable servers.

- [ ] **Step 6: Implement metadata, search, playback selection, and timeline requests.**

  Parse Plex XML with MSXML 6. Use nlohmann/json only for OAuth JSON. Build direct media URLs from the selected `Part.key` and server URI. Build the Plex universal-transcoder HLS request when the selected media requires server-side transcoding. Report timeline changes at most once per five seconds and immediately on pause, stop, and completion.

- [ ] **Step 7: Run fixture tests to verify they pass.**

  ```powershell
  rtk cmake --build host/build --config Release --target plex_fixture_tests
  rtk ctest --test-dir host/build -C Release -R plex_fixture_tests --output-on-failure
  ```

  Expected: PASS with no token or password values in captured diagnostics.

- [ ] **Step 8: Commit the Plex API checkpoint.**

  ```powershell
  rtk git add host/PlexTypes.hpp host/TokenStore.* host/PlexClient.* host/tests/PlexFixtureTests.cpp host/CMakeLists.txt
  rtk git commit -m "feat: add native plex api client"
  ```

### Task 4: Add the libmpv software-render player

**Files:**
- Create: `host/MpvPlayer.hpp`
- Create: `host/MpvPlayer.cpp`
- Modify: `host/CMakeLists.txt`
- Modify: `host/tests/MpvProbe.cpp`

**Interfaces:**
- Consumes: `PlexPlayback` from `PlexClient` and a native helper tick; the D3D11 device/context remains owned by the UI.
- Produces: `MpvPlayer::Load`, `Play`, `Pause`, `Stop`, `Seek`, `SetVolume`, `SetAudioTrack`, `SetSubtitleTrack`, `PollEvents`, `Render`, and `CopyFrameTo`.

  Use this public shape:

  ```cpp
  class MpvPlayer {
  public:
      bool Initialize(HWND window, ID3D11Device* device, ID3D11DeviceContext* context);
      void Shutdown();
      bool Load(const PlexPlayback& playback);
      void Play();
      void Pause();
      void Stop();
      void Seek(double seconds);
      void SetVolume(int percent);
      void SetAudioTrack(int trackId);
      void SetSubtitleTrack(int trackId);
      void PollEvents(std::vector<MpvEvent>& events);
      bool Render();
      bool CopyFrameTo(uint8_t* destination, uint32_t destinationStride);
  };
  ```

  `MpvPlayer` owns the DLL `HMODULE` and a private function table populated by
  `GetProcAddress`; production code must not link against `libmpv.dll.a`.
  The render context uses `MPV_RENDER_API_TYPE_SW` with `bgr0` output at
  640x360; the helper adds opaque alpha before publishing BGRA pixels.

  Define `MpvEvent` in `MpvPlayer.hpp` with `Kind` values `Loaded`, `Ended`,
  `Error`, `PositionChanged`, and `TracksChanged`, plus `std::string message`,
  `double positionSeconds`, and `int errorCode` fields.

- [ ] **Step 1: Extend the probe to initialize, set a property, and destroy libmpv.**

  The probe must call `LoadLibraryW(L"libmpv-2.dll")`, resolve `mpv_create`, `mpv_initialize`, `mpv_get_property_string`, `mpv_free`, and `mpv_destroy` with `GetProcAddress`, call `mpv_set_option_string(handle, "vo", "libmpv")`, initialize, read `mpv_version`, free the returned string, and destroy the handle.

- [ ] **Step 2: Run the probe and record the x86 result.**

  ```powershell
  rtk cmake --build host/build --config Release --target mpv_probe
  rtk ctest --test-dir host/build -C Release -R mpv_probe --output-on-failure
  ```

  Expected: PASS with the pinned x86 libmpv DLL discoverable beside the executable.

- [ ] **Step 3: Create the libmpv software render target.**

  Create the libmpv render context with `MPV_RENDER_API_TYPE_SW`, render into a 64-byte-aligned 640x360 `bgr0` buffer, and let `NativeUi` upload the opaque BGRA copy to its D3D11 texture. Do not assume the pinned i686 libmpv package has a D3D11 render backend.

- [ ] **Step 4: Implement playback commands and properties.**

  Send `loadfile`, `set pause`, `stop`, `seek`, `set volume`, `set aid`, and `set sid` through libmpv. Poll `MPV_EVENT_FILE_LOADED`, `MPV_EVENT_END_FILE`, `MPV_EVENT_PROPERTY_CHANGE`, and `MPV_EVENT_LOG_MESSAGE`. Convert end/error events into typed `MpvEvent` values consumed by the UI and Plex timeline layer.

- [ ] **Step 5: Implement software render and raw-frame copy.**

  `Render()` calls `mpv_render_context_update` and `mpv_render_context_render` when `MPV_RENDER_UPDATE_FRAME` is set. `CopyFrameTo()` converts `bgr0` to opaque BGRA rows for the frame publisher and native UI. Return false when no frame exists; do not publish stale partial data.

- [ ] **Step 6: Add the native player smoke test.**

  Extend `MpvProbe` to load a local controlled test stream supplied as a command-line argument, wait for `MPV_EVENT_FILE_LOADED` or an error for five seconds, and exit nonzero on initialization/load failure. Keep the test independent of a user's Plex account.

- [ ] **Step 7: Run player checks.**

  ```powershell
  rtk cmake --build host/build --config Release --target mpv_probe
  rtk ctest --test-dir host/build -C Release -R mpv_probe --output-on-failure
  ```

  Expected: API initialization passes; the optional local stream probe reports either a loaded stream or a clear controlled-stream failure, never a crash.

- [ ] **Step 8: Commit the libmpv checkpoint.**

  ```powershell
  rtk git add host/MpvPlayer.* host/tests/MpvProbe.cpp host/CMakeLists.txt
  rtk git commit -m "feat: add native mpv playback"
  ```

### Task 5: Build the Dear ImGui Plex interface

**Files:**
- Create: `host/NativeUi.hpp`
- Create: `host/NativeUi.cpp`
- Create: `host/tests/NativeUiTests.cpp`
- Modify: `host/CMakeLists.txt`

**Interfaces:**
- Consumes: `PlexClient`, `MpvPlayer`, `FramePublisher`, and native helper state.
- Produces: `NativeUi::Initialize`, `Draw`, `Show`, `Hide`, `IsVisible`, and `Shutdown`.

  Use this composition-root interface:

  ```cpp
  class NativeUi {
  public:
      bool Initialize(HWND window, ID3D11Device* device, ID3D11DeviceContext* context,
                      PlexClient& plex, MpvPlayer& player, FramePublisher& frames);
      void Draw();
      void Show();
      void Hide();
      bool IsVisible() const;
      void Shutdown();
  };
  ```

- [ ] **Step 1: Add the native UI smoke test state model.**

  Use a deterministic `NativeUiState` with views `Login`, `Servers`, `Home`, `Library`, `Search`, `Details`, and `Player`. Test that each `PlexEvent` moves the state to the expected view and that a player error returns to the details/player view with an error string.

  The state test should use direct assertions, for example:

  ```cpp
  NativeUiState state;
  state.Apply(PlexEvent{LoginSucceeded{}});
  assert(state.view == View::Home);
  state.Apply(PlexEvent{PlayerError{"stream failed"}});
  assert(state.view == View::Player);
  assert(state.error == "stream failed");
  ```

- [ ] **Step 2: Run the state test to verify it fails.**

  Run the host test target after adding the state-only test. Expected: FAIL because `NativeUi` and its state reducer do not exist yet.

- [ ] **Step 3: Implement Dear ImGui initialization and frame lifetime.**

  Initialize the Win32 and DX11 backends once. Each `Draw()` must call `ImGui_ImplDX11_NewFrame`, `ImGui_ImplWin32_NewFrame`, `ImGui::NewFrame`, render the current view, call `ImGui::Render`, and submit the draw data. Shutdown must destroy the backends before releasing D3D resources.

- [ ] **Step 4: Implement login, server, home, library, search, details, and player views.**

  Wire visible buttons to the exact `PlexClient` methods. Show Continue Watching, Recently Added, sections, search results, seasons/episodes, metadata, resume/play, and server/profile selection. Use the Plex thumbnail URL only through a controlled image request/cache in memory; do not persist media or thumbnails to disk.

- [ ] **Step 5: Implement player controls and track selection.**

  Wire Play/Pause/Stop, seek, volume, audio stream, subtitle stream, helper fullscreen, and Hide controls to `MpvPlayer` and the parent host. Keep the helper window responsive while mpv or Plex work is pending by showing a disabled/loading state rather than blocking the UI thread.

- [ ] **Step 6: Run the native UI state and host build checks.**

  ```powershell
  rtk cmake --build host/build --config Release --target native_ui_tests wxl-video-host
  rtk ctest --test-dir host/build -C Release -R native_ui_tests --output-on-failure
  ```

  Expected: state transitions pass and the helper links without WebView2 headers.

- [ ] **Step 7: Commit the UI checkpoint.**

  ```powershell
  rtk git add host/NativeUi.* host/CMakeLists.txt
  rtk git commit -m "feat: add native plex ui"
  ```

### Task 6: Replace the WebView2 composition root

**Files:**
- Modify: `host/VideoHost.cpp`
- Modify: `host/CMakeLists.txt`
- Modify: `host/VideoShared.hpp`

**Interfaces:**
- Consumes: `PlexClient`, `MpvPlayer`, `NativeUi`, `FramePublisher`, and v2 shared control commands.
- Produces: one native helper window that continues rendering while hidden, exits when the WarcraftXL parent exits, and publishes frames without WebView2/WIC.

- [ ] **Step 1: Add a native host initialization test seam.**

  Extract a testable `HostRuntime::Initialize(HWND)`/`Shutdown()` pair from the current `WinMain` globals. The seam must report which component failed instead of showing a WebView2-specific message box.

- [ ] **Step 2: Run the host build against the seam.**

  ```powershell
  rtk cmake --build host/build --config Release --target wxl-video-host
  ```

  Expected: the composition-root seam builds while the old WebView2 implementation is still present; the next step removes those obsolete calls.

- [ ] **Step 3: Remove WebView2, WIC, virtual-host, and capture-timer code.**

  Delete the WebView2 controller/environment, WIC factory, `ExecutePlayerScript`, `PublishPreview`, `CapturePreview`, `CreateWebView`, `kVirtualHost`, `kPlayerUrl`, and the 67 ms capture timer. Keep parent PID parsing, show/hide messages, shared mappings, and the window procedure.

- [ ] **Step 4: Create the D3D11 swap chain and component graph.**

  Create a BGRA swap chain for the helper window, initialize `PlexClient`, `MpvPlayer`, `FramePublisher`, and `NativeUi` in that order, and shut them down in reverse order. On every `WM_SIZE`, resize the swap chain and notify the native UI/player; do not destroy the raw world frame target unless the fixed 640x360 target changes.

- [ ] **Step 5: Wire the message loop and background behavior.**

  Use a 16 ms host tick to drain Plex/mpv events, process extension control commands, render the helper UI, publish only new frames, and present. `HideHostWindow` hides the native window but does not pause mpv or audio. `CloseHost` stops mpv, closes shared mappings, and destroys D3D resources.

- [ ] **Step 6: Wire v2 extension commands.**

  Map `Show`/`Hide` to the native window, `Play`/`Pause`/`Stop` to mpv, and volume writes to mpv's volume property. Ignore duplicate sequence numbers and reject unknown commands without crashing.

- [ ] **Step 7: Build and inspect the native host.**

  ```powershell
  rtk cmake --build host/build --config Release --target wxl-video-host
  rtk rg -n "WebView2|CapturePreview|WIC|player.html|YouTube" host
  ```

  Expected: Release build succeeds, the search returns no old runtime references, and the helper starts without requiring the WebView2 runtime.

- [ ] **Step 8: Commit the host checkpoint.**

  ```powershell
  rtk git add host/VideoHost.cpp host/VideoShared.hpp host/CMakeLists.txt
  rtk git commit -m "refactor: replace webview host"
  ```

### Task 7: Update the WarcraftXL extension while preserving world placement

**Files:**
- Modify: `extension/src/VideoShared.hpp`
- Modify: `extension/src/VideoSurface.hpp`
- Modify: `extension/src/VideoSurface.cpp`
- Modify: `extension/src/Module.cpp`

**Interfaces:**
- Consumes: v2 raw BGRA shared frames and v2 control commands from the native helper.
- Produces: the existing 16:9 world screen with native Plex labels, native helper controls, and unchanged placement persistence.

- [ ] **Step 1: Add the extension-side ABI validation check.**

  Keep `ValidateFrames()` strict on magic, version, header size, width, height, stride, and frame bytes. Update the expected version to 2 and leave a clear “native Plex helper ABI mismatch” status when validation fails.

- [ ] **Step 2: Replace URL-specific control code.**

  Remove `PasteUrlFromClipboard`, `AppendUrlCharacter`, `editingUrl_`, `urlInput_`, and URL typing branches from `OnInput`. Keep only commands that open/show/hide the helper and control playback. Remove the old URL-entry input hook if no remaining event consumes it.

- [ ] **Step 3: Preserve and verify placement behavior.**

  Do not change `PlaceInFront`, `Rotate`, `SavePlacement`, `LoadPlacement`, `DrawWorldScreen`, or the `width_`/16:9 calculations except for necessary status text. Confirm that width changes still call `SavePlacement()` and that saved screen size is restored from `world-screen.tsv`.

- [ ] **Step 4: Update the in-game panel.**

  Replace YouTube labels with Plex labels. Provide Open Plex Player, Hide Plex Player, Play, Pause, Stop, master volume, distance-based audio, placement, width, rotation, height, visibility, depth, and diagnostic controls. Display the current native helper/stream status returned through the control/frame bridge when available.

- [ ] **Step 5: Build the extension against WarcraftXL ABI 1.1.**

  Copy the updated `extension/src` into the pinned `wxl-core/extensions/wxl-video-screen/` checkout and run the repository-native extension build. Expected: the DLL builds with no ABI or missing-symbol errors, and no YouTube/WebView2 text remains in extension source.

- [ ] **Step 6: Commit the extension checkpoint.**

  ```powershell
  rtk git add extension/src
  rtk git commit -m "feat: connect warcraftxl to native plex"
  ```

### Task 8: Update packaging, notices, and operator documentation

**Files:**
- Modify: `README.md`
- Modify: `THIRD_PARTY_NOTICES.md`
- Modify: `wxl.json`
- Delete: `player.html`

**Interfaces:**
- Consumes: the completed native host and extension assets.
- Produces: a release package that contains only the native helper, extension DLL, manifest, and documentation required by the spec.

- [ ] **Step 1: Rewrite README installation and usage.**

  Document the 32-bit WarcraftXL requirement, native helper launch, x86 libmpv runtime packaging, first-run Plex PIN login, server selection, browse/search/playback, helper hide behavior, and in-game placement/resize controls. State that media remains on the Plex server and the world screen is client-local.

- [ ] **Step 2: Update third-party notices.**

  Identify the pinned Dear ImGui revision and MIT notice, libmpv build/source and applicable license notice, and nlohmann/json notice. Remove WebView2 and YouTube notices. Record that the release ships the required x86 libmpv runtime beside the helper if packaging includes it.

- [ ] **Step 3: Update the manifest.**

  Remove `player.html` from `extension.assets`, change the version from `0.4.0` to `0.5.0`, and update the title/tagline/description to describe a native Plex player and movable world-space screen.

- [ ] **Step 4: Delete the obsolete player page and scan release references.**

  Delete `player.html`, then run:

  ```powershell
  rtk rg -n "YouTube|WebView2|player.html|wxl-video.local|CapturePreview" README.md THIRD_PARTY_NOTICES.md wxl.json host extension
  ```

  Expected: no obsolete runtime or package references remain. Historical design-spec references are allowed outside the release source/docs scan.

- [ ] **Step 5: Commit the packaging checkpoint.**

  Delete `player.html` with the repository patch tool, then stage the release files:

  ```powershell
  rtk git add -A -- README.md THIRD_PARTY_NOTICES.md wxl.json player.html
  rtk git commit -m "docs: package native plex player"
  ```

### Task 9: Full verification and runtime acceptance

**Files:**
- Modify: `host/CMakeLists.txt` only if a test target or packaging check is missing.
- Modify: `README.md` only if verified operator steps differ from implementation.

**Interfaces:**
- Consumes: all completed host, extension, package, and test checkpoints.
- Produces: fresh build artifacts plus evidence for each acceptance item in the approved spec.

- [ ] **Step 1: Run formatting and source checks.**

  ```powershell
  rtk git diff --check HEAD^ HEAD
  rtk rg -n "TBD|TODO|FIXME" host extension README.md THIRD_PARTY_NOTICES.md wxl.json
  ```

  Expected: no whitespace errors and no unfinished implementation markers.

- [ ] **Step 2: Build all native targets.**

  ```powershell
  rtk cmake -S host -B host/build -A Win32 -DMPV_ROOT='C:/deps/libmpv-x86'
  rtk cmake --build host/build --config Release
  rtk ctest --test-dir host/build -C Release --output-on-failure
  ```

  Expected: helper, probe, frame tests, Plex fixture tests, and UI state tests build and pass.

- [ ] **Step 3: Verify the packaged helper dependencies.**

  Confirm the release staging directory contains the extension DLL, `wxl-video-host.exe`, x86 `libmpv-2.dll` and required runtime dependencies, `wxl.json`, README, license, and third-party notices, and does not contain `player.html`, WebView2 SDK files, or browser profile data.

- [ ] **Step 4: Run the Plex account/server acceptance flow.**

  In a controlled test account, sign in through the system browser, sign out, sign back in, switch managed profile if available, select a server, browse a library, search, open an item, play/resume, seek, pause, and stop. Verify direct play and server-side HLS transcoding with media that requires each path.

- [ ] **Step 5: Run track/timeline acceptance.**

  Change audio and subtitle tracks, pause/resume, complete an item, and verify the Plex server reflects position and watched state without exposing the token in helper or WarcraftXL logs.

- [ ] **Step 6: Run WarcraftXL world-screen acceptance.**

  Launch the extension, open the native Plex helper, start playback, hide the helper, and confirm video/audio continues. In game, place the screen, move it, increase/decrease width, rotate it, change height placement, toggle visibility/depth/outline, leave and re-enter a world, and confirm the saved `world-screen.tsv` placement remains correct.

- [ ] **Step 7: Run lifecycle and performance acceptance.**

  Close the helper, restart it, terminate WarcraftXL, trigger a D3D device reset, change maps, and reconnect playback. Confirm no crash, stale texture use, or shared-memory deadlock. Record observed world-screen frame rate and CPU usage; only schedule GPU interop if the raw 640x360 bridge misses the 30 FPS target.

- [ ] **Step 8: Record final evidence and commit only verified changes.**

  Capture the Release build command, CTest output, package file list, SHA-256 hashes for the helper/extension package, and the manual acceptance results in the task response. Do not claim live Plex or WarcraftXL success from compilation alone.

## Execution Order

Run Tasks 1–2 first as the dependency and transport feasibility gate. Then run Tasks 3–5 to build independently testable API, playback, and UI components. Task 6 composes the native helper, Task 7 reconnects WarcraftXL, Task 8 removes obsolete packaging, and Task 9 verifies the complete system.

The implementation should stop after Task 1 if no compatible x86 libmpv build can be produced; do not start a large UI rewrite against a 64-bit-only media library.
