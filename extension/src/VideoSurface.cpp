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
        constexpr UINT kShowHostMessage = WM_APP + 0x45;
        constexpr UINT kHideHostMessage = WM_APP + 0x46;
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
        if (self.preWorldDrawn_ || !self.initialized_ || !self.depthTest_ || !self.visible_ ||
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

    bool VideoSurface::OpenHost(bool showWindow)
    {
        if (HWND host = FindWindowW(kHostWindowClass, nullptr))
        {
            PostMessageW(host, showWindow ? kShowHostMessage : kHideHostMessage, 0, 0);
            return true;
        }

        const std::wstring directory = ExtensionDirectory();
        const std::wstring hostPath = directory.empty()
            ? std::wstring{} : (std::filesystem::path(directory) / L"wxl-video-host.exe").wstring();
        if (hostPath.empty() || GetFileAttributesW(hostPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            status_ = "Cinema helper is missing from the extension folder.";
            if (api_) api_->Log(WXL_LOG_ERROR, kTag, "%s", status_.c_str());
            return false;
        }

        std::wstring commandLine = L"\"" + hostPath + L"\" --parent-pid " +
                                   std::to_wstring(GetCurrentProcessId());
        if (!showWindow) commandLine += L" --background";
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
        status_ = showWindow
            ? "Cinema troubleshooting window opened."
            : "Background cinema player started.";
        return true;
    }

    void VideoSurface::HideHost()
    {
        if (HWND host = FindWindowW(kHostWindowClass, nullptr))
        {
            PostMessageW(host, kHideHostMessage, 0, 0);
            status_ = "Cinema player is running in the background.";
        }
    }

    void VideoSurface::CloseHost()
    {
        if (HWND host = FindWindowW(kHostWindowClass, nullptr))
            PostMessageW(host, WM_CLOSE, 0, 0);
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
        const bool showWindow = command == static_cast<LONG>(wxl_video_shared::Command::Show);
        if (!ConnectControl() || !OpenHost(showWindow)) return false;
        auto* control = static_cast<wxl_video_shared::ControlBlock*>(controlView_);
        InterlockedExchange(&control->command, command);
        MemoryBarrier();
        LONG sequence = control->commandSequence + 1;
        if (sequence <= 0) sequence = 1;
        InterlockedExchange(&control->commandSequence, sequence);
        status_ = "Command sent to the background cinema player.";
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

        // Use the engine's actual view direction.  Deriving it from camera -> player works only while
        // the stock third-person camera sits behind the character; orbit/front/free-camera views can
        // otherwise place the screen behind the visible scene.
        const float* view = camera::GetView();
        float forwardX = view ? view[2] : playerPosition[0] - cameraPosition[0];
        float forwardY = view ? view[6] : playerPosition[1] - cameraPosition[1];
        float length = std::sqrt(forwardX * forwardX + forwardY * forwardY);
        if (!(length > 0.001f) || !std::isfinite(length))
        {
            forwardX = playerPosition[0] - cameraPosition[0];
            forwardY = playerPosition[1] - cameraPosition[1];
            length = std::sqrt(forwardX * forwardX + forwardY * forwardY);
        }
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
               << width_ << '\t' << visible_ << '\t' << depthTest_ << '\t' << depthBias_ << '\n';
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
        // Never let terrain hide the diagnostic card.  As soon as live video is connected the user's
        // occlusion preference applies normally.
        device->SetRenderState(D3DRS_ZENABLE,
                               sceneDepth && (beforeWorld || (texture_ && depthTest_))
                                   ? D3DZB_TRUE : D3DZB_FALSE);
        // Build 12340's projection maps near-to-far depth in increasing order.  The live test
        // positions proved the character was between the camera and this quad while GREATER_EQUAL
        // still painted over it: the comparison was reversed.  Match the world's LESS_EQUAL test so
        // a character, doodad, or terrain pixel with smaller depth remains in front of the screen.
        device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
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
        // With physical occlusion enabled, the scene-clear hook already placed the screen into the
        // freshly cleared colour/depth targets. WoW's own world draw then naturally paints nearer
        // characters and props over it. The end-of-scene path is retained for the intentional
        // through-world mode.
        if (!depthTest_)
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

    void VideoSurface::DrawPanel()
    {
        if (!api_) return;
        api_->UiText("Native Plex player with a physical world-space screen.");
        if (api_->UiButton("Open Plex player"))
            SendCommand(static_cast<LONG>(wxl_video_shared::Command::Show));
        if (api_->UiButton("Hide Plex player"))
            SendCommand(static_cast<LONG>(wxl_video_shared::Command::Hide));
        if (api_->UiButton("Play"))
            SendCommand(static_cast<LONG>(wxl_video_shared::Command::Play));
        if (api_->UiButton("Pause"))
            SendCommand(static_cast<LONG>(wxl_video_shared::Command::Pause));
        if (api_->UiButton("Stop"))
            SendCommand(static_cast<LONG>(wxl_video_shared::Command::Stop));
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
        if (api_->UiButton("Show Plex helper window")) OpenHost(true);
        if (api_->UiButton("Hide Plex helper window")) HideHost();
        if (api_->UiButton("Close Plex player")) CloseHost();
        api_->UiSeparator();

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
