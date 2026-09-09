#include "MpvPlayer.hpp"

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <malloc.h>
#include <string>

namespace wxl_mpv
{
    struct MpvPlayer::Api
    {
        using Create = mpv_handle* (*)();
        using SetOptionString = int (*)(mpv_handle*, const char*, const char*);
        using Initialize = int (*)(mpv_handle*);
        using Destroy = void (*)(mpv_handle*);
        using TerminateDestroy = void (*)(mpv_handle*);
        using CommandAsync = int (*)(mpv_handle*, uint64_t, const char**);
        using SetPropertyString = int (*)(mpv_handle*, const char*, const char*);
        using WaitEvent = mpv_event* (*)(mpv_handle*, double);
        using GetPropertyString = char* (*)(mpv_handle*, const char*);
        using Free = void (*)(void*);
        using RenderCreate = int (*)(mpv_render_context**, mpv_handle*, mpv_render_param*);
        using RenderUpdate = uint64_t (*)(mpv_render_context*);
        using Render = int (*)(mpv_render_context*, mpv_render_param*);
        using RenderFree = void (*)(mpv_render_context*);

        Create create = nullptr;
        SetOptionString setOptionString = nullptr;
        Initialize initialize = nullptr;
        Destroy destroy = nullptr;
        TerminateDestroy terminateDestroy = nullptr;
        CommandAsync commandAsync = nullptr;
        SetPropertyString setPropertyString = nullptr;
        WaitEvent waitEvent = nullptr;
        GetPropertyString getPropertyString = nullptr;
        Free free = nullptr;
        RenderCreate renderCreate = nullptr;
        RenderUpdate renderUpdate = nullptr;
        Render render = nullptr;
        RenderFree renderFree = nullptr;

        template <typename Function>
        bool Resolve(HMODULE module, const char* name, Function& function)
        {
            function = reinterpret_cast<Function>(GetProcAddress(module, name));
            return function != nullptr;
        }

        bool Load(HMODULE module)
        {
            return Resolve(module, "mpv_create", create) &&
                   Resolve(module, "mpv_set_option_string", setOptionString) &&
                   Resolve(module, "mpv_initialize", initialize) &&
                   Resolve(module, "mpv_destroy", destroy) &&
                   Resolve(module, "mpv_terminate_destroy", terminateDestroy) &&
                   Resolve(module, "mpv_command_async", commandAsync) &&
                   Resolve(module, "mpv_set_property_string", setPropertyString) &&
                   Resolve(module, "mpv_wait_event", waitEvent) &&
                   Resolve(module, "mpv_get_property_string", getPropertyString) &&
                   Resolve(module, "mpv_free", free) &&
                   Resolve(module, "mpv_render_context_create", renderCreate) &&
                   Resolve(module, "mpv_render_context_update", renderUpdate) &&
                   Resolve(module, "mpv_render_context_render", render) &&
                   Resolve(module, "mpv_render_context_free", renderFree);
        }
    };

    MpvPlayer::MpvPlayer() = default;

    MpvPlayer::~MpvPlayer()
    {
        Shutdown();
    }

