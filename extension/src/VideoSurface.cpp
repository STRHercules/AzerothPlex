// Placeable world-space native Plex video surface for WarcraftXL build 12340.
// Copyright (C) 2026 WarcraftXL contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "VideoSurface.hpp"

#include "VideoShared.hpp"

#include "game/Camera.hpp"
#include "game/Gfx.hpp"
#include "game/World.hpp"

#include <d3d9.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>

namespace wxl_video_screen
{
    VideoSurface::SceneClearFn VideoSurface::originalSceneClear_ = nullptr;

    namespace
    {
        constexpr char kTag[] = "wxl-azeroth-plex";
        constexpr wchar_t kHostWindowClass[] = L"WXLNativePlexHost";
        constexpr float kPi = 3.14159265358979323846f;

        void ModuleAddressMarker() {}

        bool Finite3(const float value[3])
        {
            return std::isfinite(value[0]) && std::isfinite(value[1]) &&
                   std::isfinite(value[2]) && std::abs(value[0]) <= 65536.0f &&
                   std::abs(value[1]) <= 65536.0f && std::abs(value[2]) <= 65536.0f;
        }

        struct Vertex
        {
            float x, y, z;
            D3DCOLOR color;
            float u, v;
        };

        void FormatTime(char* output, size_t size, uint32_t milliseconds)
        {
            const uint32_t totalSeconds = milliseconds / 1000;
            const uint32_t seconds = totalSeconds % 60;
            const uint32_t minutes = (totalSeconds / 60) % 60;
            const uint32_t hours = totalSeconds / 3600;
            if (hours > 0)
                std::snprintf(output, size, "%u:%02u:%02u", hours, minutes, seconds);
            else
                std::snprintf(output, size, "%u:%02u", minutes, seconds);
        }
    }

    VideoSurface& VideoSurface::Instance()
    {
        static VideoSurface instance;
        return instance;
    }

    VideoSurface::VideoSurface()
    {
        on<&VideoSurface::OnFrame>(wxl::events::Event::OnFrame);
        on<&VideoSurface::OnWorldSceneEnd>(wxl::events::Event::OnWorldSceneEnd);
        on<&VideoSurface::OnInput>(wxl::events::Event::OnInput);
        on<&VideoSurface::OnUpdate>(wxl::events::Event::OnUpdate);
        on<&VideoSurface::OnDeviceLost>(wxl::events::Event::OnDeviceLost);
        on<&VideoSurface::OnDeviceReset>(wxl::events::Event::OnDeviceReset);
        on<&VideoSurface::OnWorldLeave>(wxl::events::Event::OnWorldLeave);
    }

    bool VideoSurface::Initialize(const WXL_Api* api)
    {
        if (!api || initialized_) return initialized_;
        api_ = api;
        frameScratch_.resize(wxl_video_shared::kFrameBytes);
        ConnectControl();
        ConnectUi();
        LoadPlacement();
        if (!api_->HookAttach || !api_->HookAttach(
                "wxl-azeroth-plex.scene-clear",
                wxl::game::gx::kSceneClearSeam,
                reinterpret_cast<void*>(&VideoSurface::SceneClearHook),
                reinterpret_cast<void**>(&originalSceneClear_),
                WXL_HOOK_DEFAULT_PRIORITY))
        {
            status_ = "Could not attach the pre-world screen renderer.";
            api_->Log(WXL_LOG_ERROR, kTag, "%s", status_.c_str());
            return false;
        }
        initialized_ = true;
        return true;
    }

    void __cdecl VideoSurface::SceneClearHook(uint32_t flags, uint32_t colour)
    {
        if (originalSceneClear_) originalSceneClear_(flags, colour);

        auto& self = Instance();
        if (self.preWorldDrawn_ || self.pinned_ || !self.initialized_ || !self.depthTest_ || !self.visible_ ||
            !self.placed_ || (flags & (wxl::game::gx::clear::kColor |
                                      wxl::game::gx::clear::kDepth)) !=
                             (wxl::game::gx::clear::kColor |
                              wxl::game::gx::clear::kDepth) ||
            wxl::game::world::CurrentMapId() != self.mapId_)
            return;

        auto* device = static_cast<IDirect3DDevice9*>(wxl::game::gx::RawDevice());
        if (!device) return;

        IDirect3DSurface9* depth = nullptr;
        device->GetDepthStencilSurface(&depth);
        self.DrawWorldScreen(device, depth, true);
        if (depth) depth->Release();
        self.preWorldDrawn_ = true;
    }

    void VideoSurface::OnFrame(const wxl::events::FrameArgs&)
    {
        preWorldDrawn_ = false;
    }

    void VideoSurface::OnInput(const wxl::events::InputArgs& args)
    {
        if (!pinned_ || !visible_ || (api_ && api_->UiIsOpen && api_->UiIsOpen())) return;

        auto* device = static_cast<IDirect3DDevice9*>(wxl::game::gx::RawDevice());
        if (!device) return;
        const LPARAM lparam = static_cast<LPARAM>(args.lparam);
        const float x = static_cast<float>(static_cast<short>(LOWORD(lparam)));
        const float y = static_cast<float>(static_cast<short>(HIWORD(lparam)));

        if (args.message == WM_LBUTTONDOWN)
        {
            float left = 0.0f;
            float top = 0.0f;
            float right = 0.0f;
            float bottom = 0.0f;
            if (!GetPinnedRect(device, left, top, right, bottom) ||
                x < left || x > right || y < top || y > bottom)
                return;

            pinnedDragging_ = true;
            pinnedResizing_ = x >= right - 24.0f && y >= bottom - 24.0f;
            if (!pinnedResizing_)
            {
                pinnedDragOffsetX_ = x - left;
                pinnedDragOffsetY_ = y - top;
            }
            D3DDEVICE_CREATION_PARAMETERS parameters{};
            if (SUCCEEDED(device->GetCreationParameters(&parameters)) && parameters.hFocusWindow)
                SetCapture(parameters.hFocusWindow);
            *args.handled = true;
            return;
        }

        if (pinnedDragging_ && args.message == WM_MOUSEMOVE)
        {
            D3DVIEWPORT9 viewport{};
            if (FAILED(device->GetViewport(&viewport)) || viewport.Width == 0 || viewport.Height == 0)
                return;

            const float viewportWidth = static_cast<float>(viewport.Width);
            const float viewportHeight = static_cast<float>(viewport.Height);
            if (pinnedResizing_)
            {
                pinnedWidth_ = (x - static_cast<float>(viewport.X)) / viewportWidth - pinnedLeft_;
            }
            else
            {
                pinnedLeft_ = (x - pinnedDragOffsetX_ - static_cast<float>(viewport.X)) /
                              viewportWidth;
                pinnedTop_ = (y - pinnedDragOffsetY_ - static_cast<float>(viewport.Y)) /
                             viewportHeight;
            }
            wxl_video_shared::ClampPinnedLayout(pinnedLeft_, pinnedTop_, pinnedWidth_);
            *args.handled = true;
            return;
        }

        if (pinnedDragging_ && args.message == WM_LBUTTONUP)
        {
            pinnedDragging_ = false;
            pinnedResizing_ = false;
            ReleaseCapture();
            SavePlacement();
            *args.handled = true;
            return;
        }

        if (pinnedDragging_ && args.message == WM_CANCELMODE)
        {
            pinnedDragging_ = false;
            pinnedResizing_ = false;
            ReleaseCapture();
            SavePlacement();
            *args.handled = true;
        }
    }

