#include "NativeUi.hpp"

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstring>
#include <sstream>
#include <string_view>

namespace wxl_ui
{
    namespace
    {
        template <size_t Size>
        void CopyText(char (&destination)[Size], std::string_view source)
        {
            const size_t length = std::min(source.size(), Size - 1);
            std::memcpy(destination, source.data(), length);
            destination[length] = '\0';
        }

        std::string BoundedText(const char* source, size_t capacity)
        {
            if (!source) return {};
            size_t length = 0;
            while (length < capacity && source[length]) ++length;
            return std::string(source, length);
        }

        bool ValidIndex(LONG index, size_t count)
        {
            return index >= 0 && static_cast<size_t>(index) < count;
        }

        const char* DefaultStatus(View view)
        {
            switch (view)
            {
            case View::Login: return "Sign in to Plex.";
            case View::Servers: return "Choose a Plex server.";
            case View::Home: return "Choose a Plex library.";
            case View::Library: return "Choose media.";
            case View::Search: return "Search Plex.";
            case View::Details: return "Review the selected Plex item.";
            case View::Player: return "Playback controls are ready.";
            case View::Party: return "Watch Together.";
            case View::Queue: return "Watch-party queue.";
            case View::Settings: return "AzerothPlex settings.";
            }
            return "Plex is ready.";
        }

        int64_t SystemNowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        }

        const char* HomeTitle(wxl_plex::PlexHomeKind kind)
        {
            switch (kind)
            {
            case wxl_plex::PlexHomeKind::ContinueWatching: return "Continue Watching";
            case wxl_plex::PlexHomeKind::RecentlyAddedMovies: return "Recently Added Movies";
            case wxl_plex::PlexHomeKind::RecentlyAddedTV: return "Recently Added TV";
            case wxl_plex::PlexHomeKind::OnDeck: return "On Deck";
            case wxl_plex::PlexHomeKind::RecentlyWatched: return "Recently Watched";
            }
            return "Plex Home";
        }

        bool ParseCinemaSettings(std::string_view text, wxl_party::CinemaSettings& cinema)
        {
            std::vector<std::string> fields;
            std::stringstream stream{std::string(text)};
            std::string field;
            while (std::getline(stream, field, '|')) fields.push_back(std::move(field));
            if (fields.size() != 16) return false;
            try
            {
                cinema.presetName = fields[0].substr(0, 63);
                cinema.width = std::clamp(std::stof(fields[1]), 2.0f, 30.0f);
                cinema.heightOffset = std::clamp(std::stof(fields[2]), -10.0f, 20.0f);
                cinema.rotationDegrees = std::stof(fields[3]);
                cinema.pinned = std::stoi(fields[4]) != 0;
                cinema.pinnedX = std::stof(fields[5]);
                cinema.pinnedY = std::stof(fields[6]);
                cinema.pinnedWidth = std::clamp(std::stof(fields[7]), 0.15f, 0.80f);
                cinema.depthOcclusion = std::stoi(fields[8]) != 0;
                cinema.depthOffset = std::clamp(std::stof(fields[9]), -0.030f, 0.005f);
                cinema.distanceAudio = std::stoi(fields[10]) != 0;
                cinema.audioRange = std::clamp(std::stof(fields[11]), 5.0f, 200.0f);
                cinema.aspectRatioMode = std::clamp(std::stoi(fields[12]), 0, 4);
                cinema.fitMode = std::clamp(std::stoi(fields[13]), 0, 3);
                cinema.frameStyle = std::clamp(std::stoi(fields[14]), 0, 8);
                cinema.ambientLighting = std::stoi(fields[15]) != 0;
                return true;
            }
            catch (...) { return false; }
        }

        wxl_party::StreamIdentity StreamIdentityFor(const wxl_plex::PlexStream& stream)
        {
            wxl_party::StreamIdentity identity;
            identity.language = stream.language;
            identity.title = stream.title;
            identity.codec = stream.codec;
            identity.forced = stream.forced;
            identity.hearingImpaired = stream.hearingImpaired;
            identity.valid = true;
            return identity;
        }

        const wxl_plex::PlexStream* MatchStream(const wxl_plex::PlexItem& item,
                                                const wxl_party::StreamIdentity& wanted,
                                                int streamType)
        {
            if (!wanted.valid) return nullptr;
            const wxl_plex::PlexStream* fallback = nullptr;
            for (const auto& stream : item.streams)
            {
                if (stream.streamType != streamType) continue;
                const auto candidate = StreamIdentityFor(stream);
                if (wxl_party::SameStream(candidate, wanted)) return &stream;
                if (!fallback && !wanted.language.empty() && stream.language == wanted.language)
                    fallback = &stream;
            }
            return fallback;
        }