    bool MpvPlayer::LoadApi()
    {
        wchar_t modulePath[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
        if (length == 0 || length >= MAX_PATH) return false;
        const auto directory = std::filesystem::path(modulePath).parent_path();
        module_ = LoadLibraryW((directory / L"libmpv-2.dll").c_str());
        if (!module_) module_ = LoadLibraryW(L"libmpv-2.dll");
        if (!module_) return false;

        api_ = std::make_unique<Api>();
        if (!api_->Load(module_))
        {
            api_.reset();
            FreeLibrary(module_);
            module_ = nullptr;
            return false;
        }
        return true;
    }

    bool MpvPlayer::Initialize(HWND window, ID3D11Device*, ID3D11DeviceContext*)
    {
        if (!window || IsInitialized() || !LoadApi()) return false;
        frame_ = static_cast<uint8_t*>(_aligned_malloc(wxl_video_shared::kFrameBytes, 64));
        if (!frame_) return false;

        handle_ = api_->create();
        if (!handle_)
        {
            Shutdown();
            return false;
        }
        if (api_->setOptionString(handle_, "config", "no") < 0 ||
            api_->setOptionString(handle_, "terminal", "no") < 0 ||
            api_->setOptionString(handle_, "vo", "libmpv") < 0 ||
            api_->setOptionString(handle_, "idle", "yes") < 0 ||
            api_->initialize(handle_) < 0)
        {
            Shutdown();
            return false;
        }

        const char* apiType = MPV_RENDER_API_TYPE_SW;
        mpv_render_param createParams[] = {
            {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(apiType)},
            {MPV_RENDER_PARAM_INVALID, nullptr},
        };
        if (api_->renderCreate(&renderContext_, handle_, createParams) < 0 || !renderContext_)
        {
            Shutdown();
            return false;
        }
        return true;
    }

    void MpvPlayer::Shutdown()
    {
        if (renderContext_ && api_)
        {
            api_->renderFree(renderContext_);
            renderContext_ = nullptr;
        }
        if (handle_ && api_)
        {
            api_->terminateDestroy(handle_);
            handle_ = nullptr;
        }
        if (frame_)
        {
            _aligned_free(frame_);
            frame_ = nullptr;
        }
        api_.reset();
        if (module_)
        {
            FreeLibrary(module_);
            module_ = nullptr;
        }
        hasFrame_ = false;
    }

    void MpvPlayer::Command(const std::vector<std::string>& arguments)
    {
        if (!handle_ || !api_ || arguments.empty()) return;
        std::vector<const char*> pointers;
        pointers.reserve(arguments.size() + 1);
        for (const auto& argument : arguments) pointers.push_back(argument.c_str());
        pointers.push_back(nullptr);
        api_->commandAsync(handle_, 0, pointers.data());
    }

    void MpvPlayer::SetProperty(std::string name, std::string value)
    {
        if (handle_ && api_) api_->setPropertyString(handle_, name.c_str(), value.c_str());
    }

    bool MpvPlayer::Load(const wxl_plex::PlexPlayback& playback)
    {
        if (!IsInitialized() || playback.uri.empty()) return false;
        Command({"loadfile", playback.uri, "replace"});
        return true;
    }

    void MpvPlayer::Play()
    {
        SetProperty("pause", "no");
    }

    void MpvPlayer::Pause()
    {
        SetProperty("pause", "yes");
    }

    void MpvPlayer::Stop()
    {
        Command({"stop"});
    }

    void MpvPlayer::Seek(double seconds)
    {
        Command({"seek", std::to_string(seconds), "absolute+exact"});
    }

    void MpvPlayer::SetVolume(int percent)
    {
        SetProperty("volume", std::to_string(std::clamp(percent, 0, 100)));
    }

    void MpvPlayer::SetAudioTrack(int trackId)
    {
        SetProperty("aid", trackId < 0 ? "no" : std::to_string(trackId));
    }

    void MpvPlayer::SetSubtitleTrack(int trackId)
    {
        SetProperty("sid", trackId < 0 ? "no" : std::to_string(trackId));
    }

    void MpvPlayer::PollEvents(std::vector<MpvEvent>& events)
    {
        if (!handle_ || !api_) return;
        while (mpv_event* event = api_->waitEvent(handle_, 0.0))
        {
            if (event->event_id == MPV_EVENT_NONE) break;
            if (event->event_id == MPV_EVENT_FILE_LOADED)
                events.push_back({MpvEvent::Kind::Loaded, "Media loaded"});
            else if (event->event_id == MPV_EVENT_END_FILE)
            {
                const auto* end = static_cast<const mpv_event_end_file*>(event->data);
                if (end && end->reason == MPV_END_FILE_REASON_ERROR)
                    events.push_back({MpvEvent::Kind::Error, "Media load/playback failed", 0.0,
                                      end->error});
                else
                    events.push_back({MpvEvent::Kind::Ended, "Media ended"});
            }
            else if (event->event_id == MPV_EVENT_VIDEO_RECONFIG)
                events.push_back({MpvEvent::Kind::TracksChanged, "Video reconfigured"});
        }

        char* position = api_->getPropertyString(handle_, "time-pos");
        if (position)
        {
            MpvEvent event{MpvEvent::Kind::PositionChanged};
            try { event.positionSeconds = std::stod(position); }
            catch (...) { event.positionSeconds = 0.0; }
            api_->free(position);
            events.push_back(std::move(event));
        }
    }

    bool MpvPlayer::Render()
    {
        if (!IsInitialized() || !frame_) return false;
        const uint64_t flags = api_->renderUpdate(renderContext_);
        if ((flags & MPV_RENDER_UPDATE_FRAME) == 0) return hasFrame_;

        int size[2] = {static_cast<int>(wxl_video_shared::kWidth),
                       static_cast<int>(wxl_video_shared::kHeight)};
        const char* format = "bgr0";
        size_t stride = wxl_video_shared::kStride;
        mpv_render_param params[] = {
            {MPV_RENDER_PARAM_SW_SIZE, size},
            {MPV_RENDER_PARAM_SW_FORMAT, const_cast<char*>(format)},
            {MPV_RENDER_PARAM_SW_STRIDE, &stride},
            {MPV_RENDER_PARAM_SW_POINTER, frame_},
            {MPV_RENDER_PARAM_INVALID, nullptr},
        };
        if (api_->render(renderContext_, params) < 0) return false;
        hasFrame_ = true;
        return true;
    }

    bool MpvPlayer::CopyFrameTo(uint8_t* destination, uint32_t destinationStride) const
    {
        if (!hasFrame_ || !frame_ || !destination || destinationStride < wxl_video_shared::kStride)
            return false;
        for (uint32_t row = 0; row < wxl_video_shared::kHeight; ++row)
        {
            const uint8_t* source = frame_ + size_t(row) * wxl_video_shared::kStride;
            uint8_t* target = destination + size_t(row) * destinationStride;
            for (uint32_t column = 0; column < wxl_video_shared::kWidth; ++column)
            {
                target[column * 4 + 0] = source[column * 4 + 0];
                target[column * 4 + 1] = source[column * 4 + 1];
                target[column * 4 + 2] = source[column * 4 + 2];
                target[column * 4 + 3] = 0xFF;
            }
        }
        return true;
    }
}
