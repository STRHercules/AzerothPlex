// Physical world-space native Plex client for WarcraftXL build 12340.
// Copyright (C) 2026 WarcraftXL contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "VideoSurface.hpp"

#include "wxl/PluginApi.h"

namespace
{
    constexpr char kTag[] = "wxl-azeroth-plex";

    void DrawPanel(void*)
    {
        wxl_video_screen::VideoSurface::Instance().DrawPanel();
    }
}

const WXL_PluginInfo* __cdecl WXL_Query(void)
{
    static const WXL_PluginInfo info = {
        sizeof(WXL_PluginInfo),
        WXL_API_VERSION,
        "wxl-azeroth-plex",
        300,
        WXL_CLIENT_BUILD,
    };
    return &info;
}

int __cdecl WXL_Load(const WXL_Api* api)
{
    if (!api || api->apiVersion != WXL_API_VERSION || !api->Subscribe || !api->HookAttach ||
        !api->UiAddPanel ||
        !api->UiButton || !api->UiCheckbox || !api->UiSliderFloat || !api->UiSliderInt)
        return 0;

    wxl::ext::EventScript::Bind(api);
    auto& surface = wxl_video_screen::VideoSurface::Instance();
    if (!surface.Initialize(api)) return 0;

    api->UiAddPanel("Azeroth Plex", &DrawPanel, nullptr);
    api->Log(WXL_LOG_INFO, kTag,
             "0.5.0 loaded: native Plex screen + parent-bound helper");
    return 1;
}
