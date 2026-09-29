#pragma once

#include "FramePublisher.hpp"
#include "MpvPlayer.hpp"
#include "PartyClient.hpp"
#include "PlexClient.hpp"
#include "SessionStore.hpp"
#include "SyncController.hpp"
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
        Party,
        Queue,
        Settings,
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
        void ConfigureParty(wxl_party::PartyClient& party, wxl_party::SyncController& sync,
                            wxl_party::SessionStore& store, wxl_party::SessionSettings settings);
        void HandleMpvEvents(const std::vector<wxl_mpv::MpvEvent>& events);
        void PumpPlexEvents();
        void PumpPartyEvents();
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
        void DrawParty();
        void DrawQueue();
        void DrawSettings();
        void PlayCurrentItem();
        bool StartPlayback(const wxl_plex::PlexItem& item, bool announceParty,
                           int64_t targetPositionMs = 0, bool startPaused = false);
        bool CanControlParty(std::string_view action);
        void BroadcastPlayback(wxl_party::PartyMessageType type);
        void ApplyRemotePlayback(const wxl_party::PartyWireMessage& message, bool reconnect = false);
        void AdvanceQueueOrAutoplay();
        void SavePartySettings();
        static wxl_party::MediaIdentity MediaIdentityFor(const wxl_plex::PlexServer& server,
                                                         const wxl_plex::PlexItem& item);
        static wxl_plex::PlexItem PlexHintFor(const wxl_party::MediaIdentity& media);
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
        wxl_party::PartyClient* party_ = nullptr;
        wxl_party::SyncController* sync_ = nullptr;
        wxl_party::SessionStore* sessionStore_ = nullptr;
        wxl_party::SessionSettings partySettings_{};
        wxl_plex::PlexServer selectedServer_;
        wxl_plex::PlexSection selectedSection_;
        wxl_plex::PlexItem playingItem_;
        wxl_plex::PlexPlayback playback_;
        wxl_ui::NativeUiState state_;
        std::vector<wxl_plex::PlexItem> rootItems_;
        std::vector<wxl_plex::PlexItem> browseStack_;
        wxl_plex::PlexItem nextEpisode_;
        wxl_party::PartyPlaybackState pendingRemotePlayback_{};
        wxl_party::StreamIdentity selectedAudioStream_{};
        wxl_party::StreamIdentity selectedSubtitleStream_{};
        std::vector<wxl_party::PublicRoom> publicRooms_;
        std::string activeSearch_;
        std::string pendingControlRequest_;
        std::string pendingControlRequestId_;
        uint64_t pendingControlRequestEpoch_ = 0;
        uint64_t pendingControlRequestSequence_ = 0;
        std::string partyMediaMatch_ = "No party media.";
        char search_[256]{};
        DWORD lastTimelineAt_ = 0;
        ULONGLONG lastPartyStateAt_ = 0;
        ULONGLONG autoplayDeadline_ = 0;
        ULONGLONG disconnectPauseUntil_ = 0;
        bool paused_ = false;
        bool stopped_ = true;
        bool applyingRemote_ = false;
        bool pendingRemoteMedia_ = false;
        int pendingQueueIndex_ = -1;
        bool pausedForDisconnect_ = false;
        bool readyCountdownTriggered_ = false;
        bool browsingHome_ = false;
        wxl_plex::PlexHomeKind activeHomeKind_ = wxl_plex::PlexHomeKind::ContinueWatching;
        bool initialized_ = false;
        bool visible_ = true;
        double lastPositionSeconds_ = 0.0;
        bool browsingWatchlist_ = false;
        bool searchActive_ = false;
        std::string status_ = "Starting Plex helper.";
    };
}