        bool HasChildren(const std::string& type)
        {
            return type == "show" || type == "season" || type == "artist" ||
                   type == "album" || type == "photoalbum" || type == "collection" ||
                   type == "playlist";
        }
    }

    void NativeUiState::Apply(const wxl_plex::PlexEvent& event)
    {
        error.clear();
        switch (event.kind)
        {
        case wxl_plex::PlexEvent::Kind::LoginRequired:
            view = View::Login;
            break;
        case wxl_plex::PlexEvent::Kind::LoginPending:
            view = View::Login;
            error = event.message;
            break;
        case wxl_plex::PlexEvent::Kind::LoginSucceeded:
            view = View::Home;
            break;
        case wxl_plex::PlexEvent::Kind::Servers:
            servers = event.servers;
            view = View::Servers;
            break;
        case wxl_plex::PlexEvent::Kind::Sections:
            sections = event.sections;
            view = View::Home;
            break;
        case wxl_plex::PlexEvent::Kind::Items:
            items = event.items;
            view = View::Library;
            pageOffset = event.pageOffset;
            pageSize = event.pageSize;
            pageTotalSize = event.pageTotalSize;
            break;
        case wxl_plex::PlexEvent::Kind::Children:
            items = event.items;
            view = View::Library;
            pageOffset = event.pageOffset;
            pageSize = event.pageSize;
            pageTotalSize = event.pageTotalSize;
            break;
        case wxl_plex::PlexEvent::Kind::Watchlist:
            watchlist = event.items;
            watchlistError = event.message;
            watchlistLoaded = true;
            watchlistOffset = event.pageOffset;
            watchlistSize = event.pageSize;
            watchlistTotalSize = event.pageTotalSize;
            break;
        case wxl_plex::PlexEvent::Kind::WatchlistResolved:
            break;
        case wxl_plex::PlexEvent::Kind::HomeItems:
            items = event.items;
            view = View::Library;
            pageOffset = event.pageOffset;
            pageSize = event.pageSize;
            pageTotalSize = event.pageTotalSize;
            break;
        case wxl_plex::PlexEvent::Kind::Metadata:
            item = event.item;
            view = View::Details;
            break;
        case wxl_plex::PlexEvent::Kind::NextEpisode:
        case wxl_plex::PlexEvent::Kind::PartyMediaResolved:
            break;
        case wxl_plex::PlexEvent::Kind::Error:
            error = event.message;
            if (!event.preserveView) view = View::Player;
            break;
        }
    }

    bool NativeUi::Initialize(HWND window, ID3D11Device* device, ID3D11DeviceContext* context,
                              wxl_plex::PlexClient& plex, wxl_mpv::MpvPlayer& player,
                              wxl_frame::FramePublisher& frames)
    {
        if (!window || !device || !context || initialized_) return false;
        window_ = window;
        device_ = device;
        context_ = context;
        plex_ = &plex;
        player_ = &player;
        frames_ = &frames;
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();
        if (!ImGui_ImplWin32_Init(window_) || !ImGui_ImplDX11_Init(device_, context_))
        {
            Shutdown();
            return false;
        }
        initialized_ = true;
        return true;
    }

    void NativeUi::ConfigureParty(wxl_party::PartyClient& party, wxl_party::SyncController& sync,
                                  wxl_party::SessionStore& store, wxl_party::SessionSettings settings)
    {
        party_ = &party;
        sync_ = &sync;
        sessionStore_ = &store;
        partySettings_ = std::move(settings);
        sync_->SetPermissions({partySettings_.permissionMode,
                               partySettings_.syncAudio,
                               partySettings_.syncSubtitles,
                               partySettings_.queueMode});
        sync_->SetThresholds(partySettings_.noCorrectionDriftMs,
                             partySettings_.hardSeekDriftMs);
        if (player_) player_->SetPlaybackSpeed(partySettings_.playbackSpeed);
        sync_->SetBaseSpeed(player_ ? player_->PlaybackSpeed() : partySettings_.playbackSpeed);
    }

    void NativeUi::SavePartySettings()
    {
        if (sessionStore_) sessionStore_->Save(partySettings_);
        if (party_) party_->Configure(partySettings_);
    }

    wxl_party::MediaIdentity NativeUi::MediaIdentityFor(const wxl_plex::PlexServer& server,
                                                         const wxl_plex::PlexItem& item)
    {
        wxl_party::MediaIdentity media;
        media.serverIdentifier = server.machineIdentifier;
        media.ratingKey = item.ratingKey;
        media.plexGuid = item.guid;
        media.imdbId = item.imdbId;
        media.tmdbId = item.tmdbId;
        media.tvdbId = item.tvdbId;
        media.type = item.type;
        media.title = item.title;
        media.grandparentTitle = item.grandparentTitle;
        media.year = item.year;
        media.season = item.parentIndex;
        media.episode = item.index;
        media.durationMs = item.durationMs;
        return media;
    }

    wxl_plex::PlexItem NativeUi::PlexHintFor(const wxl_party::MediaIdentity& media)
    {
        wxl_plex::PlexItem item;
        item.ratingKey = media.ratingKey;
        item.guid = media.plexGuid;
        item.imdbId = media.imdbId;
        item.tmdbId = media.tmdbId;
        item.tvdbId = media.tvdbId;
        item.type = media.type;
        item.title = media.title;
        item.grandparentTitle = media.grandparentTitle;
        item.year = media.year;
        item.parentIndex = media.season;
        item.index = media.episode;
        item.durationMs = static_cast<int>(std::clamp<int64_t>(media.durationMs, 0, INT_MAX));
        return item;
    }

    void NativeUi::SetFrame(const uint8_t* bgra, uint32_t stride)
    {
        if (!initialized_ || !bgra || stride < wxl_video_shared::kStride) return;
        if (!videoTexture_)
        {
            D3D11_TEXTURE2D_DESC description{};
            description.Width = wxl_video_shared::kWidth;
            description.Height = wxl_video_shared::kHeight;
            description.MipLevels = 1;
            description.ArraySize = 1;
            description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            description.SampleDesc.Count = 1;
            description.Usage = D3D11_USAGE_DEFAULT;
            description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(device_->CreateTexture2D(&description, nullptr, &videoTexture_))) return;
            if (FAILED(device_->CreateShaderResourceView(videoTexture_, nullptr, &videoView_)))
            {
                videoTexture_->Release();
                videoTexture_ = nullptr;
                return;
            }
        }
        D3D11_BOX box{0, 0, 0, wxl_video_shared::kWidth,
                      wxl_video_shared::kHeight, 1};
        context_->UpdateSubresource(videoTexture_, 0, &box, bgra, stride, 0);
    }

    void NativeUi::PumpPlexEvents()
    {
        if (!plex_) return;
        wxl_plex::PlexEvent event;
        while (plex_->PopEvent(event))
        {
            if (event.kind == wxl_plex::PlexEvent::Kind::WatchlistResolved)
            {
                state_.error.clear();
                if (event.item.ratingKey.empty())
                {
                    state_.error = event.message.empty()
                        ? "The watchlist item could not be resolved."
                        : event.message;
                    status_ = state_.error;
                }
                else
                {
                    OpenItem(event.item);
                }
                continue;
            }

            if (event.kind == wxl_plex::PlexEvent::Kind::NextEpisode)
            {
                if (event.item.ratingKey.empty())
                {
                    nextEpisode_ = {};
                    autoplayDeadline_ = 0;
                    if (!event.message.empty()) status_ = event.message;
                }
                else
                {
                    nextEpisode_ = event.item;
                    autoplayDeadline_ = GetTickCount64() + 15000;
                    status_ = "Next episode starts in 15 seconds.";
                }
                continue;
            }

            if (event.kind == wxl_plex::PlexEvent::Kind::PartyMediaResolved)
            {
                if (event.item.ratingKey.empty())
                {
                    pendingRemoteMedia_ = false;
                    pendingQueueIndex_ = -1;
                    partyMediaMatch_ = "No Match";
                    state_.error = event.message.empty()
                        ? "This party media could not be matched in your Plex library."
                        : event.message;
                    status_ = state_.error;
                    continue;
                }

                if (pendingRemoteMedia_ && sync_)
                {
                    const int64_t now = SystemNowMs() + (party_ ? party_->ClockOffsetMs() : 0);
                    const auto decision = sync_->EvaluateRemoteState(
                        pendingRemotePlayback_, now, 0, true, {}, true, true);
                    partyMediaMatch_ = wxl_party::ToString(
                        wxl_party::MatchMediaIdentity(MediaIdentityFor(selectedServer_, event.item),
                                                      pendingRemotePlayback_.media));
                    pendingRemoteMedia_ = false;
                    if (decision.accepted)
                        StartPlayback(event.item, false, decision.targetPositionMs,
                                      pendingRemotePlayback_.paused);
                    continue;
                }

                if (pendingQueueIndex_ >= 0)
                {
                    const int resolvedIndex = pendingQueueIndex_;
                    pendingQueueIndex_ = -1;
                    if (party_)
                    {
                        auto room = party_->Room();
                        if (ValidIndex(resolvedIndex, room.queue.size()))
                        {
                            room.queue.erase(room.queue.begin() + resolvedIndex);
                            for (size_t index = 0; index < room.queue.size(); ++index)
                                room.queue[index].order = index + 1;
                            party_->SendQueue(room.queue);
                        }
                    }
                    StartPlayback(event.item, true);
                    continue;
                }
            }

            state_.Apply(event);
            if (event.kind == wxl_plex::PlexEvent::Kind::Items)
            {
                rootItems_ = state_.items;
                browseStack_.clear();
                browsingWatchlist_ = false;
                browsingHome_ = false;
            }
            else if (event.kind == wxl_plex::PlexEvent::Kind::HomeItems)
            {
                rootItems_ = state_.items;
                browseStack_.clear();
                browsingWatchlist_ = false;
                searchActive_ = false;
                browsingHome_ = true;
                activeHomeKind_ = event.homeKind;
                status_ = HomeTitle(activeHomeKind_);
            }
            else if (event.kind == wxl_plex::PlexEvent::Kind::Watchlist &&
                     browsingWatchlist_ && browseStack_.empty())
            {
                rootItems_ = state_.watchlist;
                state_.items = rootItems_;
                state_.pageOffset = state_.watchlistOffset;
                state_.pageSize = state_.watchlistSize;
                state_.pageTotalSize = state_.watchlistTotalSize;
            }
            if (!event.message.empty())
            {
                status_ = event.message;
                continue;
            }
            if (event.kind != wxl_plex::PlexEvent::Kind::HomeItems)
                status_ = DefaultStatus(state_.view);
        }

        if (autoplayDeadline_ != 0 && GetTickCount64() >= autoplayDeadline_ &&
            !nextEpisode_.ratingKey.empty())
        {
            autoplayDeadline_ = 0;
            const wxl_plex::PlexItem next = nextEpisode_;
            nextEpisode_ = {};
            StartPlayback(next, true);
        }

        if (pausedForDisconnect_ && disconnectPauseUntil_ != 0 &&
            GetTickCount64() >= disconnectPauseUntil_)
        {
            pausedForDisconnect_ = false;
            disconnectPauseUntil_ = 0;
            paused_ = false;
            stopped_ = false;
            player_->Play();
            BroadcastPlayback(wxl_party::PartyMessageType::Play);
            status_ = "Watch party resumed after the disconnect grace period.";
        }
    }

    void NativeUi::OpenItem(const wxl_plex::PlexItem& item)
    {
        if (item.ratingKey.empty())
        {
            state_.error = "The selected Plex item has no rating key.";
            return;
        }
        if (HasChildren(item.type))
        {
            browseStack_.push_back(item);
            status_ = "Loading " + item.title + " contents.";
            plex_->LoadChildren(selectedServer_, item, 0, wxl_plex::kPlexPageSize);
        }
        else
        {
            status_ = "Loading Plex metadata.";
            plex_->LoadMetadata(selectedServer_, item);
        }
    }

    void NativeUi::NavigateBack()
    {
        state_.error.clear();
        if (browseStack_.empty())
        {
            state_.items = rootItems_;
            state_.view = View::Library;
            status_ = DefaultStatus(View::Library);
            return;
        }

        browseStack_.pop_back();
        if (browseStack_.empty())
        {
            state_.items = rootItems_;
            state_.view = View::Library;
            status_ = browsingWatchlist_ ? "Plex watchlist." : DefaultStatus(View::Library);
            return;
        }

        status_ = "Loading " + browseStack_.back().title + " contents.";
        plex_->LoadChildren(selectedServer_, browseStack_.back(), 0, wxl_plex::kPlexPageSize);
    }

    void NativeUi::LoadPage(int offset)
    {
        offset = std::max(0, offset);
        state_.error.clear();
        state_.items.clear();
        if (!browseStack_.empty())
        {
            status_ = "Loading more " + browseStack_.back().title + " contents.";
            plex_->LoadChildren(selectedServer_, browseStack_.back(), offset,
                                wxl_plex::kPlexPageSize);
        }
        else if (browsingWatchlist_)
        {
            status_ = "Loading more of your Plex watchlist.";
            plex_->LoadWatchlist(offset, wxl_plex::kPlexPageSize);
        }
        else if (browsingHome_)
        {
            status_ = std::string("Loading more ") + HomeTitle(activeHomeKind_) + ".";
            plex_->LoadHome(selectedServer_, activeHomeKind_, offset, wxl_plex::kPlexPageSize);
        }
        else if (searchActive_)
        {
            status_ = "Loading more Plex search results.";
            plex_->Search(selectedServer_, activeSearch_, offset, wxl_plex::kPlexPageSize);
        }
        else
        {
            status_ = "Loading more Plex library items.";
            plex_->LoadItems(selectedServer_, selectedSection_, offset,
                             wxl_plex::kPlexPageSize);
        }
    }

    bool NativeUi::CanControlParty(std::string_view action)
    {
        if (!party_ || !sync_) return true;
        const auto room = party_->Room();
        if (room.code.empty()) return true;
        sync_->SetRole(party_->Role());
        sync_->SetPermissions(room.permissions);
        if (sync_->CanControlLocally()) return true;
        if (sync_->ShouldRequestControl())
        {
            party_->SendControlRequest(std::string(action));
            status_ = "Control request sent to the watch-party host.";
        }
        else
        {
            state_.error = "Only the watch-party host can control playback.";
            status_ = state_.error;
        }
        return false;
    }

    void NativeUi::BroadcastPlayback(wxl_party::PartyMessageType type)
    {
        if (!party_ || !sync_ || playingItem_.ratingKey.empty()) return;
        const auto room = party_->Room();
        if (room.code.empty()) return;
        const auto role = party_->Role();
        if (role != wxl_party::PartyRole::Host &&
            room.permissions.mode != wxl_party::PartyPermissionMode::SharedControl)
            return;

        sync_->SetRole(role);
        sync_->SetPermissions(room.permissions);
        const int64_t now = SystemNowMs() + party_->ClockOffsetMs();
        auto playback = sync_->BuildLocalState(
            MediaIdentityFor(selectedServer_, playingItem_),
            static_cast<int64_t>(std::max(0.0, lastPositionSeconds_) * 1000.0),
            playingItem_.durationMs, paused_, stopped_, now,
            room.roomEpoch, room.sequence + 1);
        playback.audio = selectedAudioStream_;
        playback.subtitle = selectedSubtitleStream_;
        party_->SendPlaybackState(type, playback);
        lastPartyStateAt_ = GetTickCount64();
    }

    bool NativeUi::StartPlayback(const wxl_plex::PlexItem& item, bool announceParty,
                                 int64_t targetPositionMs, bool startPaused)
    {
        if (!player_ || item.ratingKey.empty()) return false;
        const auto playback = wxl_plex::BuildDirectPlayback(item, selectedServer_);
        if (playback.uri.empty() || !player_->Load(playback))
        {
            state_.error = "This Plex item has no direct playable part.";
            status_ = state_.error;
            return false;
        }

        state_.item = item;
        playingItem_ = item;
        playback_ = playback;
        selectedAudioStream_ = {};
        selectedSubtitleStream_ = {};
        for (const auto& stream : item.streams)
        {
            if (!stream.selected) continue;
            if (stream.streamType == 2) selectedAudioStream_ = StreamIdentityFor(stream);
            else if (stream.streamType == 3) selectedSubtitleStream_ = StreamIdentityFor(stream);
        }
        paused_ = startPaused;
        stopped_ = false;
        lastTimelineAt_ = 0;
        lastPositionSeconds_ = std::max<int64_t>(0, targetPositionMs) / 1000.0;
        autoplayDeadline_ = 0;
        nextEpisode_ = {};
        player_->SetSyncCorrection(1.0);
        player_->Play();
        if (targetPositionMs > 0)
            player_->Seek(targetPositionMs / 1000.0);
        if (startPaused)
            player_->Pause();
        state_.view = View::Player;
        status_ = "Playing " + playingItem_.title + ".";
        if (announceParty)
            BroadcastPlayback(wxl_party::PartyMessageType::MediaChanged);
        return true;
    }

    void NativeUi::ApplyRemotePlayback(const wxl_party::PartyWireMessage& message, bool reconnect)
    {
        if (!party_ || !sync_ || !player_) return;
        const auto& remote = message.playback;
        if (remote.sequence == 0 && message.sequence == 0) return;
        wxl_party::PartyPlaybackState normalized = remote;
        if (normalized.roomEpoch == 0) normalized.roomEpoch = message.roomEpoch;
        if (normalized.sequence == 0) normalized.sequence = message.sequence;

        sync_->SetRole(party_->Role());
        sync_->SetPermissions(party_->Room().permissions);
        sync_->SetBaseSpeed(player_->PlaybackSpeed());

        const wxl_party::MediaIdentity local = playingItem_.ratingKey.empty()
            ? wxl_party::MediaIdentity{}
            : MediaIdentityFor(selectedServer_, playingItem_);
        if (!normalized.media.ratingKey.empty() &&
            wxl_party::MatchMediaIdentity(local, normalized.media) == wxl_party::MediaMatchQuality::None)
        {
            if (!sync_->IsNewer(normalized.roomEpoch, normalized.sequence)) return;
            if (selectedServer_.uri.empty())
            {
                state_.error = "Choose a Plex server before joining active party playback.";
                return;
            }
            pendingRemotePlayback_ = normalized;
            pendingRemoteMedia_ = true;
            partyMediaMatch_ = "Resolving party media...";
            plex_->ResolvePartyMedia(selectedServer_, PlexHintFor(normalized.media),
                                     normalized.media.serverIdentifier);
            return;
        }

        const int64_t now = SystemNowMs() + party_->ClockOffsetMs();
        const auto decision = sync_->EvaluateRemoteState(
            normalized, now,
            static_cast<int64_t>(std::max(0.0, lastPositionSeconds_) * 1000.0),
            paused_ || stopped_, local,
            message.type == wxl_party::PartyMessageType::Seek, reconnect);
        if (!decision.accepted) return;

        partyMediaMatch_ = wxl_party::ToString(wxl_party::MatchMediaIdentity(local, normalized.media));
        const double base = std::max(0.25, player_->PlaybackSpeed());
        player_->SetSyncCorrection(decision.effectiveSpeed / base);
        applyingRemote_ = true;
        if (decision.shouldStop)
        {
            player_->Stop();
            stopped_ = true;
            paused_ = false;
            lastPositionSeconds_ = 0.0;
        }
        else
        {
            if (decision.shouldSeek)
            {
                player_->Seek(decision.targetPositionMs / 1000.0);
                lastPositionSeconds_ = decision.targetPositionMs / 1000.0;
            }
            if (decision.shouldPause)
            {
                player_->Pause();
                paused_ = true;
                stopped_ = false;
            }
            else if (decision.shouldPlay)
            {
                player_->Play();
                paused_ = false;
                stopped_ = false;
            }
        }

        const auto permissions = party_->Room().permissions;
        if (permissions.syncAudio && normalized.audio.valid)
        {
            if (const auto* stream = MatchStream(playingItem_, normalized.audio, 2))
            {
                player_->SetAudioTrack(stream->id);
                selectedAudioStream_ = StreamIdentityFor(*stream);
            }
        }
        if (permissions.syncSubtitles && normalized.subtitle.valid)
        {
            if (const auto* stream = MatchStream(playingItem_, normalized.subtitle, 3))
            {
                player_->SetSubtitleTrack(stream->id);
                selectedSubtitleStream_ = StreamIdentityFor(*stream);
            }
        }
        applyingRemote_ = false;
    }

    void NativeUi::PumpPartyEvents()
    {
        if (!party_ || !sync_) return;
        wxl_party::PartyWireMessage event;
        while (party_->PopEvent(event))
        {
            switch (event.type)
            {
            case wxl_party::PartyMessageType::RoomCreated:
            case wxl_party::PartyMessageType::RoomJoined:
            case wxl_party::PartyMessageType::RoomState:
                sync_->SetRole(party_->Role());
                sync_->SetPermissions(party_->Room().permissions);
                partySettings_.lastRoomCode = party_->Room().code;
                SavePartySettings();
                if (party_->Role() == wxl_party::PartyRole::Guest &&
                    !event.playback.media.title.empty())
                    ApplyRemotePlayback(event, true);
                break;
            case wxl_party::PartyMessageType::Play:
            case wxl_party::PartyMessageType::Pause:
            case wxl_party::PartyMessageType::Seek:
            case wxl_party::PartyMessageType::Stop:
            case wxl_party::PartyMessageType::MediaChanged:
            case wxl_party::PartyMessageType::PlaybackState:
                if (!pendingControlRequestId_.empty() &&
                    (event.roomEpoch > pendingControlRequestEpoch_ ||
                     (event.roomEpoch == pendingControlRequestEpoch_ &&
                      event.sequence > pendingControlRequestSequence_)))
                {
                    pendingControlRequest_.clear();
                    pendingControlRequestId_.clear();
                    pendingControlRequestEpoch_ = 0;
                    pendingControlRequestSequence_ = 0;
                }
                if (event.senderId != party_->ClientId()) ApplyRemotePlayback(event);
                break;
            case wxl_party::PartyMessageType::Permissions:
                sync_->SetPermissions(party_->Room().permissions);
                break;
            case wxl_party::PartyMessageType::HostChanged:
                pendingControlRequest_.clear();
                pendingControlRequestId_.clear();
                pendingControlRequestEpoch_ = 0;
                pendingControlRequestSequence_ = 0;
                sync_->Reset();
                sync_->SetRole(party_->Role());
                sync_->SetPermissions(party_->Room().permissions);
                status_ = party_->Role() == wxl_party::PartyRole::Host
                    ? "You are now the watch-party host."
                    : "The watch party has a new host.";
                break;
            case wxl_party::PartyMessageType::ControlRequest:
                if (party_->Role() == wxl_party::PartyRole::Host)
                {
                    pendingControlRequest_ = event.request;
                    pendingControlRequestId_ = event.requestId;
                    pendingControlRequestEpoch_ = event.roomEpoch;
                    pendingControlRequestSequence_ = event.sequence;
                    status_ = "A party member requested playback control.";
                }
                break;
            case wxl_party::PartyMessageType::MemberLeft:
                if (party_->Role() == wxl_party::PartyRole::Host && !paused_ && !stopped_)
                {
                    if (partySettings_.disconnectPolicy == wxl_party::DisconnectPolicy::PauseFor60Seconds ||
                        partySettings_.disconnectPolicy == wxl_party::DisconnectPolicy::PauseUntilReconnect)
                    {
                        player_->Pause();
                        paused_ = true;
                        pausedForDisconnect_ = true;
                        disconnectPauseUntil_ = partySettings_.disconnectPolicy == wxl_party::DisconnectPolicy::PauseFor60Seconds
                            ? GetTickCount64() + 60000 : 0;
                        BroadcastPlayback(wxl_party::PartyMessageType::Pause);
                        status_ = "Playback paused because a party member disconnected.";
                    }
                    else if (partySettings_.disconnectPolicy == wxl_party::DisconnectPolicy::AskHost)
                    {
                        status_ = "A party member disconnected. Pause playback if needed.";
                    }
                }
                break;
            case wxl_party::PartyMessageType::MemberJoined:
                if (party_->Role() == wxl_party::PartyRole::Host && pausedForDisconnect_ &&
                    partySettings_.disconnectPolicy == wxl_party::DisconnectPolicy::PauseUntilReconnect)
                {
                    pausedForDisconnect_ = false;
                    player_->Play();
                    paused_ = false;
                    stopped_ = false;
                    BroadcastPlayback(wxl_party::PartyMessageType::Play);
                    status_ = "Disconnected party member rejoined; playback resumed.";
                }
                break;
            case wxl_party::PartyMessageType::ReadyCheck:
            case wxl_party::PartyMessageType::ReadyState:
                if (party_->Room().readyCheckActive && party_->Room().readyStartAtMs == 0)
                    status_ = "Ready check: waiting for everyone.";
                break;
            case wxl_party::PartyMessageType::PublicRooms:
                publicRooms_ = event.publicRooms;
                break;
            case wxl_party::PartyMessageType::LiveStarted:
                liveBroadcasting_ = event.live.broadcasterId == party_->ClientId();
                status_ = liveBroadcasting_
                    ? "You are live. Room members can watch your broadcast."
                    : "A live broadcast started in this room.";
                break;
            case wxl_party::PartyMessageType::LiveStopped:
                liveBroadcasting_ = false;
                liveWatching_ = false;
                status_ = "The live broadcast ended.";
                break;
            case wxl_party::PartyMessageType::LiveState:
                if (!event.live.active)
                {
                    liveBroadcasting_ = false;
                    liveWatching_ = false;
                }
                break;
            case wxl_party::PartyMessageType::Error:
                state_.error = event.error.empty() ? "Watch Together error." : event.error;
                status_ = state_.error;
                break;
            default:
                break;
            }
        }

        const auto room = party_->Room();
        if (!room.readyCheckActive)
        {
            readyCountdownTriggered_ = false;
        }
        else if (room.readyStartAtMs <= 0)
        {
            readyCountdownTriggered_ = false;
        }
        else if (party_->Role() == wxl_party::PartyRole::Host && !readyCountdownTriggered_ &&
                 SystemNowMs() + party_->ClockOffsetMs() >= room.readyStartAtMs)
        {
            readyCountdownTriggered_ = true;
            if (playingItem_.ratingKey.empty())
            {
                state_.error = "The ready check completed, but no Plex item is loaded.";
                status_ = state_.error;
            }
            else if (stopped_)
            {
                StartPlayback(playingItem_, true);
                status_ = "Everyone is ready. Playback started.";
            }
            else
            {
                if (paused_)
                {
                    player_->Play();
                    paused_ = false;
                    stopped_ = false;
                }
                BroadcastPlayback(wxl_party::PartyMessageType::Play);
                status_ = "Everyone is ready. Playback started.";
            }
        }
    }

    void NativeUi::AdvanceQueueOrAutoplay()
    {
        if (party_ && party_->Role() == wxl_party::PartyRole::Guest) return;
        if (party_)
        {
            const auto room = party_->Room();
            if (!room.queue.empty())
            {
                pendingQueueIndex_ = 0;
                plex_->ResolvePartyMedia(selectedServer_, PlexHintFor(room.queue.front().media),
                                         room.queue.front().media.serverIdentifier);
                status_ = "Loading the next watch-party queue item.";
                return;
            }
        }
        if (playingItem_.type == "episode")
        {
            status_ = "Looking for the next episode.";
            plex_->LoadNextEpisode(selectedServer_, playingItem_);
        }
    }

    void NativeUi::HandleUiCommand(const wxl_video_shared::UiCommandPacket& command)
    {
        if (!plex_ || !player_) return;
        state_.error.clear();

        switch (static_cast<wxl_video_shared::UiCommand>(command.command))
        {
        case wxl_video_shared::UiCommand::StartLogin:
            status_ = "Opening Plex sign-in in your browser.";
            plex_->StartLogin();
            break;
        case wxl_video_shared::UiCommand::SelectServer:
            if (!ValidIndex(command.index, state_.servers.size()))
            {
                state_.error = "The selected Plex server is no longer available.";
                break;
            }
            selectedServer_ = state_.servers[static_cast<size_t>(command.index)];
            selectedSection_ = {};
            state_.sections.clear();
            state_.items.clear();
            rootItems_.clear();
            browseStack_.clear();
            browsingWatchlist_ = false;
            browsingHome_ = false;
            searchActive_ = false;
            status_ = "Loading Plex libraries.";
            plex_->LoadSections(selectedServer_);
            break;
        case wxl_video_shared::UiCommand::SelectSection:
            if (!ValidIndex(command.index, state_.sections.size()))
            {
                state_.error = "The selected Plex library is no longer available.";
                break;
            }
            selectedSection_ = state_.sections[static_cast<size_t>(command.index)];
            state_.items.clear();
            rootItems_.clear();
            browseStack_.clear();
            browsingWatchlist_ = false;
            browsingHome_ = false;
            searchActive_ = false;
            status_ = "Loading Plex library items.";
            plex_->LoadItems(selectedServer_, selectedSection_, 0, wxl_plex::kPlexPageSize);
            break;
        case wxl_video_shared::UiCommand::SelectItem:
        {
            if (!ValidIndex(command.index, state_.items.size()))
            {
                state_.error = "The selected Plex item is no longer available.";
                break;
            }
            const auto& item = state_.items[static_cast<size_t>(command.index)];
            if (browsingWatchlist_ && browseStack_.empty())
            {
                status_ = "Finding the watchlist item on the selected Plex server.";
                plex_->ResolveWatchlistItem(selectedServer_, item);
            }
            else
            {
                OpenItem(item);
            }
            break;
        }
        case wxl_video_shared::UiCommand::Search:
        {
            const std::string query = BoundedText(command.text, sizeof(command.text));
            if (selectedServer_.uri.empty())
            {
                state_.error = "Choose a Plex server before searching.";
                break;
            }
            if (query.empty())
            {
                state_.error = "Enter a Plex search term.";
                break;
            }
            state_.items.clear();
            rootItems_.clear();
            browseStack_.clear();
            browsingWatchlist_ = false;
            browsingHome_ = false;
            activeSearch_ = query;
            searchActive_ = true;
            status_ = "Searching Plex.";
            plex_->Search(selectedServer_, query, 0, wxl_plex::kPlexPageSize);
            break;
        }
        case wxl_video_shared::UiCommand::Play:
            if (!playingItem_.ratingKey.empty() && stopped_ == false)
            {
                if (!CanControlParty("play")) break;
                paused_ = false;
                stopped_ = false;
                player_->Play();
                BroadcastPlayback(wxl_party::PartyMessageType::Play);
                status_ = "Playback resumed.";
            }
            else if (CanControlParty("play"))
            {
                PlayCurrentItem();
            }
            break;
        case wxl_video_shared::UiCommand::Pause:
            if (!CanControlParty("pause")) break;
            paused_ = true;
            stopped_ = false;
            player_->Pause();
            BroadcastPlayback(wxl_party::PartyMessageType::Pause);
            status_ = "Playback paused.";
            break;
        case wxl_video_shared::UiCommand::Stop:
            if (!CanControlParty("stop")) break;
            player_->Stop();
            paused_ = false;
            stopped_ = true;
            lastPositionSeconds_ = 0.0;
            autoplayDeadline_ = 0;
            nextEpisode_ = {};
            BroadcastPlayback(wxl_party::PartyMessageType::Stop);
            if (!playingItem_.ratingKey.empty())
                plex_->ReportTimeline(selectedServer_, playingItem_, wxl_plex::TimelineState::Stopped, 0);
            status_ = "Playback stopped.";
            break;
        case wxl_video_shared::UiCommand::Seek:
            if (playingItem_.ratingKey.empty() || playingItem_.durationMs <= 0)
            {
                state_.error = "This Plex item has no seekable duration.";
                break;
            }
            {
                const int positionMs = wxl_mpv::ClampSeekMilliseconds(
                    command.value, playingItem_.durationMs);
                const std::string request = "seek:" + std::to_string(positionMs);
                if (!CanControlParty(request)) break;
                player_->Seek(positionMs / 1000.0);
                lastPositionSeconds_ = positionMs / 1000.0;
                BroadcastPlayback(wxl_party::PartyMessageType::Seek);
                status_ = "Seeking playback.";
            }
            break;
        case wxl_video_shared::UiCommand::NextPage:
            if (state_.pageSize <= 0 ||
                state_.pageOffset + state_.pageSize >= state_.pageTotalSize)
            {
                state_.error = "There is no next Plex page.";
                break;
            }
            LoadPage(state_.pageOffset + state_.pageSize);
            break;
        case wxl_video_shared::UiCommand::PreviousPage:
            if (state_.pageOffset <= 0)
            {
                state_.error = "There is no previous Plex page.";
                break;
            }
            LoadPage(std::max(0, state_.pageOffset -
                                  (state_.pageSize > 0 ? state_.pageSize : wxl_plex::kPlexPageSize)));
            break;
        case wxl_video_shared::UiCommand::OpenWatchlist:
            if (!state_.watchlistLoaded)
            {
                state_.error = "The Plex watchlist is still loading.";
                break;
            }
            if (state_.watchlist.empty())
            {
                state_.error = state_.watchlistError.empty()
                    ? "Your Plex watchlist is empty." : state_.watchlistError;
                break;
            }
            rootItems_ = state_.watchlist;
            state_.items = rootItems_;
            state_.pageOffset = state_.watchlistOffset;
            state_.pageSize = state_.watchlistSize;
            state_.pageTotalSize = state_.watchlistTotalSize;
            browseStack_.clear();
            browsingWatchlist_ = true;
            browsingHome_ = false;
            searchActive_ = false;
            state_.view = View::Library;
            status_ = "Plex watchlist.";
            break;
        case wxl_video_shared::UiCommand::NavigateBack:
            NavigateBack();
            break;
        case wxl_video_shared::UiCommand::SelectAudio:
            if (!ValidIndex(command.index, state_.item.streams.size()) ||
                state_.item.streams[static_cast<size_t>(command.index)].streamType != 2)
            {
                state_.error = "The selected audio track is no longer available.";
                break;
            }
            player_->SetAudioTrack(state_.item.streams[static_cast<size_t>(command.index)].id);
            status_ = "Audio track changed.";
            break;
        case wxl_video_shared::UiCommand::SelectSubtitle:
            if (!ValidIndex(command.index, state_.item.streams.size()) ||
                state_.item.streams[static_cast<size_t>(command.index)].streamType != 3)
            {
                state_.error = "The selected subtitle track is no longer available.";
                break;
            }
            player_->SetSubtitleTrack(state_.item.streams[static_cast<size_t>(command.index)].id);
            status_ = "Subtitle track changed.";
            break;
        case wxl_video_shared::UiCommand::OpenHomeSection:
            if (selectedServer_.uri.empty())
            {
                state_.error = "Choose a Plex server first.";
                break;
            }
            if (command.value < static_cast<LONG>(wxl_plex::PlexHomeKind::ContinueWatching) ||
                command.value > static_cast<LONG>(wxl_plex::PlexHomeKind::RecentlyWatched))
            {
                state_.error = "Invalid Plex home section.";
                break;
            }
            activeHomeKind_ = static_cast<wxl_plex::PlexHomeKind>(command.value);
            browsingHome_ = true;
            browsingWatchlist_ = false;
            searchActive_ = false;
            browseStack_.clear();
            status_ = std::string("Loading ") + HomeTitle(activeHomeKind_) + ".";
            plex_->LoadHome(selectedServer_, activeHomeKind_);
            break;
        case wxl_video_shared::UiCommand::SetPlaybackSpeed:
        {
            const double speed = wxl_mpv::ClampPlaybackSpeed(command.value / 1000.0);
            player_->SetPlaybackSpeed(speed);
            partySettings_.playbackSpeed = speed;
            SavePartySettings();
            if (sync_) sync_->SetBaseSpeed(speed);
            status_ = "Playback speed changed.";
            break;
        }
        case wxl_video_shared::UiCommand::SkipMarker:
            if (!ValidIndex(command.index, state_.item.markers.size()))
            {
                state_.error = "That Plex marker is no longer available.";
                break;
            }
            if (CanControlParty("seek:" + std::to_string(
                    state_.item.markers[static_cast<size_t>(command.index)].endMs)))
            {
                const int target = state_.item.markers[static_cast<size_t>(command.index)].endMs;
                player_->Seek(target / 1000.0);
                lastPositionSeconds_ = target / 1000.0;
                BroadcastPlayback(wxl_party::PartyMessageType::Seek);
                status_ = "Skipped " + state_.item.markers[static_cast<size_t>(command.index)].type + ".";
            }
            break;
        case wxl_video_shared::UiCommand::PlayNextEpisode:
            if (nextEpisode_.ratingKey.empty())
            {
                state_.error = "No next episode is ready.";
                break;
            }
            autoplayDeadline_ = 0;
            if (CanControlParty("next"))
            {
                const auto next = nextEpisode_;
                nextEpisode_ = {};
                StartPlayback(next, true);
            }
            break;
        case wxl_video_shared::UiCommand::CancelAutoplay:
            autoplayDeadline_ = 0;
            nextEpisode_ = {};
            status_ = "Autoplay cancelled.";
            break;
        case wxl_video_shared::UiCommand::CreateParty:
        case wxl_video_shared::UiCommand::CreatePublicParty:
            if (!party_ || !partySettings_.features.watchTogether)
            {
                state_.error = "Watch Together is disabled.";
                break;
            }
            if (static_cast<wxl_video_shared::UiCommand>(command.command) ==
                    wxl_video_shared::UiCommand::CreatePublicParty &&
                !partySettings_.features.publicRooms)
            {
                state_.error = "Public rooms are experimental and disabled until relay authentication and moderation are configured.";
                break;
            }
            party_->CreateRoom(static_cast<wxl_video_shared::UiCommand>(command.command) ==
                                       wxl_video_shared::UiCommand::CreatePublicParty
                                   ? wxl_party::RoomVisibility::Public
                                   : wxl_party::RoomVisibility::Private);
            state_.view = View::Party;
            status_ = "Creating watch party.";
            break;
        case wxl_video_shared::UiCommand::JoinParty:
            if (!party_ || !partySettings_.features.watchTogether) break;
            party_->JoinRoom(BoundedText(command.text, sizeof(command.text)));
            state_.view = View::Party;
            status_ = "Joining watch party.";
            break;
        case wxl_video_shared::UiCommand::LeaveParty:
            if (party_) party_->LeaveRoom();
            if (sync_) sync_->Reset();
            liveBroadcasting_ = false;
            liveWatching_ = false;
            pendingRemoteMedia_ = false;
            pendingControlRequest_.clear();
            pendingControlRequestId_.clear();
            pendingControlRequestEpoch_ = 0;
            pendingControlRequestSequence_ = 0;
            partySettings_.lastRoomCode.clear();
            SavePartySettings();
            status_ = "Left watch party.";
            break;
        case wxl_video_shared::UiCommand::RefreshPublicRooms:
            if (party_ && partySettings_.features.publicRooms)
                party_->RequestPublicRooms();
            else
                state_.error = "Public rooms are experimental and disabled by default.";
            break;
        case wxl_video_shared::UiCommand::JoinPublicRoom:
            if (!partySettings_.features.publicRooms)
            {
                state_.error = "Public rooms are experimental and disabled by default.";
                break;
            }
            if (!party_ || !ValidIndex(command.index, publicRooms_.size()))
            {
                state_.error = "That public room is no longer available.";
                break;
            }
            party_->JoinRoom(publicRooms_[static_cast<size_t>(command.index)].code);
            state_.view = View::Party;
            break;
        case wxl_video_shared::UiCommand::SetPartyPermissionMode:
            if (!party_ || command.value < static_cast<LONG>(wxl_party::PartyPermissionMode::HostOnly) ||
                command.value > static_cast<LONG>(wxl_party::PartyPermissionMode::RequestControl))
                break;
            partySettings_.permissionMode = static_cast<wxl_party::PartyPermissionMode>(command.value);
            SavePartySettings();
            {
                auto permissions = party_->Room().permissions;
                permissions.mode = partySettings_.permissionMode;
                if (sync_) sync_->SetPermissions(permissions);
                if (party_->Role() == wxl_party::PartyRole::Host) party_->SendPermissions(permissions);
            }
            break;
        case wxl_video_shared::UiCommand::SetPartyQueueMode:
            if (!party_ || command.value < static_cast<LONG>(wxl_party::PartyQueueMode::HostManaged) ||
                command.value > static_cast<LONG>(wxl_party::PartyQueueMode::EveryoneMayAdd))
                break;
            partySettings_.queueMode = static_cast<wxl_party::PartyQueueMode>(command.value);
            SavePartySettings();
            {
                auto permissions = party_->Room().permissions;
                permissions.queueMode = partySettings_.queueMode;
                if (sync_) sync_->SetPermissions(permissions);
                if (party_->Role() == wxl_party::PartyRole::Host) party_->SendPermissions(permissions);
            }
            break;
        case wxl_video_shared::UiCommand::SetPartySyncAudio:
        case wxl_video_shared::UiCommand::SetPartySyncSubtitles:
            if (!party_) break;
            if (static_cast<wxl_video_shared::UiCommand>(command.command) == wxl_video_shared::UiCommand::SetPartySyncAudio)
                partySettings_.syncAudio = command.value != 0;
            else
                partySettings_.syncSubtitles = command.value != 0;
            SavePartySettings();
            {
                auto permissions = party_->Room().permissions;
                permissions.syncAudio = partySettings_.syncAudio;
                permissions.syncSubtitles = partySettings_.syncSubtitles;
                if (sync_) sync_->SetPermissions(permissions);
                if (party_->Role() == wxl_party::PartyRole::Host) party_->SendPermissions(permissions);
            }
            break;
        case wxl_video_shared::UiCommand::StartReadyCheck:
            if (party_ && party_->Role() == wxl_party::PartyRole::Host && partySettings_.features.readyChecks)
            {
                if (playingItem_.ratingKey.empty())
                {
                    state_.error = "Load a Plex item before starting a ready check.";
                    break;
                }
                readyCountdownTriggered_ = false;
                party_->StartReadyCheck(0);
                status_ = "Ready check: waiting for everyone.";
            }
            break;
        case wxl_video_shared::UiCommand::SetReadyState:
            if (party_) party_->SetReady(command.value != 0);
            break;
        case wxl_video_shared::UiCommand::AddCurrentToQueue:
            if (party_ && partySettings_.features.sharedQueue)
            {
                auto room = party_->Room();
                const auto role = party_->Role();
                const bool mayAdd = role == wxl_party::PartyRole::Host ||
                    (role == wxl_party::PartyRole::Guest &&
                     room.permissions.queueMode == wxl_party::PartyQueueMode::EveryoneMayAdd);
                if (room.code.empty() || !mayAdd)
                {
                    state_.error = "Only the host may add to this watch-party queue.";
                    break;
                }
                const auto& source = state_.item.ratingKey.empty() ? playingItem_ : state_.item;
                if (source.ratingKey.empty())
                {
                    state_.error = "Select an item before adding it to the party queue.";
                    break;
                }
                wxl_party::PartyQueueItem item;
                item.media = MediaIdentityFor(selectedServer_, source);
                item.addedBy = room.memberId;
                item.order = room.queue.empty() ? 1 : room.queue.back().order + 1;
                room.queue.push_back(std::move(item));
                party_->SendQueue(room.queue);
                status_ = "Added to the watch-party queue.";
            }
            break;
        case wxl_video_shared::UiCommand::RemoveQueueItem:
            if (party_ && party_->Role() == wxl_party::PartyRole::Host)
            {
                auto room = party_->Room();
                if (!ValidIndex(command.index, room.queue.size())) break;
                room.queue.erase(room.queue.begin() + command.index);
                party_->SendQueue(room.queue);
            }
            break;
        case wxl_video_shared::UiCommand::MoveQueueItem:
            if (party_ && party_->Role() == wxl_party::PartyRole::Host)
            {
                auto room = party_->Room();
                if (!ValidIndex(command.index, room.queue.size()) ||
                    !ValidIndex(command.value, room.queue.size())) break;
                auto item = room.queue[static_cast<size_t>(command.index)];
                room.queue.erase(room.queue.begin() + command.index);
                room.queue.insert(room.queue.begin() + command.value, std::move(item));
                for (size_t index = 0; index < room.queue.size(); ++index) room.queue[index].order = index + 1;
                party_->SendQueue(room.queue);
            }
            break;
        case wxl_video_shared::UiCommand::PlayQueueItem:
            if (party_)
            {
                const auto room = party_->Room();
                if (!ValidIndex(command.index, room.queue.size())) break;
                if (!CanControlParty("queue:" + std::to_string(command.index))) break;
                pendingQueueIndex_ = command.index;
                plex_->ResolvePartyMedia(selectedServer_,
                    PlexHintFor(room.queue[static_cast<size_t>(command.index)].media),
                    room.queue[static_cast<size_t>(command.index)].media.serverIdentifier);
            }
            break;
        case wxl_video_shared::UiCommand::RespondControlRequest:
            if (party_ && party_->Role() == wxl_party::PartyRole::Host && !pendingControlRequestId_.empty())
            {
                const bool allow = command.value != 0;
                party_->SendControlResponse(pendingControlRequestId_, allow);
                if (allow)
                {
                    if (pendingControlRequest_ == "play")
                    {
                        paused_ = false; stopped_ = false; player_->Play();
                        BroadcastPlayback(wxl_party::PartyMessageType::Play);
                    }
                    else if (pendingControlRequest_ == "pause")
                    {
                        paused_ = true; stopped_ = false; player_->Pause();
                        BroadcastPlayback(wxl_party::PartyMessageType::Pause);
                    }
                    else if (pendingControlRequest_ == "stop")
                    {
                        player_->Stop(); stopped_ = true; paused_ = false; lastPositionSeconds_ = 0.0;
                        BroadcastPlayback(wxl_party::PartyMessageType::Stop);
                    }
                    else if (pendingControlRequest_.rfind("seek:", 0) == 0)
                    {
                        try
                        {
                            const int target = wxl_mpv::ClampSeekMilliseconds(
                                std::stoi(pendingControlRequest_.substr(5)), playingItem_.durationMs);
                            player_->Seek(target / 1000.0);
                            lastPositionSeconds_ = target / 1000.0;
                            BroadcastPlayback(wxl_party::PartyMessageType::Seek);
                        }
                        catch (...) {}
                    }
                    else if (pendingControlRequest_ == "next" && !nextEpisode_.ratingKey.empty())
                    {
                        autoplayDeadline_ = 0;
                        const auto next = nextEpisode_;
                        nextEpisode_ = {};
                        StartPlayback(next, true);
                    }
                    else if (pendingControlRequest_.rfind("queue:", 0) == 0)
                    {
                        try
                        {
                            const int queueIndex = std::stoi(pendingControlRequest_.substr(6));
                            const auto room = party_->Room();
                            if (ValidIndex(queueIndex, room.queue.size()))
                            {
                                pendingQueueIndex_ = queueIndex;
                                plex_->ResolvePartyMedia(
                                    selectedServer_,
                                    PlexHintFor(room.queue[static_cast<size_t>(queueIndex)].media),
                                    room.queue[static_cast<size_t>(queueIndex)].media.serverIdentifier);
                            }
                        }
                        catch (...) {}
                    }
                }
                pendingControlRequest_.clear();
                pendingControlRequestId_.clear();
                pendingControlRequestEpoch_ = 0;
                pendingControlRequestSequence_ = 0;
            }
            break;
        case wxl_video_shared::UiCommand::SetDisconnectPolicy:
            if (command.value >= static_cast<LONG>(wxl_party::DisconnectPolicy::KeepPlaying) &&
                command.value <= static_cast<LONG>(wxl_party::DisconnectPolicy::AskHost))
            {
                partySettings_.disconnectPolicy = static_cast<wxl_party::DisconnectPolicy>(command.value);
                SavePartySettings();
            }
            break;
        case wxl_video_shared::UiCommand::ShareCinemaSettings:
            if (party_ && party_->Role() == wxl_party::PartyRole::Host &&
                partySettings_.features.sharedCinema)
            {
                wxl_party::CinemaSettings cinema;
                if (!ParseCinemaSettings(BoundedText(command.text, sizeof(command.text)), cinema))
                {
                    state_.error = "Shared cinema settings were malformed.";
                    break;
                }
                party_->SendCinema(cinema);
                status_ = "Shared cinema layout updated.";
            }
            break;
        case wxl_video_shared::UiCommand::GoLive:
            if (!party_ || !partySettings_.features.liveBroadcast)
            {
                state_.error = "Live broadcast is disabled.";
                break;
            }
            if (party_->Role() != wxl_party::PartyRole::Host)
            {
                state_.error = "Only the watch-party host can go live.";
                break;
            }
            if (playingItem_.ratingKey.empty() || stopped_)
            {
                state_.error = "Play a Plex item before going live.";
                break;
            }
            party_->SendLiveStart();
            status_ = "Starting live broadcast.";
            break;
        case wxl_video_shared::UiCommand::StopLive:
            if (party_) party_->SendLiveStop();
            liveBroadcasting_ = false;
            status_ = "Stopping live broadcast.";
            break;
        case wxl_video_shared::UiCommand::WatchLive:
            if (!party_) break;
            party_->SendLiveWatch();
            liveWatching_ = true;
            player_->Stop();
            paused_ = false;
            stopped_ = true;
            lastPositionSeconds_ = 0.0;
            status_ = "Watching live broadcast.";
            break;
        case wxl_video_shared::UiCommand::StopWatchingLive:
            if (party_) party_->SendLiveUnwatch();
            liveWatching_ = false;
            status_ = "Stopped watching the live broadcast.";
            break;
        case wxl_video_shared::UiCommand::SignOut:
            player_->Stop();
            plex_->SignOut();
            selectedServer_ = {};
            selectedSection_ = {};
            playingItem_ = {};
            playback_ = {};
            state_.servers.clear();
            state_.sections.clear();
            state_.items.clear();
            state_.watchlist.clear();
            state_.watchlistError.clear();
            state_.watchlistLoaded = false;
            state_.item = {};
            rootItems_.clear();
            browseStack_.clear();
            browsingWatchlist_ = false;
            browsingHome_ = false;
            activeSearch_.clear();
            searchActive_ = false;
            paused_ = false;
            stopped_ = true;
            status_ = "Signing out of Plex.";
            break;
        case wxl_video_shared::UiCommand::SetView:
            if (command.value < static_cast<LONG>(View::Login) ||
                command.value > static_cast<LONG>(View::Settings))
            {
                state_.error = "Invalid Plex view.";
                break;
            }
            state_.view = static_cast<View>(command.value);
            status_ = DefaultStatus(state_.view);
            break;
        default:
            break;
        }
    }

    void NativeUi::SetLiveStats(uint32_t framesSent, uint32_t framesReceived, uint32_t fpsPermille,
                                bool audioActive, uint32_t audioFramesReceived)
    {
        liveFramesSent_ = framesSent;
        liveFramesReceived_ = framesReceived;
        liveFpsPermille_ = fpsPermille;
        liveAudioActive_ = audioActive;
        liveAudioFramesReceived_ = audioFramesReceived;
    }

    void NativeUi::PlayCurrentItem()
    {
        if (state_.item.ratingKey.empty())
        {
            state_.error = "Select a Plex item before playing.";
            return;
        }
        StartPlayback(state_.item, true,
                      std::max(0, state_.item.viewOffsetMs), false);
    }

    void NativeUi::PublishUiState(wxl_video_shared::UiSnapshot& snapshot) const
    {
        snapshot = {};
        snapshot.structBytes = sizeof(snapshot);
        snapshot.view = static_cast<wxl_video_shared::UiView>(state_.view);
        snapshot.watchlistCount = static_cast<uint32_t>(
            state_.watchlistTotalSize > 0
                ? state_.watchlistTotalSize
                : std::min(state_.watchlist.size(), wxl_video_shared::kUiMaxItems));
        snapshot.watchlistLoaded = state_.watchlistLoaded ? 1u : 0u;
        snapshot.browseDepth = static_cast<uint32_t>(std::min(
            browseStack_.size(), wxl_video_shared::kUiMaxItems));
        snapshot.browseRoot = browsingWatchlist_ ? 1u : 0u;
        snapshot.pageOffset = static_cast<uint32_t>(std::max(0, state_.pageOffset));
        snapshot.pageSize = static_cast<uint32_t>(std::max(0, state_.pageSize));
        snapshot.pageTotalSize = static_cast<uint32_t>(std::max(0, state_.pageTotalSize));
        snapshot.playbackDurationMs = static_cast<uint32_t>(
            std::max(0, playingItem_.durationMs));
        if (snapshot.playbackDurationMs > 0)
            snapshot.playbackPositionMs = static_cast<uint32_t>(std::clamp(
                lastPositionSeconds_ * 1000.0, 0.0,
                static_cast<double>(snapshot.playbackDurationMs)));
        snapshot.playbackPaused = paused_ ? 1u : 0u;
        snapshot.playbackSpeedPermille = static_cast<uint32_t>(std::lround(
            std::clamp(player_ ? player_->PlaybackSpeed() : 1.0, 0.25, 4.0) * 1000.0));
        snapshot.videoAspectRatio = static_cast<float>(player_ ? player_->VideoAspectRatio() : 16.0 / 9.0);
        if (autoplayDeadline_ > GetTickCount64())
            snapshot.autoplayRemainingMs = static_cast<uint32_t>(std::min<ULONGLONG>(
                UINT32_MAX, autoplayDeadline_ - GetTickCount64()));
        snapshot.nextEpisodeAvailable = nextEpisode_.ratingKey.empty() ? 0u : 1u;
        CopyText(snapshot.error, state_.error);
        CopyText(snapshot.status, status_.empty() ? DefaultStatus(state_.view) : status_);
        CopyText(snapshot.watchlistError, state_.watchlistError);
        if (!browseStack_.empty())
            CopyText(snapshot.browseTitle, browseStack_.back().title);
        else if (browsingWatchlist_)
            CopyText(snapshot.browseTitle, "Watchlist");
        else if (browsingHome_)
            CopyText(snapshot.browseTitle, HomeTitle(activeHomeKind_));
        else
            CopyText(snapshot.browseTitle, selectedSection_.title);

        auto copyItem = [](const wxl_plex::PlexItem& source,
                           wxl_video_shared::UiItemSnapshot& destination)
        {
            CopyText(destination.ratingKey, source.ratingKey);
            CopyText(destination.guid, source.guid);
            CopyText(destination.title, source.title);
            CopyText(destination.type, source.type);
            CopyText(destination.grandparentTitle, source.grandparentTitle);
            CopyText(destination.thumb, source.thumb);
            destination.viewOffsetMs = source.viewOffsetMs;
            destination.durationMs = source.durationMs;
            destination.viewed = source.viewed ? 1 : 0;
        };

        snapshot.serverCount = static_cast<uint32_t>(
            std::min(state_.servers.size(), wxl_video_shared::kUiMaxServers));
        for (uint32_t index = 0; index < snapshot.serverCount; ++index)
        {
            CopyText(snapshot.servers[index].name, state_.servers[index].name);
            CopyText(snapshot.servers[index].uri, state_.servers[index].uri);
        }

        snapshot.sectionCount = static_cast<uint32_t>(
            std::min(state_.sections.size(), wxl_video_shared::kUiMaxSections));
        for (uint32_t index = 0; index < snapshot.sectionCount; ++index)
        {
            CopyText(snapshot.sections[index].key, state_.sections[index].key);
            CopyText(snapshot.sections[index].title, state_.sections[index].title);
            CopyText(snapshot.sections[index].type, state_.sections[index].type);
        }

        snapshot.itemCount = static_cast<uint32_t>(
            std::min(state_.items.size(), wxl_video_shared::kUiMaxItems));
        for (uint32_t index = 0; index < snapshot.itemCount; ++index)
            copyItem(state_.items[index], snapshot.items[index]);
        copyItem(state_.item, snapshot.item);
        copyItem(playingItem_, snapshot.playingItem);
        copyItem(nextEpisode_, snapshot.nextEpisode);

        snapshot.markerCount = static_cast<uint32_t>(std::min(
            playingItem_.markers.size(), wxl_video_shared::kUiMaxMarkers));
        for (uint32_t index = 0; index < snapshot.markerCount; ++index)
        {
            CopyText(snapshot.markers[index].type, playingItem_.markers[index].type);
            snapshot.markers[index].startMs = playingItem_.markers[index].startMs;
            snapshot.markers[index].endMs = playingItem_.markers[index].endMs;
        }

        snapshot.streamCount = static_cast<uint32_t>(
            std::min(state_.item.streams.size(), wxl_video_shared::kUiMaxStreams));
        for (uint32_t index = 0; index < snapshot.streamCount; ++index)
        {
            const auto& stream = state_.item.streams[index];
            snapshot.streams[index].id = stream.id;
            snapshot.streams[index].streamType = stream.streamType;
            CopyText(snapshot.streams[index].language, stream.language);
            CopyText(snapshot.streams[index].title, stream.title);
        }

        if (party_)
        {
            const auto room = party_->Room();
            auto permissions = room.permissions;
            if (room.code.empty())
            {
                permissions.mode = partySettings_.permissionMode;
                permissions.syncAudio = partySettings_.syncAudio;
                permissions.syncSubtitles = partySettings_.syncSubtitles;
                permissions.queueMode = partySettings_.queueMode;
            }
            snapshot.partyConnectionState = static_cast<uint32_t>(party_->ConnectionState());
            snapshot.partyRole = static_cast<uint32_t>(party_->Role());
            snapshot.partyPermissionMode = static_cast<uint32_t>(permissions.mode);
            snapshot.partyQueueMode = static_cast<uint32_t>(permissions.queueMode);
            snapshot.partySyncAudio = permissions.syncAudio ? 1u : 0u;
            snapshot.partySyncSubtitles = permissions.syncSubtitles ? 1u : 0u;
            snapshot.partyDisconnectPolicy = static_cast<uint32_t>(partySettings_.disconnectPolicy);
            snapshot.partyPublicRoomsEnabled = partySettings_.features.publicRooms ? 1u : 0u;
            snapshot.partyReadyCheckActive = room.readyCheckActive ? 1u : 0u;
            snapshot.partyReadyStartAtMs = room.readyStartAtMs;
            snapshot.partyRelayRttMs = party_->RelayRttMs();
            snapshot.partyClockOffsetMs = party_->ClockOffsetMs();
            snapshot.partyReconnectCount = party_->ReconnectCount();
            snapshot.partyRoomEpoch = room.roomEpoch;
            snapshot.partySequence = room.sequence;
            snapshot.partyLiveActive = room.live.active ? 1u : 0u;
            snapshot.partyLiveBroadcasting = liveBroadcasting_ ? 1u : 0u;
            snapshot.partyLiveWatching = liveWatching_ ? 1u : 0u;
            snapshot.partyLiveViewerCount = room.live.watcherCount;
            snapshot.partyLiveFramesSent = liveFramesSent_;
            snapshot.partyLiveFramesReceived = liveFramesReceived_;
            snapshot.partyLiveFpsPermille = liveFpsPermille_;
            snapshot.partyLiveAudioActive = liveAudioActive_ ? 1u : 0u;
            snapshot.partyLiveAudioFramesReceived = liveAudioFramesReceived_;
            CopyText(snapshot.partyRoomCode, room.code);
            CopyText(snapshot.partyError, party_->LastError());
            CopyText(snapshot.partyMediaMatch, partyMediaMatch_);
            CopyText(snapshot.controlRequest, pendingControlRequest_);
            CopyText(snapshot.controlRequestId, pendingControlRequestId_);
            for (const auto& member : room.members)
            {
                if (member.id == room.hostMemberId)
                {
                    CopyText(snapshot.partyHostName, member.displayName);
                    break;
                }
            }

            snapshot.partyMemberCount = static_cast<uint32_t>(std::min(
                room.members.size(), wxl_video_shared::kUiMaxPartyMembers));
            for (uint32_t index = 0; index < snapshot.partyMemberCount; ++index)
            {
                const auto& member = room.members[index];
                CopyText(snapshot.partyMembers[index].id, member.id);
                CopyText(snapshot.partyMembers[index].name, member.displayName);
                snapshot.partyMembers[index].role = static_cast<LONG>(member.role);
                snapshot.partyMembers[index].connected = member.connected ? 1 : 0;
                snapshot.partyMembers[index].ready = member.ready ? 1 : 0;
            }

            snapshot.partyQueueCount = static_cast<uint32_t>(std::min(
                room.queue.size(), wxl_video_shared::kUiMaxPartyQueue));
            for (uint32_t index = 0; index < snapshot.partyQueueCount; ++index)
            {
                const auto& item = room.queue[index];
                CopyText(snapshot.partyQueue[index].title, item.media.title);
                CopyText(snapshot.partyQueue[index].type, item.media.type);
                const bool exactLocal = !selectedServer_.machineIdentifier.empty() &&
                    item.media.serverIdentifier == selectedServer_.machineIdentifier &&
                    !item.media.ratingKey.empty();
                snapshot.partyQueue[index].resolved = item.resolvedLocally || exactLocal ? 1 : 0;
                snapshot.partyQueue[index].order = item.order;
            }

            const auto& cinema = room.cinema;
            CopyText(snapshot.sharedCinema.presetName, cinema.presetName);
            snapshot.sharedCinema.width = cinema.width;
            snapshot.sharedCinema.heightOffset = cinema.heightOffset;
            snapshot.sharedCinema.rotationDegrees = cinema.rotationDegrees;
            snapshot.sharedCinema.pinned = cinema.pinned ? 1 : 0;
            snapshot.sharedCinema.pinnedX = cinema.pinnedX;
            snapshot.sharedCinema.pinnedY = cinema.pinnedY;
            snapshot.sharedCinema.pinnedWidth = cinema.pinnedWidth;
            snapshot.sharedCinema.depthOcclusion = cinema.depthOcclusion ? 1 : 0;
            snapshot.sharedCinema.depthOffset = cinema.depthOffset;
            snapshot.sharedCinema.distanceAudio = cinema.distanceAudio ? 1 : 0;
            snapshot.sharedCinema.audioRange = cinema.audioRange;
            snapshot.sharedCinema.aspectRatioMode = cinema.aspectRatioMode;
            snapshot.sharedCinema.fitMode = cinema.fitMode;
            snapshot.sharedCinema.frameStyle = cinema.frameStyle;
            snapshot.sharedCinema.ambientLighting = cinema.ambientLighting ? 1 : 0;
        }

        if (sync_)
        {
            snapshot.partyDriftMs = sync_->LastDriftMs();
            CopyText(snapshot.partyCorrection, wxl_party::ToString(sync_->LastCorrection()));
        }

        snapshot.publicRoomCount = static_cast<uint32_t>(std::min(
            publicRooms_.size(), wxl_video_shared::kUiMaxPublicRooms));
        for (uint32_t index = 0; index < snapshot.publicRoomCount; ++index)
        {
            CopyText(snapshot.publicRooms[index].code, publicRooms_[index].code);
            CopyText(snapshot.publicRooms[index].title, publicRooms_[index].title);
            CopyText(snapshot.publicRooms[index].hostName, publicRooms_[index].hostName);
            snapshot.publicRooms[index].memberCount = static_cast<LONG>(publicRooms_[index].memberCount);
        }
    }

    void NativeUi::HandleMpvEvents(const std::vector<wxl_mpv::MpvEvent>& events)
    {
        for (const auto& event : events)
        {
            if (event.kind == wxl_mpv::MpvEvent::Kind::PositionChanged)
            {
                lastPositionSeconds_ = std::max(0.0, event.positionSeconds);
                if (playingItem_.ratingKey.empty()) continue;
                const DWORD now = GetTickCount();
                if (now - lastTimelineAt_ >= 5000)
                {
                    plex_->ReportTimeline(selectedServer_, playingItem_,
                                          paused_ ? wxl_plex::TimelineState::Paused
                                                  : wxl_plex::TimelineState::Playing,
                                          static_cast<int>(std::max(0.0, event.positionSeconds) * 1000.0));
                    lastTimelineAt_ = now;
                }
                if (party_ && party_->Role() == wxl_party::PartyRole::Host &&
                    !party_->Room().code.empty() && GetTickCount64() - lastPartyStateAt_ >= 2000)
                    BroadcastPlayback(wxl_party::PartyMessageType::PlaybackState);
            }
            else if (event.kind == wxl_mpv::MpvEvent::Kind::Ended &&
                     !playingItem_.ratingKey.empty())
            {
                if (event.endReason == MPV_END_FILE_REASON_STOP ||
                    event.endReason == MPV_END_FILE_REASON_QUIT)
                {
                    status_ = "Playback stopped.";
                    continue;
                }
                const double position = event.positionSeconds > 0.0
                    ? event.positionSeconds : lastPositionSeconds_;
                const bool endedEarly = wxl_mpv::IsPrematureEnd(
                    event.endReason, playingItem_.durationMs, position);
                if (endedEarly && !playback_.transcoded)
                {
                    playback_ = wxl_plex::BuildTranscodePlayback(
                        playingItem_, selectedServer_, "azerothplex-" + playingItem_.ratingKey);
                    if (!playback_.uri.empty() && player_->Load(playback_))
                    {
                        player_->Play();
                        state_.error = "Direct stream ended early; trying Plex transcoding.";
                        status_ = state_.error;
                        lastPositionSeconds_ = 0.0;
                        continue;
                    }
                }
                if (endedEarly)
                {
                    state_.error = "Plex playback ended before the media was complete.";
                    status_ = state_.error;
                    continue;
                }
                plex_->ReportTimeline(selectedServer_, playingItem_, wxl_plex::TimelineState::Watched,
                                      playingItem_.durationMs);
                state_.view = View::Details;
                paused_ = false;
                stopped_ = true;
                status_ = "Playback complete.";
                AdvanceQueueOrAutoplay();
            }
            else if (event.kind == wxl_mpv::MpvEvent::Kind::Error)
            {
                if (!playback_.transcoded && !playingItem_.ratingKey.empty())
                {
                    playback_ = wxl_plex::BuildTranscodePlayback(
                        playingItem_, selectedServer_, "azerothplex-" + playingItem_.ratingKey);
                    if (!playback_.uri.empty() && player_->Load(playback_))
                    {
                        player_->Play();
                        state_.error = "Direct play failed; trying Plex transcoding.";
                        status_ = state_.error;
                        continue;
                    }
                }
                state_.error = event.message.empty() ? "Plex playback failed." : event.message;
                status_ = state_.error;
            }
        }
    }

    void NativeUi::DrawLogin()
    {
        ImGui::TextUnformatted("Sign in to Plex");
        if (ImGui::Button("Open Plex sign-in")) plex_->StartLogin();
        if (!state_.error.empty()) ImGui::TextWrapped("%s", state_.error.c_str());
    }

    void NativeUi::DrawServers()
    {
        ImGui::TextUnformatted("Choose a Plex server");
        for (const auto& server : state_.servers)
        {
            if (ImGui::Button(server.name.c_str()))
            {
                selectedServer_ = server;
                plex_->LoadSections(selectedServer_);
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(server.uri.c_str());
        }
    }

    void NativeUi::DrawHome()
    {
        ImGui::Text("Plex libraries (%zu)", state_.sections.size());
        if (ImGui::Button("Choose another server"))
            state_.view = View::Servers;
        ImGui::SameLine();
        if (ImGui::Button("Search")) state_.view = View::Search;
        ImGui::SameLine();
        if (ImGui::Button("Watch Together")) state_.view = View::Party;
        ImGui::SameLine();
        if (ImGui::Button("Settings")) state_.view = View::Settings;
        ImGui::SameLine();
        if (ImGui::Button("Sign out")) plex_->SignOut();
        if (ImGui::Button("Continue Watching")) plex_->LoadHome(selectedServer_, wxl_plex::PlexHomeKind::ContinueWatching);
        ImGui::SameLine();
        if (ImGui::Button("On Deck")) plex_->LoadHome(selectedServer_, wxl_plex::PlexHomeKind::OnDeck);
        ImGui::SameLine();
        if (ImGui::Button("Recently Added")) plex_->LoadHome(selectedServer_, wxl_plex::PlexHomeKind::RecentlyAddedMovies);
        for (const auto& section : state_.sections)
        {
            if (ImGui::Button(section.title.c_str()))
            {
                selectedSection_ = section;
                plex_->LoadItems(selectedServer_, selectedSection_);
            }
        }
    }

    void NativeUi::DrawLibrary()
    {
        if (ImGui::Button("Back to libraries")) state_.view = View::Home;
        ImGui::Separator();
        for (const auto& item : state_.items)
        {
            const std::string label = item.title.empty() ? item.ratingKey : item.title;
            if (ImGui::Button(label.c_str())) plex_->LoadMetadata(selectedServer_, item);
        }
    }

    void NativeUi::DrawSearch()
    {
        ImGui::InputText("Search", search_, sizeof(search_));
        if (ImGui::Button("Search")) plex_->Search(selectedServer_, search_);
        ImGui::SameLine();
        if (ImGui::Button("Back")) state_.view = View::Home;
    }

    void NativeUi::DrawDetails()
    {
        if (ImGui::Button("Back")) state_.view = View::Library;
        ImGui::Text("%s", state_.item.title.c_str());
        ImGui::Text("Type: %s", state_.item.type.c_str());
        if (state_.item.viewOffsetMs > 0)
            ImGui::Text("Resume at %d ms", state_.item.viewOffsetMs);
        if (ImGui::Button("Play"))
        {
            PlayCurrentItem();
        }
        for (const auto& stream : state_.item.streams)
        {
            if (stream.streamType == 2 && ImGui::Button(("Audio: " + stream.title).c_str()))
                player_->SetAudioTrack(stream.id);
            if (stream.streamType == 3 && ImGui::Button(("Subtitle: " + stream.title).c_str()))
                player_->SetSubtitleTrack(stream.id);
        }
    }

    void NativeUi::DrawPlayer()
    {
        if (videoView_)
            ImGui::Image(reinterpret_cast<ImTextureID>(videoView_), ImVec2(640.0f, 360.0f));
        if (ImGui::Button("Play") && CanControlParty("play"))
        {
            paused_ = false; stopped_ = false; player_->Play();
            BroadcastPlayback(wxl_party::PartyMessageType::Play);
        }
        ImGui::SameLine();
        if (ImGui::Button("Pause") && CanControlParty("pause"))
        {
            paused_ = true; stopped_ = false; player_->Pause();
            BroadcastPlayback(wxl_party::PartyMessageType::Pause);
        }
        ImGui::SameLine();
        if (ImGui::Button("Stop") && CanControlParty("stop"))
        {
            player_->Stop(); stopped_ = true; paused_ = false;
            autoplayDeadline_ = 0;
            nextEpisode_ = {};
            BroadcastPlayback(wxl_party::PartyMessageType::Stop);
            if (!playingItem_.ratingKey.empty())
                plex_->ReportTimeline(selectedServer_, playingItem_, wxl_plex::TimelineState::Stopped, 0);
        }
        ImGui::SameLine();
        if (ImGui::Button("Watch Together")) state_.view = View::Party;
        ImGui::Text("Speed %.2fx", player_->PlaybackSpeed());
        if (ImGui::Button("Hide helper")) Hide();
        if (!state_.error.empty()) ImGui::TextWrapped("%s", state_.error.c_str());
    }

    void NativeUi::DrawParty()
    {
        if (!party_)
        {
            ImGui::TextUnformatted("Watch Together is unavailable.");
            return;
        }
        const auto room = party_->Room();
        ImGui::Text("Connection: %s", wxl_party::ToString(party_->ConnectionState()));
        if (room.code.empty())
        {
            static char roomCode[32]{};
            if (ImGui::Button("Create private party")) party_->CreateRoom();
            if (partySettings_.features.publicRooms)
            {
                ImGui::SameLine();
                if (ImGui::Button("Create public party")) party_->CreateRoom(wxl_party::RoomVisibility::Public);
            }
            ImGui::InputText("Room code", roomCode, sizeof(roomCode));
            if (ImGui::Button("Join")) party_->JoinRoom(roomCode);
            if (partySettings_.features.publicRooms)
            {
                ImGui::SameLine();
                if (ImGui::Button("Browse public rooms")) party_->RequestPublicRooms();
            }
        }
        else
        {
            ImGui::Text("Room %s | Host %s", room.code.c_str(), room.hostMemberId.c_str());
            ImGui::Text("RTT %lld ms | drift %lld ms | %s",
                        static_cast<long long>(party_->RelayRttMs()),
                        static_cast<long long>(sync_ ? sync_->LastDriftMs() : 0),
                        sync_ ? wxl_party::ToString(sync_->LastCorrection()) : "None");
            for (const auto& member : room.members)
                ImGui::BulletText("%s%s%s", member.displayName.c_str(),
                                  member.role == wxl_party::PartyRole::Host ? " [Host]" : "",
                                  member.ready ? " [Ready]" : "");
            if (party_->Role() == wxl_party::PartyRole::Host && ImGui::Button("Ready check"))
            {
                if (playingItem_.ratingKey.empty())
                    state_.error = "Load a Plex item before starting a ready check.";
                else
                {
                    readyCountdownTriggered_ = false;
                    party_->StartReadyCheck(0);
                    status_ = "Ready check: waiting for everyone.";
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Ready")) party_->SetReady(true);
            if (partySettings_.features.liveBroadcast)
            {
                if (room.live.active)
                {
                    ImGui::Text("Live broadcast | %u viewer%s", room.live.watcherCount,
                                room.live.watcherCount == 1 ? "" : "s");
                    if (liveWatching_)
                    {
                        if (ImGui::Button("Leave live"))
                        {
                            party_->SendLiveUnwatch();
                            liveWatching_ = false;
                        }
                    }
                    else if (ImGui::Button("Watch live"))
                    {
                        party_->SendLiveWatch();
                        liveWatching_ = true;
                        player_->Stop();
                        paused_ = false;
                        stopped_ = true;
                        lastPositionSeconds_ = 0.0;
                    }
                    if (liveBroadcasting_)
                    {
                        ImGui::SameLine();
                        if (ImGui::Button("Stop live")) party_->SendLiveStop();
                    }
                }
                else if (party_->Role() == wxl_party::PartyRole::Host && ImGui::Button("Go live"))
                {
                    if (playingItem_.ratingKey.empty() || stopped_)
                        state_.error = "Play a Plex item before going live.";
                    else
                    {
                        party_->SendLiveStart();
                        status_ = "Starting live broadcast.";
                    }
                }
            }
            if (ImGui::Button("Queue")) state_.view = View::Queue;
            ImGui::SameLine();
            if (ImGui::Button("Leave"))
            {
                party_->LeaveRoom();
                liveBroadcasting_ = false;
                liveWatching_ = false;
            }
        }
        if (ImGui::Button("Back")) state_.view = View::Home;
    }

    void NativeUi::DrawQueue()
    {
        if (!party_)
        {
            state_.view = View::Party;
            return;
        }
        const auto room = party_->Room();
        ImGui::Text("Queue (%zu)", room.queue.size());
        for (size_t index = 0; index < room.queue.size(); ++index)
        {
            ImGui::PushID(static_cast<int>(index));
            ImGui::Text("%zu. %s", index + 1, room.queue[index].media.title.c_str());
            ImGui::SameLine();
            if (ImGui::Button("Play") &&
                CanControlParty("queue:" + std::to_string(index)))
            {
                pendingQueueIndex_ = static_cast<int>(index);
                plex_->ResolvePartyMedia(selectedServer_, PlexHintFor(room.queue[index].media),
                                         room.queue[index].media.serverIdentifier);
            }
            ImGui::PopID();
        }
        if (ImGui::Button("Back")) state_.view = View::Party;
    }

    void NativeUi::DrawSettings()
    {
        int policy = static_cast<int>(partySettings_.disconnectPolicy);
        ImGui::Text("Relay: %s", partySettings_.relayUrl.c_str());
        ImGui::Text("Display name: %s", partySettings_.displayName.c_str());
        ImGui::Text("Disconnect policy: %d", policy);
        ImGui::Text("Drift thresholds: %d / %d ms",
                    partySettings_.noCorrectionDriftMs, partySettings_.hardSeekDriftMs);
        if (ImGui::Button("Back")) state_.view = View::Home;
    }

    void NativeUi::Draw()
    {
        if (!initialized_ || !visible_) return;
        PumpPlexEvents();
        PumpPartyEvents();

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        ImGui::Begin("AzerothPlex", nullptr, ImGuiWindowFlags_NoCollapse);
        switch (state_.view)
        {
        case View::Login: DrawLogin(); break;
        case View::Servers: DrawServers(); break;
        case View::Home: DrawHome(); break;
        case View::Library: DrawLibrary(); break;
        case View::Search: DrawSearch(); break;
        case View::Details: DrawDetails(); break;
        case View::Player: DrawPlayer(); break;
        case View::Party: DrawParty(); break;
        case View::Queue: DrawQueue(); break;
        case View::Settings: DrawSettings(); break;
        }
        ImGui::End();
        ImGui::Render();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }

    void NativeUi::Hide()
    {
        visible_ = false;
        if (window_) ShowWindow(window_, SW_HIDE);
    }

    void NativeUi::Shutdown()
    {
        if (videoView_) videoView_->Release();
        if (videoTexture_) videoTexture_->Release();
        videoView_ = nullptr;
        videoTexture_ = nullptr;
        if (initialized_)
        {
            ImGui_ImplDX11_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
        }
        initialized_ = false;
        window_ = nullptr;
        device_ = nullptr;
        context_ = nullptr;
        plex_ = nullptr;
        player_ = nullptr;
        frames_ = nullptr;
        party_ = nullptr;
        sync_ = nullptr;
        sessionStore_ = nullptr;
    }
}
