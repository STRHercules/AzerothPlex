#pragma once

#include "FramePublisher.hpp"
#include "MpvPlayer.hpp"
#include "PlexClient.hpp"

#include <d3d11.h>

#include <string>
#include <vector>

namespace wxl_ui
{
    enum class View
    {
        Login,
        Servers,
        Home,
        Library,
        Search,
        Details,
        Player,
    };

    struct NativeUiState
    {
        View view = View::Login;
        std::string error;
        std::vector<wxl_plex::PlexServer> servers;
        std::vector<wxl_plex::PlexSection> sections;
        std::vector<wxl_plex::PlexItem> items;
        wxl_plex::PlexItem item;

        void Apply(const wxl_plex::PlexEvent& event);
    };

    class NativeUi final
    {
    public:
        bool Initialize(HWND window, ID3D11Device* device, ID3D11DeviceContext* context,
                        wxl_plex::PlexClient& plex, wxl_mpv::MpvPlayer& player,
                        wxl_frame::FramePublisher& frames);
        void SetFrame(const uint8_t* bgra, uint32_t stride);
        void HandleMpvEvents(const std::vector<wxl_mpv::MpvEvent>& events);
        void Draw();
        void Show();
        void Hide();
        bool IsVisible() const { return visible_; }
        void Shutdown();

    private:
        void DrawLogin();
        void DrawServers();
        void DrawHome();
        void DrawLibrary();
        void DrawSearch();
        void DrawDetails();
        void DrawPlayer();

        HWND window_ = nullptr;
        ID3D11Device* device_ = nullptr;
        ID3D11DeviceContext* context_ = nullptr;
        ID3D11Texture2D* videoTexture_ = nullptr;
        ID3D11ShaderResourceView* videoView_ = nullptr;
        wxl_plex::PlexClient* plex_ = nullptr;
        wxl_mpv::MpvPlayer* player_ = nullptr;
        wxl_frame::FramePublisher* frames_ = nullptr;
        wxl_plex::PlexServer selectedServer_;
        wxl_plex::PlexSection selectedSection_;
        wxl_plex::PlexItem playingItem_;
        wxl_plex::PlexPlayback playback_;
        wxl_ui::NativeUiState state_;
        char search_[256]{};
        DWORD lastTimelineAt_ = 0;
        bool paused_ = false;
        bool initialized_ = false;
        bool visible_ = true;
    };
}
