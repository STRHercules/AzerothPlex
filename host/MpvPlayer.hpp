#pragma once

#include "PlexTypes.hpp"
#include "VideoShared.hpp"

#include <mpv/client.h>
#include <mpv/render.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace wxl_mpv
{
    struct MpvEvent
    {
        enum class Kind
        {
            Loaded,
            Ended,
            Error,
            PositionChanged,
            TracksChanged,
        };

        Kind kind = Kind::Error;
        std::string message;
        double positionSeconds = 0.0;
        int errorCode = 0;
    };

    class MpvPlayer final
    {
    public:
        MpvPlayer();
        ~MpvPlayer();

        bool Initialize(HWND window, ID3D11Device* device, ID3D11DeviceContext* context);
        void Shutdown();
        bool Load(const wxl_plex::PlexPlayback& playback);
        void Play();
        void Pause();
        void Stop();
        void Seek(double seconds);
        void SetVolume(int percent);
        void SetAudioTrack(int trackId);
        void SetSubtitleTrack(int trackId);
        void PollEvents(std::vector<MpvEvent>& events);
        bool Render();
        bool CopyFrameTo(uint8_t* destination, uint32_t destinationStride) const;
        bool IsInitialized() const { return handle_ != nullptr && renderContext_ != nullptr; }

    private:
        struct Api;

        bool LoadApi();
        void Command(const std::vector<std::string>& arguments);
        void SetProperty(std::string name, std::string value);

        std::unique_ptr<Api> api_;
        HMODULE module_ = nullptr;
        mpv_handle* handle_ = nullptr;
        mpv_render_context* renderContext_ = nullptr;
        uint8_t* frame_ = nullptr;
        bool hasFrame_ = false;
    };
}
