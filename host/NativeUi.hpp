#pragma once

#include "FramePublisher.hpp"
#include "MpvPlayer.hpp"
#include "PlexClient.hpp"
#include "VideoShared.hpp"

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
        std::string watchlistError;
        std::vector<wxl_plex::PlexServer> servers;
        std::vector<wxl_plex::PlexSection> sections;
        std::vector<wxl_plex::PlexItem> items;
        std::vector<wxl_plex::PlexItem> watchlist;
        wxl_plex::PlexItem item;
        bool watchlistLoaded = false;
        int pageOffset = 0;
        int pageSize = 0;
        int pageTotalSize = 0;
        int watchlistOffset = 0;
        int watchlistSize = 0;
        int watchlistTotalSize = 0;

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
        void PumpPlexEvents();
        void HandleUiCommand(const wxl_video_shared::UiCommandPacket& command);
        void PublishUiState(wxl_video_shared::UiSnapshot& snapshot) const;
        void Draw();
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
        void PlayCurrentItem();
        void OpenItem(const wxl_plex::PlexItem& item);
        void NavigateBack();
        void LoadPage(int offset);

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
        std::vector<wxl_plex::PlexItem> rootItems_;
        std::vector<wxl_plex::PlexItem> browseStack_;
        std::string activeSearch_;
        char search_[256]{};
        DWORD lastTimelineAt_ = 0;
        bool paused_ = false;
        bool initialized_ = false;
        bool visible_ = true;
        double lastPositionSeconds_ = 0.0;
        bool browsingWatchlist_ = false;
        bool searchActive_ = false;
        std::string status_ = "Starting Plex helper.";
    };
}
