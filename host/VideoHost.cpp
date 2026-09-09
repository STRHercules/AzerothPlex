// Native Plex helper used by wxl-azeroth-plex.
// Copyright (C) 2026 WarcraftXL contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "FramePublisher.hpp"
#include "MpvPlayer.hpp"
#include "NativeUi.hpp"
#include "PlexClient.hpp"
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
    constexpr UINT kShowHostMessage = WM_APP + 0x45;
    constexpr UINT kHideHostMessage = WM_APP + 0x46;

    HWND g_window = nullptr;
    HANDLE g_controlMapping = nullptr;
    void* g_controlView = nullptr;
    HANDLE g_parentProcess = nullptr;
    bool g_background = false;
    LONG g_lastCommandSequence = 0;
    LONG g_lastVolumeSequence = 0;

    ComPtr<ID3D11Device> g_device;
    ComPtr<ID3D11DeviceContext> g_context;
    ComPtr<IDXGISwapChain> g_swapChain;
    ComPtr<ID3D11RenderTargetView> g_renderTarget;
    std::vector<uint8_t> g_frame(wxl_video_shared::kFrameBytes);
    wxl_frame::FramePublisher g_frames;
    wxl_mpv::MpvPlayer g_player;
    std::unique_ptr<wxl_plex::PlexClient> g_plex;
    wxl_ui::NativeUi g_ui;

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
                case wxl_video_shared::Command::Show: g_ui.Show(); break;
                case wxl_video_shared::Command::Hide: g_ui.Hide(); break;
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
        g_player.Render();
        if (g_player.CopyFrameTo(g_frame.data(), wxl_video_shared::kStride))
        {
            g_frames.Publish(g_frame, wxl_video_shared::kStride);
            g_ui.SetFrame(g_frame.data(), wxl_video_shared::kStride);
        }

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
        case kShowHostMessage:
            g_ui.Show();
            return 0;
        case kHideHostMessage:
            g_ui.Hide();
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

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int showCommand)
{
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    g_background = commandLine && std::wcsstr(commandLine, L"--background") != nullptr;
    const DWORD parentPid = ParseParentPid(commandLine);
    if (parentPid) g_parentProcess = OpenProcess(SYNCHRONIZE, FALSE, parentPid);

    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.lpszClassName = kWindowClass;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        CoUninitialize();
        return 2;
    }

    g_window = CreateWindowExW(0, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, CW_USEDEFAULT, 960, 640, nullptr, nullptr,
                               instance, nullptr);
    if (!g_window || !InitializeControl() || !InitializeGraphics(g_window) || !g_frames.Open())
    {
        if (g_window) DestroyWindow(g_window);
        ShutdownControl();
        CoUninitialize();
        return 3;
    }

    g_plex = std::make_unique<wxl_plex::PlexClient>(LocalDataDirectory());
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
        ShutdownControl();
        CoUninitialize();
        return 4;
    }

    if (g_background) g_ui.Hide();
    else ShowWindow(g_window, showCommand == SW_HIDE ? SW_SHOWNORMAL : showCommand);
    UpdateWindow(g_window);
    SetTimer(g_window, kTickTimer, kTickIntervalMs, nullptr);
    if (g_parentProcess) CreateThread(nullptr, 0, WatchParentProcess, nullptr, 0, nullptr);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    g_ui.Shutdown();
    g_player.Shutdown();
    g_plex.reset();
    g_frames.Close();
    g_renderTarget.Reset();
    g_swapChain.Reset();
    g_context.Reset();
    g_device.Reset();
    ShutdownControl();
    if (g_parentProcess) CloseHandle(g_parentProcess);
    CoUninitialize();
    return static_cast<int>(message.wParam);
}
