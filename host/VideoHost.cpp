// Native Plex helper used by wxl-azeroth-plex.
// Copyright (C) 2026 WarcraftXL contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "FramePublisher.hpp"
#include "LiveAudio.hpp"
#include "LiveCodec.hpp"
#include "MpvPlayer.hpp"
#include "NativeUi.hpp"
#include "PartyClient.hpp"
#include "PlexClient.hpp"
#include "SessionStore.hpp"
#include "SyncController.hpp"
#include "VideoShared.hpp"
#include "backends/imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <shlobj.h>
#include <wrl.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
    constexpr wchar_t kWindowClass[] = L"WXLNativePlexHost";
    constexpr wchar_t kWindowTitle[] = L"AzerothPlex";
    constexpr UINT_PTR kTickTimer = 1;
    constexpr UINT kTickIntervalMs = 16;

    HWND g_window = nullptr;
    HANDLE g_controlMapping = nullptr;
    void* g_controlView = nullptr;
    HANDLE g_uiMapping = nullptr;
    void* g_uiView = nullptr;
    HANDLE g_parentProcess = nullptr;
    LONG g_lastCommandSequence = 0;
    LONG g_lastVolumeSequence = 0;
    LONG g_lastUiCommandSequence = 0;

    ComPtr<ID3D11Device> g_device;
    ComPtr<ID3D11DeviceContext> g_context;
    ComPtr<IDXGISwapChain> g_swapChain;
    ComPtr<ID3D11RenderTargetView> g_renderTarget;
    std::vector<uint8_t> g_frame(wxl_video_shared::kFrameBytes);
    wxl_frame::FramePublisher g_frames;
    wxl_mpv::MpvPlayer g_player;
    std::unique_ptr<wxl_plex::PlexClient> g_plex;
    std::unique_ptr<wxl_party::SessionStore> g_partyStore;
    std::unique_ptr<wxl_party::PartyClient> g_party;
    wxl_party::SyncController g_sync;
    wxl_ui::NativeUi g_ui;
    wxl_live::LiveEncoder g_liveEncoder;
    wxl_live::LiveDecoder g_liveDecoder;
    wxl_live::LiveAudioCapture g_liveAudioCapture;
    wxl_live::LiveAudioEncoder g_liveAudioEncoder;
    wxl_live::LiveAudioDecoder g_liveAudioDecoder;
    wxl_live::LiveAudioOutput g_liveAudioOutput;
    uint64_t g_liveFramesSent = 0;
    uint64_t g_liveFramesReceived = 0;
    uint64_t g_liveAudioFramesSent = 0;
    uint64_t g_liveAudioFramesReceived = 0;
    uint32_t g_liveFpsPermille = 0;
    uint64_t g_liveFpsLastCount = 0;
    ULONGLONG g_liveFpsAt = 0;
    ULONGLONG g_liveDecoderRetryAt = 0;
    ULONGLONG g_lastLiveEncodeAt = 0;
    ULONGLONG g_liveAudioRetryAt = 0;
    uint32_t g_liveAudioSequence = 0;
    bool g_liveAudioActive = false;

    constexpr uint32_t kLiveEncodeFps = 30;
    constexpr uint32_t kLiveEncodeBitrate = 1200 * 1000;
    constexpr uint32_t kLiveKeyframeInterval = kLiveEncodeFps * 2;

    DWORD ParseParentPid(const wchar_t* commandLine)
    {
        if (!commandLine) return 0;
        constexpr wchar_t option[] = L"--parent-pid";
        const wchar_t* cursor = std::wcsstr(commandLine, option);
        if (!cursor) return 0;
        cursor += std::size(option) - 1;
        while (*cursor == L' ' || *cursor == L'\t') ++cursor;
        wchar_t* end = nullptr;
        const unsigned long value = std::wcstoul(cursor, &end, 10);
        return end != cursor && value > 0 ? static_cast<DWORD>(value) : 0;
    }

    DWORD WINAPI WatchParentProcess(void*)
    {
        if (g_parentProcess && WaitForSingleObject(g_parentProcess, INFINITE) == WAIT_OBJECT_0 && g_window)
            PostMessageW(g_window, WM_CLOSE, 0, 0);
        return 0;
    }

    wxl_video_shared::ControlBlock* SharedControl()
    {
        return static_cast<wxl_video_shared::ControlBlock*>(g_controlView);
    }

    bool InitializeControl()
    {
        g_controlMapping = CreateFileMappingW(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
            static_cast<DWORD>(sizeof(wxl_video_shared::ControlBlock)),
            wxl_video_shared::kControlMappingName);
        if (!g_controlMapping) return false;
        const bool created = GetLastError() != ERROR_ALREADY_EXISTS;
        g_controlView = MapViewOfFile(g_controlMapping, FILE_MAP_ALL_ACCESS, 0, 0,
                                      sizeof(wxl_video_shared::ControlBlock));
        if (!g_controlView) return false;

        auto* control = SharedControl();
        if (created)
        {
            std::memset(control, 0, sizeof(*control));
            control->magic = wxl_video_shared::kControlMagic;
            control->version = wxl_video_shared::kControlVersion;
            control->structBytes = sizeof(*control);
            control->volumePercent = 80;
        }
        return control->magic == wxl_video_shared::kControlMagic &&
               control->version == wxl_video_shared::kControlVersion &&
               control->structBytes == sizeof(*control);
    }

    void ShutdownControl()
    {
        if (g_controlView)
        {
            UnmapViewOfFile(g_controlView);
            g_controlView = nullptr;
        }
        if (g_controlMapping)
        {
            CloseHandle(g_controlMapping);
            g_controlMapping = nullptr;
        }
    }

    wxl_video_shared::UiBridge* SharedUi()
    {
        return static_cast<wxl_video_shared::UiBridge*>(g_uiView);
    }

    bool InitializeUiBridge()
    {
        g_uiMapping = CreateFileMappingW(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
            static_cast<DWORD>(sizeof(wxl_video_shared::UiBridge)),
            wxl_video_shared::kUiMappingName);
        if (!g_uiMapping) return false;
        const bool created = GetLastError() != ERROR_ALREADY_EXISTS;
        g_uiView = MapViewOfFile(g_uiMapping, FILE_MAP_ALL_ACCESS, 0, 0,
                                 sizeof(wxl_video_shared::UiBridge));
        if (!g_uiView) return false;

        auto* bridge = SharedUi();
        if (!created && (bridge->magic != wxl_video_shared::kUiMagic ||
                         bridge->version != wxl_video_shared::kUiVersion ||
                         bridge->structBytes != sizeof(*bridge)))
            return false;

        if (created)
        {
            std::memset(bridge, 0, sizeof(*bridge));
            bridge->magic = wxl_video_shared::kUiMagic;
            bridge->version = wxl_video_shared::kUiVersion;
            bridge->structBytes = sizeof(*bridge);
            bridge->slots[0].structBytes = sizeof(wxl_video_shared::UiSnapshot);
            bridge->slots[1].structBytes = sizeof(wxl_video_shared::UiSnapshot);
        }
        return bridge->magic == wxl_video_shared::kUiMagic &&
               bridge->version == wxl_video_shared::kUiVersion &&
               bridge->structBytes == sizeof(*bridge);
    }

    void ShutdownUiBridge()
    {
        if (g_uiView)
        {
            UnmapViewOfFile(g_uiView);
            g_uiView = nullptr;
        }
        if (g_uiMapping)
        {
            CloseHandle(g_uiMapping);
            g_uiMapping = nullptr;
        }
    }

    void ProcessUiCommand()
    {
        auto* bridge = SharedUi();
        if (!bridge) return;

        const LONG sequence = bridge->commandSequence;
        if (sequence <= 0 || sequence == g_lastUiCommandSequence) return;
        MemoryBarrier();
        wxl_video_shared::UiCommandPacket command{};
        std::memcpy(&command, &bridge->command, sizeof(command));
        MemoryBarrier();
        if (bridge->commandSequence != sequence) return;
        g_ui.HandleUiCommand(command);
        g_lastUiCommandSequence = sequence;
    }

    void PublishUiState()
    {
        auto* bridge = SharedUi();
        if (!bridge) return;

        const LONG active = bridge->activeIndex;
        const LONG next = active == 0 ? 1 : 0;
        std::memset(&bridge->slots[next], 0, sizeof(bridge->slots[next]));
        g_ui.PublishUiState(bridge->slots[next]);
        LONG sequence = bridge->sequence + 1;
        if (sequence <= 0) sequence = 1;
        MemoryBarrier();
        InterlockedExchange(&bridge->slotSequence[next], sequence);
        InterlockedExchange(&bridge->activeIndex, next);
        InterlockedExchange(&bridge->sequence, sequence);
    }

    void ProcessControls()
    {
        auto* control = SharedControl();
        if (!control) return;

        const LONG commandSequence = control->commandSequence;
        if (commandSequence > 0 && commandSequence != g_lastCommandSequence)
        {
            MemoryBarrier();
            const LONG command = control->command;
            MemoryBarrier();
            if (control->commandSequence == commandSequence)
            {
                switch (static_cast<wxl_video_shared::Command>(command))
                {
                case wxl_video_shared::Command::Show:
                case wxl_video_shared::Command::Hide:
                    break;
                case wxl_video_shared::Command::Play: g_player.Play(); break;
                case wxl_video_shared::Command::Pause: g_player.Pause(); break;
                case wxl_video_shared::Command::Stop: g_player.Stop(); break;
                default: break;
                }
                g_lastCommandSequence = commandSequence;
            }
        }

        const LONG volumeSequence = control->volumeSequence;
        if (volumeSequence > 0 && volumeSequence != g_lastVolumeSequence)
        {
            MemoryBarrier();
            const LONG rawVolume = control->volumePercent;
            const LONG volume = std::clamp(rawVolume, static_cast<LONG>(0), static_cast<LONG>(100));
            MemoryBarrier();
            if (control->volumeSequence == volumeSequence)
            {
                g_player.SetVolume(static_cast<int>(volume));
                g_liveAudioOutput.SetVolumePercent(static_cast<int>(volume));
                g_lastVolumeSequence = volumeSequence;
            }
        }
    }

    bool CreateRenderTarget()
    {
        ComPtr<ID3D11Texture2D> buffer;
        return SUCCEEDED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(&buffer))) &&
               SUCCEEDED(g_device->CreateRenderTargetView(buffer.Get(), nullptr, &g_renderTarget));
    }

    bool InitializeGraphics(HWND window)
    {
        DXGI_SWAP_CHAIN_DESC description{};
        description.BufferCount = 2;
        description.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.OutputWindow = window;
        description.SampleDesc.Count = 1;
        description.Windowed = TRUE;
        description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        UINT flags = 0;
#ifdef _DEBUG
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        D3D_FEATURE_LEVEL level{};
        const D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_0,
        };
        return SUCCEEDED(D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
            static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &description,
            &g_swapChain, &g_device, &level, &g_context)) && CreateRenderTarget();
    }

    void ResizeGraphics(UINT width, UINT height)
    {
        if (!g_swapChain || width == 0 || height == 0) return;
        g_renderTarget.Reset();
        if (SUCCEEDED(g_swapChain->ResizeBuffers(0, width, height,
                                                  DXGI_FORMAT_B8G8R8A8_UNORM, 0)))
            CreateRenderTarget();
    }

    std::filesystem::path LocalDataDirectory()
    {
        wchar_t path[MAX_PATH]{};
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, path)))
            return {};
        return std::filesystem::path(path) / L"WarcraftXL" / L"plex-player";
    }

    void RenderFrame()
    {
        ProcessControls();
        std::vector<wxl_mpv::MpvEvent> events;
        g_player.PollEvents(events);
        g_ui.HandleMpvEvents(events);
        g_ui.PumpPlexEvents();
        g_ui.PumpPartyEvents();
        ProcessUiCommand();

        const bool broadcastingLive = g_ui.LiveBroadcasting();
        const bool watchingLive = g_ui.LiveWatching();

        if (watchingLive)
        {
            if (!g_liveDecoder.IsInitialized() && GetTickCount64() - g_liveDecoderRetryAt >= 1000)
            {
                g_liveDecoderRetryAt = GetTickCount64();
                g_liveDecoder.Initialize();
            }
            std::vector<uint8_t> packet;
            int drained = 0;
            while (drained < 8 && g_party->PopBinaryFrame(packet))
            {
                ++drained;
                wxl_live::FrameHeader header{};
                if (!wxl_live::ReadFrameHeader(packet, header)) continue;
                std::span<const uint8_t> payload(packet.data() + wxl_live::kFrameHeaderBytes,
                                                 packet.size() - wxl_live::kFrameHeaderBytes);
                if ((header.flags & wxl_live::kFrameFlagAudio) != 0)
                {
                    if (!g_liveAudioDecoder.IsInitialized())
                        g_liveAudioDecoder.Initialize(wxl_live::kLiveAudioSampleRate, wxl_live::kLiveAudioChannels);
                    if (!g_liveAudioOutput.IsRunning() && g_liveAudioDecoder.IsInitialized())
                        g_liveAudioOutput.Start(wxl_live::kLiveAudioSampleRate, wxl_live::kLiveAudioChannels);
                    std::vector<uint8_t> pcm;
                    if (g_liveAudioDecoder.IsInitialized() &&
                        g_liveAudioDecoder.Decode(payload, pcm) && !pcm.empty())
                    {
                        if (g_liveAudioOutput.IsRunning())
                            g_liveAudioOutput.Submit(pcm.data(), pcm.size());
                        ++g_liveAudioFramesReceived;
                        g_liveAudioActive = true;
                    }
                    continue;
                }
                if (!g_liveDecoder.IsInitialized()) continue;
                std::vector<uint8_t> frame;
                uint32_t width = 0;
                uint32_t height = 0;
                if (!g_liveDecoder.Decode(payload, header.sequence, frame, width, height))
                    continue;
                if (width != wxl_video_shared::kWidth || height != wxl_video_shared::kHeight)
                    continue;
                g_frames.Publish(frame, wxl_video_shared::kStride);
                g_ui.SetFrame(frame.data(), wxl_video_shared::kStride);
                ++g_liveFramesReceived;
            }
        }
        else
        {
            if (g_liveDecoder.IsInitialized() || g_liveAudioDecoder.IsInitialized() ||
                g_liveAudioOutput.IsRunning())
            {
                g_liveDecoder.Shutdown();
                g_liveAudioDecoder.Shutdown();
                g_liveAudioOutput.Stop();
                std::vector<uint8_t> stale;
                while (g_party->PopBinaryFrame(stale)) {}
            }
            g_liveFramesReceived = 0;
            g_liveAudioFramesReceived = 0;
            g_liveAudioActive = false;
        }

        g_player.Render();
        if (!watchingLive && g_player.CopyFrameTo(g_frame.data(), wxl_video_shared::kStride))
        {
            g_frames.Publish(g_frame, wxl_video_shared::kStride);
            g_ui.SetFrame(g_frame.data(), wxl_video_shared::kStride);

            if (broadcastingLive)
            {
                if (!g_liveEncoder.IsInitialized())
                {
                    g_liveEncoder.Initialize(wxl_video_shared::kWidth, wxl_video_shared::kHeight,
                                             kLiveEncodeFps, kLiveEncodeBitrate,
                                             kLiveKeyframeInterval);
                }
                const ULONGLONG now = GetTickCount64();
                if (g_liveEncoder.IsInitialized() && now - g_lastLiveEncodeAt >= 1000 / kLiveEncodeFps)
                {
                    g_lastLiveEncodeAt = now;
                    std::vector<uint8_t> accessUnit;
                    uint32_t sequence = 0;
                    bool keyframe = false;
                    if (g_liveEncoder.Encode(g_frame.data(), wxl_video_shared::kStride,
                                             accessUnit, sequence, keyframe) && !accessUnit.empty())
                    {
                        wxl_live::WriteFrameHeader(accessUnit, keyframe, sequence);
                        g_party->SendBinaryFrame(accessUnit);
                        ++g_liveFramesSent;
                    }
                }

                if (!g_liveAudioCapture.IsRunning() &&
                    GetTickCount64() - g_liveAudioRetryAt >= 2000)
                {
                    g_liveAudioRetryAt = GetTickCount64();
                    g_liveAudioCapture.Start(GetCurrentProcessId());
                    if (g_liveAudioCapture.IsRunning() && !g_liveAudioEncoder.IsInitialized())
                        g_liveAudioEncoder.Initialize(wxl_live::kLiveAudioSampleRate,
                                                      wxl_live::kLiveAudioChannels,
                                                      wxl_live::kLiveAudioBitrate);
                }
                g_liveAudioActive = g_liveAudioCapture.IsRunning() &&
                                    g_liveAudioEncoder.IsInitialized();
                if (g_liveAudioActive)
                {
                    std::vector<uint8_t> captured(
                        size_t(wxl_live::kLiveAudioChunkFrames) * wxl_live::kLiveAudioChannels * 2);
                    const size_t got = g_liveAudioCapture.Read(captured.data(), captured.size());
                    if (got > 0)
                    {
                        std::vector<uint8_t> encoded;
                        if (g_liveAudioEncoder.Encode(captured.data(),
                                                      static_cast<uint32_t>(got), encoded) &&
                            !encoded.empty())
                        {
                            wxl_live::WriteFrameHeader(encoded, false, ++g_liveAudioSequence, true);
                            g_party->SendBinaryFrame(encoded);
                            ++g_liveAudioFramesSent;
                        }
                    }
                }
            }
        }
        if (!broadcastingLive && g_liveEncoder.IsInitialized())
        {
            g_liveEncoder.Shutdown();
            g_liveFramesSent = 0;
            g_lastLiveEncodeAt = 0;
        }
        if (!broadcastingLive)
        {
            if (g_liveAudioCapture.IsRunning()) g_liveAudioCapture.Stop();
            if (g_liveAudioEncoder.IsInitialized()) g_liveAudioEncoder.Shutdown();
            g_liveAudioFramesSent = 0;
            g_liveAudioActive = false;
        }
        if (!broadcastingLive && !watchingLive)
            g_liveFramesSent = 0;

        const ULONGLONG statsNow = GetTickCount64();
        if (statsNow - g_liveFpsAt >= 1000)
        {
            const uint64_t frames = broadcastingLive ? g_liveFramesSent
                                   : watchingLive ? g_liveFramesReceived : 0;
            const uint64_t delta = frames >= g_liveFpsLastCount ? frames - g_liveFpsLastCount : 0;
            g_liveFpsPermille = static_cast<uint32_t>(std::min<uint64_t>(delta, 999) * 1000);
            g_liveFpsLastCount = frames;
            g_liveFpsAt = statsNow;
        }
        g_ui.SetLiveStats(static_cast<uint32_t>(std::min<uint64_t>(g_liveFramesSent, UINT32_MAX)),
                          static_cast<uint32_t>(std::min<uint64_t>(g_liveFramesReceived, UINT32_MAX)),
                          g_liveFpsPermille, g_liveAudioActive,
                          static_cast<uint32_t>(std::min<uint64_t>(g_liveAudioFramesReceived,
                                                                   UINT32_MAX)));

        PublishUiState();

        if (!g_ui.IsVisible() || !g_renderTarget) return;
        const float clear[4] = {0.025f, 0.025f, 0.035f, 1.0f};
        g_context->OMSetRenderTargets(1, g_renderTarget.GetAddressOf(), nullptr);
        g_context->ClearRenderTargetView(g_renderTarget.Get(), clear);
        g_ui.Draw();
        g_swapChain->Present(1, 0);
    }

    LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam))
            return 1;

        switch (message)
        {
        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED)
                ResizeGraphics(LOWORD(lParam), HIWORD(lParam));
            return 0;
        case WM_TIMER:
            if (wParam == kTickTimer) RenderFrame();
            return 0;
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            KillTimer(window, kTickTimer);
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window, message, wParam, lParam);
        }
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int)
{
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    wxl_live::StartupMediaFoundation();
    const DWORD parentPid = ParseParentPid(commandLine);
    if (parentPid) g_parentProcess = OpenProcess(SYNCHRONIZE, FALSE, parentPid);

    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.lpszClassName = kWindowClass;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        wxl_live::ShutdownMediaFoundation();
        CoUninitialize();
        return 2;
    }

    g_window = CreateWindowExW(0, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, CW_USEDEFAULT, 960, 640, nullptr, nullptr,
                               instance, nullptr);
    if (!g_window || !InitializeControl() || !InitializeUiBridge() ||
        !InitializeGraphics(g_window) || !g_frames.Open())
    {
        if (g_window) DestroyWindow(g_window);
        ShutdownUiBridge();
        ShutdownControl();
        wxl_live::ShutdownMediaFoundation();
        CoUninitialize();
        return 3;
    }

    const std::filesystem::path dataDirectory = LocalDataDirectory();
    g_plex = std::make_unique<wxl_plex::PlexClient>(dataDirectory);
    g_partyStore = std::make_unique<wxl_party::SessionStore>(dataDirectory);
    const wxl_party::SessionSettings partySettings = g_partyStore->Load();
    g_party = std::make_unique<wxl_party::PartyClient>(partySettings);
    if (!g_player.Initialize(g_window, g_device.Get(), g_context.Get()) ||
        !g_ui.Initialize(g_window, g_device.Get(), g_context.Get(), *g_plex, g_player, g_frames))
    {
        g_ui.Shutdown();
        g_player.Shutdown();
        g_plex.reset();
        g_frames.Close();
        g_renderTarget.Reset();
        g_swapChain.Reset();
        g_context.Reset();
        g_device.Reset();
        DestroyWindow(g_window);
        ShutdownUiBridge();
        ShutdownControl();
        wxl_live::ShutdownMediaFoundation();
        CoUninitialize();
        return 4;
    }

    g_ui.ConfigureParty(*g_party, g_sync, *g_partyStore, partySettings);
    g_ui.Hide();
    SetTimer(g_window, kTickTimer, kTickIntervalMs, nullptr);
    if (g_parentProcess) CreateThread(nullptr, 0, WatchParentProcess, nullptr, 0, nullptr);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (g_party) g_party->Disconnect();
    g_liveAudioCapture.Stop();
    g_liveAudioEncoder.Shutdown();
    g_liveAudioDecoder.Shutdown();
    g_liveAudioOutput.Stop();
    g_liveEncoder.Shutdown();
    g_liveDecoder.Shutdown();
    wxl_live::ShutdownMediaFoundation();
    g_ui.Shutdown();
    g_player.Shutdown();
    g_party.reset();
    g_partyStore.reset();
    g_plex.reset();
    g_frames.Close();
    g_renderTarget.Reset();
    g_swapChain.Reset();
    g_context.Reset();
    g_device.Reset();
    ShutdownUiBridge();
    ShutdownControl();
    if (g_parentProcess) CloseHandle(g_parentProcess);
    CoUninitialize();
    return static_cast<int>(message.wParam);
}

