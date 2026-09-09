# Third-party notices

## libmpv

The release uses the x86 Windows libmpv build from the mpv-winbuild-cmake
`20260608` release:

`mpv-dev-i686-20260608-git-6444c05.7z`

The helper loads `libmpv-2.dll` dynamically. mpv and its bundled multimedia
components remain under their upstream licenses. Preserve the upstream mpv,
FFmpeg, and codec notices distributed with the source/runtime package when
redistributing the DLL.

## Dear ImGui

The native helper UI uses Dear ImGui `v1.92.9b`, including its Win32 and
Direct3D 11 backends. Dear ImGui is distributed under the MIT license. The
corresponding license text is included at `host/third_party/imgui/LICENSE.txt`.

## nlohmann/json

Plex OAuth responses use nlohmann/json `v3.12.0`, distributed under the MIT
license. The single-header implementation is included at
`host/third_party/json.hpp`.

The project itself remains GPL-3.0-or-later; see [LICENSE](LICENSE).