    std::wstring VideoSurface::ExtensionDirectory() const
    {
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(&ModuleAddressMarker), &module))
            return {};

        wchar_t path[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
        if (!length || length >= MAX_PATH) return {};
        return std::filesystem::path(path).parent_path().wstring();
    }

    bool VideoSurface::OpenHost()
    {
        if (FindWindowW(kHostWindowClass, nullptr)) return true;

        const std::wstring directory = ExtensionDirectory();
        const std::wstring hostPath = directory.empty()
            ? std::wstring{} : (std::filesystem::path(directory) / L"wxl-video-host.exe").wstring();
        if (hostPath.empty() || GetFileAttributesW(hostPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            status_ = "Plex playback worker is missing from the extension folder.";
            if (api_) api_->Log(WXL_LOG_ERROR, kTag, "%s", status_.c_str());
            return false;
        }

        if (ConnectUi())
        {
            auto* bridge = static_cast<wxl_video_shared::UiBridge*>(uiView_);
            std::memset(bridge, 0, sizeof(*bridge));
            bridge->magic = wxl_video_shared::kUiMagic;
            bridge->version = wxl_video_shared::kUiVersion;
            bridge->structBytes = sizeof(*bridge);
            bridge->slots[0].structBytes = sizeof(wxl_video_shared::UiSnapshot);
            bridge->slots[1].structBytes = sizeof(wxl_video_shared::UiSnapshot);
        }

        std::wstring commandLine = L"\"" + hostPath + L"\" --parent-pid " +
                                   std::to_wstring(GetCurrentProcessId()) + L" --background";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(hostPath.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                            CREATE_NEW_PROCESS_GROUP, nullptr, directory.c_str(), &startup, &process))
        {
            char error[128]{};
            std::snprintf(error, sizeof(error), "Could not start helper (Win32 error %lu).",
                          static_cast<unsigned long>(GetLastError()));
            status_ = error;
            if (api_) api_->Log(WXL_LOG_ERROR, kTag, "%s", status_.c_str());
            return false;
        }

        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        status_ = "Background Plex player started.";
        return true;
    }

    void VideoSurface::HideHost()
    {
        plexPanelOpen_ = false;
        status_ = "Plex player is running in the background.";
    }

    void VideoSurface::CloseHost()
    {
        if (HWND host = FindWindowW(kHostWindowClass, nullptr))
            PostMessageW(host, WM_CLOSE, 0, 0);
        plexPanelOpen_ = false;
        status_ = "Plex player closed.";
    }

    bool VideoSurface::ConnectControl()
    {
        if (controlView_)
        {
            const auto* control = static_cast<const wxl_video_shared::ControlBlock*>(controlView_);
            return control->magic == wxl_video_shared::kControlMagic &&
                   control->version == wxl_video_shared::kControlVersion &&
                   control->structBytes == sizeof(*control);
        }

        controlMapping_ = CreateFileMappingW(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
            static_cast<DWORD>(sizeof(wxl_video_shared::ControlBlock)),
            wxl_video_shared::kControlMappingName);
        if (!controlMapping_) return false;
        const bool created = GetLastError() != ERROR_ALREADY_EXISTS;
        controlView_ = MapViewOfFile(controlMapping_, FILE_MAP_ALL_ACCESS, 0, 0,
                                     sizeof(wxl_video_shared::ControlBlock));
        if (!controlView_)
        {
            CloseHandle(controlMapping_);
            controlMapping_ = nullptr;
            return false;
        }

        auto* control = static_cast<wxl_video_shared::ControlBlock*>(controlView_);
        if (created)
        {
            std::memset(control, 0, sizeof(*control));
            control->magic = wxl_video_shared::kControlMagic;
            control->version = wxl_video_shared::kControlVersion;
            control->structBytes = sizeof(*control);
            control->volumePercent = masterVolume_;
        }
        if (control->magic != wxl_video_shared::kControlMagic ||
            control->version != wxl_video_shared::kControlVersion ||
            control->structBytes != sizeof(*control))
        {
            UnmapViewOfFile(controlView_);
            controlView_ = nullptr;
            CloseHandle(controlMapping_);
            controlMapping_ = nullptr;
            return false;
        }
        return true;
    }

    bool VideoSurface::SendCommand(LONG command)
    {
        if (!ConnectControl() || !OpenHost()) return false;
        auto* control = static_cast<wxl_video_shared::ControlBlock*>(controlView_);
        InterlockedExchange(&control->command, command);
        MemoryBarrier();
        LONG sequence = control->commandSequence + 1;
        if (sequence <= 0) sequence = 1;
        InterlockedExchange(&control->commandSequence, sequence);
        status_ = "Command sent to the background Plex player.";
        return true;
    }

    bool VideoSurface::ConnectUi()
    {
        if (uiView_)
        {
            const auto* bridge = static_cast<const wxl_video_shared::UiBridge*>(uiView_);
            return bridge->magic == wxl_video_shared::kUiMagic &&
                   bridge->version == wxl_video_shared::kUiVersion &&
                   bridge->structBytes == sizeof(*bridge);
        }

        uiMapping_ = CreateFileMappingW(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
            static_cast<DWORD>(sizeof(wxl_video_shared::UiBridge)),
            wxl_video_shared::kUiMappingName);
        if (!uiMapping_) return false;
        const bool created = GetLastError() != ERROR_ALREADY_EXISTS;
        uiView_ = MapViewOfFile(uiMapping_, FILE_MAP_ALL_ACCESS, 0, 0,
                                sizeof(wxl_video_shared::UiBridge));
        if (!uiView_)
        {
            CloseHandle(uiMapping_);
            uiMapping_ = nullptr;
            return false;
        }

        auto* bridge = static_cast<wxl_video_shared::UiBridge*>(uiView_);
        if (created)
        {
            std::memset(bridge, 0, sizeof(*bridge));
            bridge->magic = wxl_video_shared::kUiMagic;
            bridge->version = wxl_video_shared::kUiVersion;
            bridge->structBytes = sizeof(*bridge);
            bridge->slots[0].structBytes = sizeof(wxl_video_shared::UiSnapshot);
            bridge->slots[1].structBytes = sizeof(wxl_video_shared::UiSnapshot);
        }
        if (bridge->magic != wxl_video_shared::kUiMagic ||
            bridge->version != wxl_video_shared::kUiVersion ||
            bridge->structBytes != sizeof(*bridge))
        {
            UnmapViewOfFile(uiView_);
            uiView_ = nullptr;
            CloseHandle(uiMapping_);
            uiMapping_ = nullptr;
            return false;
        }
        return true;
    }

    bool VideoSurface::SendUiCommand(wxl_video_shared::UiCommand command, LONG index,
                                     LONG value, const char* text)
    {
        if (!ConnectUi() || !OpenHost()) return false;
        auto* bridge = static_cast<wxl_video_shared::UiBridge*>(uiView_);
        std::memset(&bridge->command, 0, sizeof(bridge->command));
        bridge->command.command = static_cast<LONG>(command);
        bridge->command.index = index;
        bridge->command.value = value;
        if (text)
        {
            const size_t length = std::min(std::strlen(text),
                                           wxl_video_shared::kUiTextBytes - 1);
            std::memcpy(bridge->command.text, text, length);
            bridge->command.text[length] = '\0';
        }
        MemoryBarrier();
        LONG sequence = bridge->commandSequence + 1;
        if (sequence <= 0) sequence = 1;
        InterlockedExchange(&bridge->commandSequence, sequence);
        return true;
    }

    bool VideoSurface::RefreshUiState()
    {
        if (!ConnectUi()) return false;
        wxl_video_shared::UiSnapshot snapshot{};
        if (!wxl_video_shared::ReadUiSnapshot(
                static_cast<const wxl_video_shared::UiBridge*>(uiView_), snapshot))
            return false;
        uiState_ = snapshot;
        return true;
    }

    void VideoSurface::SendVolume(int volume)
    {
        volume = std::clamp(volume, 0, 100);
        effectiveVolume_ = volume;
        if (!ConnectControl() || volume == lastSentVolume_) return;
        auto* control = static_cast<wxl_video_shared::ControlBlock*>(controlView_);
        InterlockedExchange(&control->volumePercent, volume);
        MemoryBarrier();
        LONG sequence = control->volumeSequence + 1;
        if (sequence <= 0) sequence = 1;
        InterlockedExchange(&control->volumeSequence, sequence);
        lastSentVolume_ = volume;
    }


    bool VideoSurface::PlaceInFront()
    {
        namespace camera = wxl::game::camera;
        namespace world = wxl::game::world;

        const int map = world::CurrentMapId();
        const unsigned long long guid = world::ActivePlayerGuid();
        void* player = guid ? world::ResolveObject(guid, world::kTypeMaskPlayer) : nullptr;
        if (map < 0 || !player)
        {
            status_ = "Enter the world before placing the screen.";
            return false;
        }

        float playerPosition[3]{};
        float cameraPosition[3]{};
        world::UnitPosition(player, playerPosition);
        camera::GetPosition(cameraPosition);
        if (!Finite3(playerPosition) || !Finite3(cameraPosition))
        {
            status_ = "Player or camera position is unavailable.";
            return false;
        }

        float forwardX = playerPosition[0] - cameraPosition[0];
        float forwardY = playerPosition[1] - cameraPosition[1];
        float length = std::sqrt(forwardX * forwardX + forwardY * forwardY);
        if (!(length > 0.001f) || !std::isfinite(length))
        {
            forwardX = 1.0f;
            forwardY = 0.0f;
            length = 1.0f;
        }
        forwardX /= length;
        forwardY /= length;

        right_[0] = -forwardY;
        right_[1] = forwardX;
        right_[2] = 0.0f;
        const float height = width_ * 9.0f / 16.0f;
        center_[0] = playerPosition[0] + forwardX * placeDistance_;
        center_[1] = playerPosition[1] + forwardY * placeDistance_;
        center_[2] = playerPosition[2] + bottomOffset_ + height * 0.5f;
        mapId_ = map;
        placed_ = true;
        visible_ = 1;
        SavePlacement();
        status_ = "World screen placed in front of your character.";
        if (api_)
            api_->Log(WXL_LOG_INFO, kTag, "placed world screen map=%d xyz=%.3f %.3f %.3f width=%.2f",
                      mapId_, center_[0], center_[1], center_[2], width_);
        return true;
    }

    void VideoSurface::Rotate(float degrees)
    {
        if (!placed_) return;
        const float radians = degrees * kPi / 180.0f;
        const float cosine = std::cos(radians);
        const float sine = std::sin(radians);
        const float x = right_[0] * cosine - right_[1] * sine;
        const float y = right_[0] * sine + right_[1] * cosine;
        right_[0] = x;
        right_[1] = y;
        SavePlacement();
    }

    void VideoSurface::SavePlacement()
    {
        if (!placed_) return;
        const std::wstring directory = ExtensionDirectory();
        if (directory.empty()) return;

        const std::filesystem::path target = std::filesystem::path(directory) / L"world-screen.tsv";
        const std::filesystem::path temporary = target.wstring() + L".tmp";
        std::ofstream output(temporary, std::ios::trunc);
        if (!output) return;
        output.imbue(std::locale::classic());
        output << "wxl-video-screen-v1\t" << mapId_ << '\t'
               << std::setprecision(9) << center_[0] << '\t' << center_[1] << '\t' << center_[2] << '\t'
               << right_[0] << '\t' << right_[1] << '\t' << right_[2] << '\t'
               << width_ << '\t' << visible_ << '\t' << depthTest_ << '\t' << depthBias_
               << '\t' << pinned_ << '\t' << pinnedLeft_ << '\t' << pinnedTop_
               << '\t' << pinnedWidth_ << '\n';
        output.close();
        if (!output) return;
        MoveFileExW(temporary.c_str(), target.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }

    void VideoSurface::LoadPlacement()
    {
        const std::wstring directory = ExtensionDirectory();
        if (directory.empty()) return;
        std::ifstream input(std::filesystem::path(directory) / L"world-screen.tsv");
        if (!input) return;
        input.imbue(std::locale::classic());

        std::string version;
        int map = -1;
        int visible = 1;
        int depth = 1;
        float center[3]{};
        float right[3]{};
        float width = 0.0f;
        float depthBias = -0.010f;
        int pinned = 0;
        float pinnedLeft = 0.64f;
        float pinnedTop = 0.04f;
        float pinnedWidth = 0.32f;
        if (!std::getline(input, version, '\t') || version != "wxl-video-screen-v1" ||
            !(input >> map) || input.get() != '\t' ||
            !(input >> center[0]) || input.get() != '\t' ||
            !(input >> center[1]) || input.get() != '\t' ||
            !(input >> center[2]) || input.get() != '\t' ||
            !(input >> right[0]) || input.get() != '\t' ||
            !(input >> right[1]) || input.get() != '\t' ||
            !(input >> right[2]) || input.get() != '\t' ||
            !(input >> width) || input.get() != '\t' ||
            !(input >> visible) || input.get() != '\t' || !(input >> depth))
            return;
        if (input.peek() == '\t')
        {
            input.get();
            if (!(input >> depthBias)) depthBias = -0.010f;
        }
        if (input.peek() == '\t')
        {
            input.get();
            if (!(input >> pinned)) pinned = 0;
        }
        if (input.peek() == '\t')
        {
            input.get();
            if (!(input >> pinnedLeft)) pinnedLeft = 0.64f;
        }
        if (input.peek() == '\t')
        {
            input.get();
            if (!(input >> pinnedTop)) pinnedTop = 0.04f;
        }
        if (input.peek() == '\t')
        {
            input.get();
            if (!(input >> pinnedWidth)) pinnedWidth = 0.32f;
        }

        const float rightLength = std::sqrt(right[0] * right[0] + right[1] * right[1]);
        if (map < 0 || !Finite3(center) || !(rightLength > 0.9f && rightLength < 1.1f) ||
            !std::isfinite(width) || width < 2.0f || width > 30.0f)
            return;

        mapId_ = map;
        std::memcpy(center_, center, sizeof(center_));
        right_[0] = right[0] / rightLength;
        right_[1] = right[1] / rightLength;
        right_[2] = 0.0f;
        width_ = width;
        visible_ = visible ? 1 : 0;
        depthTest_ = depth ? 1 : 0;
        depthBias_ = std::isfinite(depthBias) ? std::clamp(depthBias, -0.030f, 0.005f) : -0.010f;
        pinned_ = pinned ? 1 : 0;
        pinnedWidth_ = std::isfinite(pinnedWidth) ? pinnedWidth : 0.32f;
        pinnedLeft_ = std::isfinite(pinnedLeft) ? pinnedLeft : 0.64f;
        pinnedTop_ = std::isfinite(pinnedTop) ? pinnedTop : 0.04f;
        wxl_video_shared::ClampPinnedLayout(pinnedLeft_, pinnedTop_, pinnedWidth_);
        placed_ = true;
        status_ = "Restored the saved world-screen placement.";
    }

    bool VideoSurface::ValidateFrames() const
    {
        if (!mappingView_) return false;
        const auto* header = static_cast<const wxl_video_shared::FrameHeader*>(mappingView_);
        return header->magic == wxl_video_shared::kMagic &&
               header->version == wxl_video_shared::kVersion &&
               header->headerBytes == sizeof(*header) &&
               header->width == wxl_video_shared::kWidth &&
               header->height == wxl_video_shared::kHeight &&
               header->stride == wxl_video_shared::kStride &&
               header->frameBytes == wxl_video_shared::kFrameBytes;
    }

    bool VideoSurface::ConnectFrames()
    {
        if (mappingView_) return ValidateFrames();
        const DWORD now = GetTickCount();
        if (now - lastConnectAttempt_ < 1000) return false;
        lastConnectAttempt_ = now;

        mapping_ = OpenFileMappingW(FILE_MAP_READ, FALSE, wxl_video_shared::kMappingName);
        if (!mapping_) return false;
        mappingView_ = MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, wxl_video_shared::kMappingBytes);
        if (!mappingView_ || !ValidateFrames())
        {
            if (mappingView_) UnmapViewOfFile(mappingView_);
            mappingView_ = nullptr;
            CloseHandle(mapping_);
            mapping_ = nullptr;
            return false;
        }
        return true;
    }

    void VideoSurface::ReleaseTexture()
    {
        if (texture_)
        {
            texture_->Release();
            texture_ = nullptr;
        }
        textureDevice_ = nullptr;
        lastFrameSequence_ = 0;
    }

    bool VideoSurface::UploadLatestFrame(IDirect3DDevice9* device)
    {
        if (!device || !ConnectFrames() || !ValidateFrames()) return false;
        const auto* header = static_cast<const wxl_video_shared::FrameHeader*>(mappingView_);
        if (!header->ready) return false;

        const unsigned active = static_cast<unsigned>(header->activeIndex) & 1u;
        const LONG before = header->slotSequence[active];
        if (before <= 0 || before == lastFrameSequence_) return texture_ != nullptr;
        std::memcpy(frameScratch_.data(), wxl_video_shared::Pixels(mappingView_, active),
                    frameScratch_.size());
        MemoryBarrier();
        if (header->slotSequence[active] != before) return texture_ != nullptr;

        if (textureDevice_ != device)
            ReleaseTexture();
        if (!texture_)
        {
            if (FAILED(device->CreateTexture(wxl_video_shared::kWidth, wxl_video_shared::kHeight,
                                             1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8,
                                             D3DPOOL_DEFAULT, &texture_, nullptr)) || !texture_)
            {
                status_ = "D3D9 could not create the world-screen texture.";
                return false;
            }
            textureDevice_ = device;
        }

        D3DLOCKED_RECT locked{};
        if (FAILED(texture_->LockRect(0, &locked, nullptr, D3DLOCK_DISCARD)))
            return false;
        const auto* source = frameScratch_.data();
        auto* destination = static_cast<uint8_t*>(locked.pBits);
        for (uint32_t row = 0; row < wxl_video_shared::kHeight; ++row)
            std::memcpy(destination + size_t(row) * locked.Pitch,
                        source + size_t(row) * wxl_video_shared::kStride,
                        wxl_video_shared::kStride);
        texture_->UnlockRect(0);
        lastFrameSequence_ = before;

        char message[128]{};
        std::snprintf(message, sizeof(message), "Live video frame %ld connected (640x360).", before);
        status_ = message;
        return true;
    }

    void VideoSurface::DrawWorldScreen(IDirect3DDevice9* device, void* sceneDepthRaw,
                                       bool beforeWorld)
    {
        if (!device || !placed_ || !visible_ || wxl::game::world::CurrentMapId() != mapId_)
            return;

        ++drawAttempts_;
        UploadLatestFrame(device);

        const float halfWidth = width_ * 0.5f;
        const float halfHeight = width_ * 9.0f / 32.0f;
        // A missing helper/video must still produce an unmistakable world-space test card.  The
        // previous nearly-black placeholder was effectively invisible in dark zones and, because it
        // also honored world depth, gave no way to distinguish placement from playback failures.
        const D3DCOLOR topLeft = texture_ ? 0xFFFFFFFFu : 0xFFFF2D55u;
        const D3DCOLOR topRight = texture_ ? 0xFFFFFFFFu : 0xFFFFD60Au;
        const D3DCOLOR bottomLeft = texture_ ? 0xFFFFFFFFu : 0xFF00E5FFu;
        const D3DCOLOR bottomRight = texture_ ? 0xFFFFFFFFu : 0xFF7C4DFFu;
        const Vertex vertices[4] = {
            { center_[0] - right_[0] * halfWidth, center_[1] - right_[1] * halfWidth,
              center_[2] + halfHeight, topLeft, 1.0f, 0.0f },
            { center_[0] + right_[0] * halfWidth, center_[1] + right_[1] * halfWidth,
              center_[2] + halfHeight, topRight, 0.0f, 0.0f },
            { center_[0] - right_[0] * halfWidth, center_[1] - right_[1] * halfWidth,
              center_[2] - halfHeight, bottomLeft, 1.0f, 1.0f },
            { center_[0] + right_[0] * halfWidth, center_[1] + right_[1] * halfWidth,
              center_[2] - halfHeight, bottomRight, 0.0f, 1.0f },
        };
        const Vertex border[5] = {
            { vertices[0].x, vertices[0].y, vertices[0].z, 0xFFFFFF00u, 0.0f, 0.0f },
            { vertices[1].x, vertices[1].y, vertices[1].z, 0xFFFFFF00u, 0.0f, 0.0f },
            { vertices[3].x, vertices[3].y, vertices[3].z, 0xFFFFFF00u, 0.0f, 0.0f },
            { vertices[2].x, vertices[2].y, vertices[2].z, 0xFFFFFF00u, 0.0f, 0.0f },
            { vertices[0].x, vertices[0].y, vertices[0].z, 0xFFFFFF00u, 0.0f, 0.0f },
        };

        IDirect3DStateBlock9* state = nullptr;
        const HRESULT stateResult = device->CreateStateBlock(D3DSBT_ALL, &state);
        lastStateResult_ = stateResult;
        if (FAILED(stateResult) || !state)
        {
            const DWORD now = GetTickCount();
            if (api_ && now - lastDrawFailureLog_ >= 1000)
            {
                api_->Log(WXL_LOG_ERROR, kTag, "world screen state capture failed hr=0x%08lX",
                          static_cast<unsigned long>(stateResult));
                lastDrawFailureLog_ = now;
            }
            return;
        }
        IDirect3DSurface9* oldDepth = nullptr;
        device->GetDepthStencilSurface(&oldDepth);
        auto* sceneDepth = static_cast<IDirect3DSurface9*>(sceneDepthRaw);
        if (sceneDepth) device->SetDepthStencilSurface(sceneDepth);

        // Build 12340 creates a pure D3D9 device, so GetTransform is unavailable.  Use WarcraftXL's
        // exact scene-matrix bank while the world-scene-end event still owns it.  Camera globals are
        // close enough to place the quad visually, but not guaranteed to use the identical projection
        // that populated this depth surface; that mismatch makes a live video texture appear in broken
        // patches even though the depth-free diagnostic card is complete.
        float eye[3]{};
        wxl::game::camera::GetPosition(eye);
        D3DMATRIX world{};
        world._11 = world._22 = world._33 = world._44 = 1.0f;
        world._41 = -eye[0];
        world._42 = -eye[1];
        world._43 = -eye[2];
        D3DMATRIX view{};
        D3DMATRIX projection{};
        if (!wxl::game::gfx::SceneMatrices(&view._11, &projection._11))
        {
            device->SetDepthStencilSurface(oldDepth);
            if (oldDepth) oldDepth->Release();
            state->Apply();
            state->Release();
            return;
        }
        const float* sceneView = &view._11;
        const float* sceneProjection = &projection._11;

        const float relativeCenter[4] = {
            center_[0] - eye[0], center_[1] - eye[1], center_[2] - eye[2], 1.0f
        };
        float viewCenter[4]{};
        float clipCenter[4]{};
        for (int column = 0; column < 4; ++column)
        {
            for (int row = 0; row < 4; ++row)
                viewCenter[column] += relativeCenter[row] * sceneView[row * 4 + column];
            for (int row = 0; row < 4; ++row)
                clipCenter[column] += viewCenter[row] * sceneProjection[row * 4 + column];
        }
        projectedClipW_ = clipCenter[3];
        projectedCenterValid_ = std::isfinite(projectedClipW_) &&
                                std::abs(projectedClipW_) > 0.0001f;
        if (projectedCenterValid_)
        {
            projectedCenter_[0] = clipCenter[0] / projectedClipW_;
            projectedCenter_[1] = clipCenter[1] / projectedClipW_;
            projectedCenter_[2] = clipCenter[2] / projectedClipW_;
            projectedCenterValid_ = Finite3(projectedCenter_);
        }

        device->SetTransform(D3DTS_WORLD, &world);
        device->SetTransform(D3DTS_VIEW, &view);
        device->SetTransform(D3DTS_PROJECTION, &projection);
        device->SetVertexShader(nullptr);
        device->SetPixelShader(nullptr);
        device->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1);
        device->SetTexture(0, texture_);
        if (texture_)
        {
            device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
            device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
            device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        }
        else
        {
            device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
            device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        }
        device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        device->SetRenderState(D3DRS_CLIPPING, TRUE);
        device->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0Fu);
        device->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
        device->SetRenderState(D3DRS_ZWRITEENABLE, beforeWorld ? TRUE : FALSE);
        device->SetRenderState(D3DRS_ZENABLE,
                               sceneDepth && beforeWorld ? D3DZB_TRUE : D3DZB_FALSE);
        // Seed the cleared depth buffer unconditionally; world geometry rendered afterward uses its
        // normal depth test and can overwrite the screen wherever it is closer to the camera.
        device->SetRenderState(D3DRS_ZFUNC, D3DCMP_ALWAYS);
        DWORD depthBiasBits = 0;
        std::memcpy(&depthBiasBits, &depthBias_, sizeof(depthBiasBits));
        device->SetRenderState(D3DRS_DEPTHBIAS, depthBiasBits);

        const HRESULT drawResult = device->DrawPrimitiveUP(
            D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(Vertex));

        HRESULT borderResult = S_OK;
        if (outline_)
        {
            device->SetTexture(0, nullptr);
            device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
            device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
            borderResult = device->DrawPrimitiveUP(
                D3DPT_LINESTRIP, 4, border, sizeof(Vertex));
        }
        lastDrawResult_ = FAILED(drawResult) ? drawResult : borderResult;
        if (SUCCEEDED(drawResult) && SUCCEEDED(borderResult))
        {
            ++drawSuccesses_;
        }
        else
        {
            const DWORD now = GetTickCount();
            if (api_ && now - lastDrawFailureLog_ >= 1000)
            {
                api_->Log(WXL_LOG_ERROR, kTag, "world screen draw failed fill=0x%08lX border=0x%08lX",
                          static_cast<unsigned long>(drawResult),
                          static_cast<unsigned long>(borderResult));
                lastDrawFailureLog_ = now;
            }
        }

        device->SetDepthStencilSurface(oldDepth);
        if (oldDepth) oldDepth->Release();
        state->Apply();
        state->Release();
    }

    bool VideoSurface::GetPinnedRect(IDirect3DDevice9* device, float& left, float& top,
                                     float& right, float& bottom) const
    {
        if (!device) return false;
        D3DVIEWPORT9 viewport{};
        if (FAILED(device->GetViewport(&viewport)) || viewport.Width == 0 || viewport.Height == 0)
            return false;

        const float viewportWidth = static_cast<float>(viewport.Width);
        const float viewportHeight = static_cast<float>(viewport.Height);
        float leftFraction = pinnedLeft_;
        float topFraction = pinnedTop_;
        float widthFraction = pinnedWidth_;
        wxl_video_shared::ClampPinnedLayout(leftFraction, topFraction, widthFraction);
        const float width = viewportWidth * widthFraction;
        const float height = width * 9.0f / 16.0f;
        left = static_cast<float>(viewport.X) + leftFraction * viewportWidth - 0.5f;
        top = static_cast<float>(viewport.Y) + topFraction * viewportHeight - 0.5f;
        right = left + width;
        bottom = top + height;
        return true;
    }

    void VideoSurface::DrawPinnedScreen(IDirect3DDevice9* device)
    {
        if (!device || !visible_) return;

        UploadLatestFrame(device);
        float left = 0.0f;
        float top = 0.0f;
        float right = 0.0f;
        float bottom = 0.0f;
        if (!GetPinnedRect(device, left, top, right, bottom)) return;

        struct ScreenVertex
        {
            float x, y, z, rhw;
            D3DCOLOR color;
            float u, v;
        };
        const D3DCOLOR topLeft = texture_ ? 0xFFFFFFFFu : 0xFFFF2D55u;
        const D3DCOLOR topRight = texture_ ? 0xFFFFFFFFu : 0xFFFFD60Au;
        const D3DCOLOR bottomLeft = texture_ ? 0xFFFFFFFFu : 0xFF00E5FFu;
        const D3DCOLOR bottomRight = texture_ ? 0xFFFFFFFFu : 0xFF7C4DFFu;
        const ScreenVertex vertices[4] = {
            {left, top, 0.0f, 1.0f, topLeft, 0.0f, 0.0f},
            {right, top, 0.0f, 1.0f, topRight, 1.0f, 0.0f},
            {left, bottom, 0.0f, 1.0f, bottomLeft, 0.0f, 1.0f},
            {right, bottom, 0.0f, 1.0f, bottomRight, 1.0f, 1.0f},
        };
        const ScreenVertex border[5] = {
            {vertices[0].x, vertices[0].y, vertices[0].z, vertices[0].rhw, 0xFFFFFF00u, 0.0f, 0.0f},
            {vertices[1].x, vertices[1].y, vertices[1].z, vertices[1].rhw, 0xFFFFFF00u, 0.0f, 0.0f},
            {vertices[3].x, vertices[3].y, vertices[3].z, vertices[3].rhw, 0xFFFFFF00u, 0.0f, 0.0f},
            {vertices[2].x, vertices[2].y, vertices[2].z, vertices[2].rhw, 0xFFFFFF00u, 0.0f, 0.0f},
            {vertices[0].x, vertices[0].y, vertices[0].z, vertices[0].rhw, 0xFFFFFF00u, 0.0f, 0.0f},
        };

        IDirect3DStateBlock9* state = nullptr;
        const HRESULT stateResult = device->CreateStateBlock(D3DSBT_ALL, &state);
        lastStateResult_ = stateResult;
        if (FAILED(stateResult) || !state) return;

        device->SetVertexShader(nullptr);
        device->SetPixelShader(nullptr);
        device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
        device->SetTexture(0, texture_);
        if (texture_)
        {
            device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
            device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
            device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        }
        else
        {
            device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
            device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        }
        device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_ZENABLE, FALSE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        device->SetRenderState(D3DRS_CLIPPING, TRUE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0Fu);

        const HRESULT drawResult = device->DrawPrimitiveUP(
            D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(ScreenVertex));
        HRESULT borderResult = S_OK;
        if (outline_)
        {
            device->SetTexture(0, nullptr);
            device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
            device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
            borderResult = device->DrawPrimitiveUP(
                D3DPT_LINESTRIP, 4, border, sizeof(ScreenVertex));
        }
        lastDrawResult_ = FAILED(drawResult) ? drawResult : borderResult;
        if (SUCCEEDED(drawResult) && SUCCEEDED(borderResult)) ++drawSuccesses_;
        state->Apply();
        state->Release();
    }

    void VideoSurface::OnWorldSceneEnd(const wxl::events::WorldSceneEndArgs& args)
    {
        ++worldSceneCalls_;
        if (!loggedFirstWorldScene_ && api_)
        {
            api_->Log(WXL_LOG_INFO, kTag,
                      "first world scene device=%p depth=%p currentMap=%d placed=%d savedMap=%d",
                      args.device, args.sceneDepth, wxl::game::world::CurrentMapId(),
                      placed_ ? 1 : 0, mapId_);
            loggedFirstWorldScene_ = true;
        }
        if (pinned_)
            DrawPinnedScreen(static_cast<IDirect3DDevice9*>(args.device));
        else if (!depthTest_)
            DrawWorldScreen(static_cast<IDirect3DDevice9*>(args.device), args.sceneDepth, false);
    }

    void VideoSurface::OnUpdate(const wxl::events::UpdateArgs& args)
    {
        if (args.timeMs - lastVolumeUpdate_ < 200) return;
        lastVolumeUpdate_ = args.timeMs;

        int targetVolume = masterVolume_;
        if (spatialAudio_ && placed_)
        {
            namespace world = wxl::game::world;
            if (world::CurrentMapId() != mapId_)
            {
                targetVolume = 0;
            }
            else
            {
                const unsigned long long guid = world::ActivePlayerGuid();
                void* player = guid ? world::ResolveObject(guid, world::kTypeMaskPlayer) : nullptr;
                float position[3]{};
                if (!player)
                {
                    targetVolume = 0;
                }
                else
                {
                    world::UnitPosition(player, position);
                    const float dx = position[0] - center_[0];
                    const float dy = position[1] - center_[1];
                    const float dz = position[2] - center_[2];
                    const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                    if (!std::isfinite(distance) || distance >= silentDistance_)
                        targetVolume = 0;
                    else if (distance > fullVolumeDistance_)
                    {
                        const float range = std::max(1.0f, silentDistance_ - fullVolumeDistance_);
                        const float gain = 1.0f - (distance - fullVolumeDistance_) / range;
                        targetVolume = static_cast<int>(std::lround(masterVolume_ * gain));
                    }
                }
            }
        }
        SendVolume(targetVolume);
    }

    void VideoSurface::OnDeviceLost(const wxl::events::DeviceResetArgs&)
    {
        ReleaseTexture();
    }

    void VideoSurface::OnDeviceReset(const wxl::events::DeviceResetArgs&)
    {
        ReleaseTexture();
    }

    void VideoSurface::OnWorldLeave(const wxl::events::WorldLeaveArgs&)
    {
        ReleaseTexture();
    }

    void VideoSurface::DrawPlexLogin()
    {
        if (api_->UiButton("Open Plex sign-in"))
            SendUiCommand(wxl_video_shared::UiCommand::StartLogin);
    }

    void VideoSurface::DrawPlexServers()
    {
        api_->UiText("Choose a Plex server.");
        const uint32_t count = static_cast<uint32_t>(std::min(
            static_cast<size_t>(uiState_.serverCount), wxl_video_shared::kUiMaxServers));
        for (uint32_t index = 0; index < count; ++index)
        {
            char label[192]{};
            std::snprintf(label, sizeof(label), "%u. %s", index + 1,
                          uiState_.servers[index].name[0]
                              ? uiState_.servers[index].name : "Plex server");
            if (api_->UiButton(label))
                SendUiCommand(wxl_video_shared::UiCommand::SelectServer,
                              static_cast<LONG>(index));
            api_->UiSameLine();
            api_->UiText(uiState_.servers[index].uri[0]
                             ? uiState_.servers[index].uri : "address unavailable");
        }
    }

    void VideoSurface::DrawPlexHome()
    {
        if (api_->UiButton("Choose another Plex server"))
            SendUiCommand(wxl_video_shared::UiCommand::SetView, -1,
                          static_cast<LONG>(wxl_video_shared::UiView::Servers));
        api_->UiSameLine();
        if (api_->UiButton("Search Plex"))
            SendUiCommand(wxl_video_shared::UiCommand::SetView, -1,
                          static_cast<LONG>(wxl_video_shared::UiView::Search));
        api_->UiSameLine();
        if (api_->UiButton("Sign out of Plex"))
            SendUiCommand(wxl_video_shared::UiCommand::SignOut);

        if (!uiState_.watchlistLoaded)
            api_->UiText("Loading Plex watchlist...");
        else
        {
            char watchlist[96]{};
            std::snprintf(watchlist, sizeof(watchlist), "Watchlist (%u)",
                          uiState_.watchlistCount);
            if (api_->UiButton(watchlist))
                SendUiCommand(wxl_video_shared::UiCommand::OpenWatchlist);
            if (uiState_.watchlistError[0]) api_->UiText(uiState_.watchlistError);
        }

        api_->UiText("Plex libraries:");
        const uint32_t count = static_cast<uint32_t>(std::min(
            static_cast<size_t>(uiState_.sectionCount), wxl_video_shared::kUiMaxSections));
        for (uint32_t index = 0; index < count; ++index)
        {
            char label[224]{};
            std::snprintf(label, sizeof(label), "%u. %s (%s)", index + 1,
                          uiState_.sections[index].title[0]
                              ? uiState_.sections[index].title : "Plex library",
                          uiState_.sections[index].type[0]
                              ? uiState_.sections[index].type : "unknown");
            if (api_->UiButton(label))
                SendUiCommand(wxl_video_shared::UiCommand::SelectSection,
                              static_cast<LONG>(index));
        }
    }

    void VideoSurface::DrawPlexLibrary()
    {
        if (uiState_.browseDepth > 0)
        {
            char back[192]{};
            std::snprintf(back, sizeof(back), "Back from %s",
                          uiState_.browseTitle[0] ? uiState_.browseTitle : "child items");
            if (api_->UiButton(back))
                SendUiCommand(wxl_video_shared::UiCommand::NavigateBack);
        }
        else if (api_->UiButton("Back to Plex libraries"))
            SendUiCommand(wxl_video_shared::UiCommand::SetView, -1,
                          static_cast<LONG>(wxl_video_shared::UiView::Home));
        if (uiState_.browseTitle[0]) api_->UiText(uiState_.browseTitle);
        api_->UiSeparator();

        const uint32_t count = static_cast<uint32_t>(std::min(
            static_cast<size_t>(uiState_.itemCount), wxl_video_shared::kUiMaxItems));
        for (uint32_t index = 0; index < count; ++index)
        {
            const auto& item = uiState_.items[index];
            char label[256]{};
            if (item.grandparentTitle[0])
                std::snprintf(label, sizeof(label), "%u. %s - %s", index + 1,
                              item.grandparentTitle, item.title[0] ? item.title : "Plex item");
            else
                std::snprintf(label, sizeof(label), "%u. %s", index + 1,
                              item.title[0] ? item.title : "Plex item");
            if (api_->UiButton(label))
                SendUiCommand(wxl_video_shared::UiCommand::SelectItem,
                              static_cast<LONG>(index));
        }
        if (count == 0) api_->UiText("No Plex items found.");

        const bool hasPrevious = uiState_.pageOffset > 0;
        const bool hasNext = uiState_.pageSize > 0 &&
                             uiState_.pageOffset < uiState_.pageTotalSize &&
                             uiState_.pageSize <= uiState_.pageTotalSize - uiState_.pageOffset;
        if (hasPrevious && api_->UiButton("Previous page"))
            SendUiCommand(wxl_video_shared::UiCommand::PreviousPage);
        if (hasPrevious && hasNext) api_->UiSameLine();
        if (hasNext && api_->UiButton("Next page"))
            SendUiCommand(wxl_video_shared::UiCommand::NextPage);
        if (count > 0 && uiState_.pageTotalSize > 0)
        {
            char page[96]{};
            const uint32_t first = uiState_.pageOffset + 1;
            const uint32_t last = std::min(
                uiState_.pageOffset + count, uiState_.pageTotalSize);
            std::snprintf(page, sizeof(page), "Showing %u-%u of %u",
                          first, last, uiState_.pageTotalSize);
            api_->UiText(page);
        }
    }

    void VideoSurface::DrawPlexSearch()
    {
        api_->UiInputText("Plex search", search_, sizeof(search_));
        if (api_->UiButton("Search Plex now"))
            SendUiCommand(wxl_video_shared::UiCommand::Search, -1, 0, search_);
        api_->UiSameLine();
        if (api_->UiButton("Back to Plex libraries"))
            SendUiCommand(wxl_video_shared::UiCommand::SetView, -1,
                          static_cast<LONG>(wxl_video_shared::UiView::Home));
    }

    void VideoSurface::DrawPlexDetails()
    {
        if (api_->UiButton("Back to Plex items"))
            SendUiCommand(wxl_video_shared::UiCommand::SetView, -1,
                          static_cast<LONG>(wxl_video_shared::UiView::Library));
        api_->UiText(uiState_.item.title[0] ? uiState_.item.title : "Plex item");
        api_->UiText(uiState_.item.type[0] ? uiState_.item.type : "Unknown type");
        if (uiState_.item.grandparentTitle[0])
            api_->UiText(uiState_.item.grandparentTitle);
        if (uiState_.item.viewOffsetMs > 0)
        {
            char resume[96]{};
            std::snprintf(resume, sizeof(resume), "Resume position: %ld ms",
                          static_cast<long>(uiState_.item.viewOffsetMs));
            api_->UiText(resume);
        }
        if (api_->UiButton("Play selected Plex item"))
            SendUiCommand(wxl_video_shared::UiCommand::Play);

        const uint32_t count = static_cast<uint32_t>(std::min(
            static_cast<size_t>(uiState_.streamCount), wxl_video_shared::kUiMaxStreams));
        for (uint32_t index = 0; index < count; ++index)
        {
            const auto& stream = uiState_.streams[index];
            if (stream.streamType == 2)
            {
                char label[192]{};
                std::snprintf(label, sizeof(label), "%u. Audio: %s%s%s", index + 1, stream.title,
                              stream.title[0] && stream.language[0] ? " (" : "",
                              stream.title[0] && stream.language[0] ? stream.language : "");
                if (stream.title[0] && stream.language[0])
                    std::strncat(label, ")", sizeof(label) - std::strlen(label) - 1);
                if (api_->UiButton(label))
                    SendUiCommand(wxl_video_shared::UiCommand::SelectAudio,
                                  static_cast<LONG>(index));
            }
            else if (stream.streamType == 3)
            {
                char label[192]{};
                std::snprintf(label, sizeof(label), "%u. Subtitle: %s%s%s", index + 1,
                              stream.title,
                              stream.title[0] && stream.language[0] ? " (" : "",
                              stream.title[0] && stream.language[0] ? stream.language : "");
                if (stream.title[0] && stream.language[0])
                    std::strncat(label, ")", sizeof(label) - std::strlen(label) - 1);
                if (api_->UiButton(label))
                    SendUiCommand(wxl_video_shared::UiCommand::SelectSubtitle,
                                  static_cast<LONG>(index));
            }
        }
    }

    void VideoSurface::DrawPlexPlayer()
    {
        api_->UiText("Video continues on the placed world-space screen.");
        if (uiState_.playbackDurationMs > 0)
        {
            const float duration = uiState_.playbackDurationMs / 1000.0f;
            scrubPositionSeconds_ = std::clamp(
                scrubPositionSeconds_, 0.0f, duration);
            if (GetTickCount() - lastScrubAt_ > 250)
                scrubPositionSeconds_ = std::clamp(
                    uiState_.playbackPositionMs / 1000.0f, 0.0f, duration);
            if (api_->UiSliderFloat("Timeline", &scrubPositionSeconds_, 0.0f, duration))
            {
                lastScrubAt_ = GetTickCount();
                const double milliseconds = scrubPositionSeconds_ * 1000.0;
                const LONG positionMs = static_cast<LONG>(std::min(
                    milliseconds, static_cast<double>(0x7FFFFFFF)));
                SendUiCommand(wxl_video_shared::UiCommand::Seek, -1, positionMs);
            }
            char timeline[64]{};
            char current[32]{};
            char total[32]{};
            FormatTime(current, sizeof(current), uiState_.playbackPositionMs);
            FormatTime(total, sizeof(total), uiState_.playbackDurationMs);
            std::snprintf(timeline, sizeof(timeline), "%s / %s", current, total);
            api_->UiText(timeline);
        }
        else
        {
            api_->UiText("Timeline unavailable for this item.");
        }
        if (api_->UiButton("Play Plex playback"))
            SendUiCommand(wxl_video_shared::UiCommand::Play);
        api_->UiSameLine();
        if (api_->UiButton("Pause Plex playback"))
            SendUiCommand(wxl_video_shared::UiCommand::Pause);
        api_->UiSameLine();
        if (api_->UiButton("Stop Plex playback"))
            SendUiCommand(wxl_video_shared::UiCommand::Stop);
        api_->UiSameLine();
        if (api_->UiButton("Back to Plex item"))
            SendUiCommand(wxl_video_shared::UiCommand::SetView, -1,
                          static_cast<LONG>(wxl_video_shared::UiView::Details));
    }

    void VideoSurface::DrawPlexPanel()
    {
        if (!FindWindowW(kHostWindowClass, nullptr))
        {
            api_->UiText("Plex playback worker is not running.");
            if (api_->UiButton("Start Plex playback worker")) OpenHost();
            return;
        }
        if (!RefreshUiState())
        {
            api_->UiText("Plex player is starting.");
            return;
        }
        if (uiState_.status[0]) api_->UiText(uiState_.status);
        if (uiState_.error[0]) api_->UiText(uiState_.error);

        switch (uiState_.view)
        {
        case wxl_video_shared::UiView::Login: DrawPlexLogin(); break;
        case wxl_video_shared::UiView::Servers: DrawPlexServers(); break;
        case wxl_video_shared::UiView::Home: DrawPlexHome(); break;
        case wxl_video_shared::UiView::Library: DrawPlexLibrary(); break;
        case wxl_video_shared::UiView::Search: DrawPlexSearch(); break;
        case wxl_video_shared::UiView::Details: DrawPlexDetails(); break;
        case wxl_video_shared::UiView::Player: DrawPlexPlayer(); break;
        default: api_->UiText("Plex player returned an invalid view."); break;
        }
        if (uiState_.view == wxl_video_shared::UiView::Player &&
            !uiState_.item.ratingKey[0] && api_->UiButton("Back to Plex sign-in"))
            SendUiCommand(wxl_video_shared::UiCommand::SetView, -1,
                          static_cast<LONG>(wxl_video_shared::UiView::Login));
    }

    void VideoSurface::DrawPanel()
    {
        if (!api_) return;
        api_->UiText("Native Plex player with a physical world-space screen.");
        if (plexPanelOpen_)
        {
            if (api_->UiButton("Hide Plex player UI"))
                HideHost();
            else
            {
                DrawPlexPanel();
                api_->UiSeparator();
            }
        }
        else if (api_->UiButton("Open Plex player inside WoW"))
        {
            if (OpenHost()) plexPanelOpen_ = true;
        }

        if (!plexPanelOpen_)
        {
            if (api_->UiButton("Play"))
                SendCommand(static_cast<LONG>(wxl_video_shared::Command::Play));
            if (api_->UiButton("Pause"))
                SendCommand(static_cast<LONG>(wxl_video_shared::Command::Pause));
            if (api_->UiButton("Stop"))
                SendCommand(static_cast<LONG>(wxl_video_shared::Command::Stop));
        }
        api_->UiSeparator();

        if (api_->UiSliderInt("Master volume", &masterVolume_, 0, 100))
            SendVolume(masterVolume_);
        api_->UiCheckbox("Distance-based audio", &spatialAudio_);
        if (spatialAudio_)
        {
            api_->UiSliderFloat("Full volume distance (yards)", &fullVolumeDistance_, 1.0f, 30.0f);
            api_->UiSliderFloat("Silent distance (yards)", &silentDistance_, 10.0f, 120.0f);
            if (silentDistance_ < fullVolumeDistance_ + 1.0f)
                silentDistance_ = fullVolumeDistance_ + 1.0f;
        }
        char volumeStatus[96]{};
        std::snprintf(volumeStatus, sizeof(volumeStatus), "Current screen audio: %d%%", effectiveVolume_);
        api_->UiText(volumeStatus);
        if (api_->UiButton("Close background Plex player")) CloseHost();
        api_->UiSeparator();

        if (api_->UiCheckbox("Pin Plex screen to display", &pinned_))
        {
            if (placed_) SavePlacement();
            else status_ = "Pinned Plex screen enabled; place it later to save the setting.";
        }
        if (pinned_)
        {
            api_->UiText("Pinned Plex screen stays fixed while your character moves.");
            api_->UiText("Use these controls while the WoW overlay is open; drag when it is closed.");
            bool pinnedLayoutChanged = api_->UiSliderFloat(
                "Pinned size", &pinnedWidth_, 0.15f, 0.80f) != 0;
            pinnedLayoutChanged = api_->UiSliderFloat(
                "Pinned X (left to right)", &pinnedLeft_, 0.0f,
                std::max(0.0f, 1.0f - pinnedWidth_)) != 0 || pinnedLayoutChanged;
            pinnedLayoutChanged = api_->UiSliderFloat(
                "Pinned Y (top to bottom)", &pinnedTop_, 0.0f,
                std::max(0.0f, 1.0f - pinnedWidth_ * 9.0f / 16.0f)) != 0 || pinnedLayoutChanged;
            wxl_video_shared::ClampPinnedLayout(pinnedLeft_, pinnedTop_, pinnedWidth_);
            if (pinnedLayoutChanged) SavePlacement();
        }

        if (api_->UiSliderFloat("Screen width (yards)", &width_, 2.0f, 30.0f) && placed_)
            SavePlacement();
        api_->UiSliderFloat("Place distance (yards)", &placeDistance_, 4.0f, 30.0f);
        api_->UiSliderFloat("Bottom height (yards)", &bottomOffset_, -3.0f, 10.0f);
        if (api_->UiButton("Place / move screen in front of me")) PlaceInFront();

        if (placed_)
        {
            if (api_->UiCheckbox("Show physical screen", &visible_)) SavePlacement();
            if (api_->UiCheckbox("Characters and world appear in front", &depthTest_)) SavePlacement();
            if (api_->UiSliderFloat("Depth offset (more negative brings screen forward)",
                                    &depthBias_, -0.030f, 0.005f))
                SavePlacement();
            api_->UiCheckbox("Show yellow placement outline", &outline_);
            if (api_->UiButton("Rotate screen left 5 degrees")) Rotate(5.0f);
            if (api_->UiButton("Rotate screen right 5 degrees")) Rotate(-5.0f);
            if (api_->UiButton("Raise screen 0.5 yard")) { center_[2] += 0.5f; SavePlacement(); }
            if (api_->UiButton("Lower screen 0.5 yard")) { center_[2] -= 0.5f; SavePlacement(); }

            char placement[192]{};
            std::snprintf(placement, sizeof(placement),
                          "Saved screen: map %d at %.2f, %.2f, %.2f (%.2f x %.2f yards)",
                          mapId_, center_[0], center_[1], center_[2], width_, width_ * 9.0f / 16.0f);
            api_->UiText(placement);
        }
        else
        {
            api_->UiText("No physical screen has been placed yet.");
        }
        char diagnostics[256]{};
        std::snprintf(diagnostics, sizeof(diagnostics),
                      "World draw: scenes %llu | attempts %llu | success %llu | hr 0x%08lX | frame %ld | helper %s",
                      static_cast<unsigned long long>(worldSceneCalls_),
                      static_cast<unsigned long long>(drawAttempts_),
                      static_cast<unsigned long long>(drawSuccesses_),
                      static_cast<unsigned long>(lastDrawResult_), lastFrameSequence_,
                      FindWindowW(kHostWindowClass, nullptr) ? "open" : "closed");
        api_->UiText(diagnostics);
        char projection[192]{};
        if (projectedCenterValid_)
            std::snprintf(projection, sizeof(projection),
                          "Projected center NDC: %.3f, %.3f, %.3f | clip W %.3f (visible XY is -1 to +1)",
                          projectedCenter_[0], projectedCenter_[1], projectedCenter_[2], projectedClipW_);
        else
            std::snprintf(projection, sizeof(projection),
                          "Projected center unavailable | clip W %.3f", projectedClipW_);
        api_->UiText(projection);
        if (!texture_)
            api_->UiText("No video frame yet: a bright four-color test card should still be visible in the world.");
        api_->UiText(status_.c_str());
    }
}
