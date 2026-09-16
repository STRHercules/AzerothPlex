#include "NativeUi.hpp"

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

#include <algorithm>
#include <cmath>
#include <cstring>
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
            }
            return "Plex is ready.";
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
        case wxl_plex::PlexEvent::Kind::Metadata:
            item = event.item;
            view = View::Details;
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

            state_.Apply(event);
            if (event.kind == wxl_plex::PlexEvent::Kind::Items)
            {
                rootItems_ = state_.items;
                browseStack_.clear();
                browsingWatchlist_ = false;
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
            status_ = DefaultStatus(state_.view);
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
            activeSearch_ = query;
            searchActive_ = true;
            status_ = "Searching Plex.";
            plex_->Search(selectedServer_, query, 0, wxl_plex::kPlexPageSize);
            break;
        }
        case wxl_video_shared::UiCommand::Play:
            PlayCurrentItem();
            break;
        case wxl_video_shared::UiCommand::Pause:
            paused_ = true;
            player_->Pause();
            status_ = "Playback paused.";
            break;
        case wxl_video_shared::UiCommand::Stop:
            player_->Stop();
            paused_ = false;
            lastPositionSeconds_ = 0.0;
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
                player_->Seek(positionMs / 1000.0);
                lastPositionSeconds_ = positionMs / 1000.0;
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
            activeSearch_.clear();
            searchActive_ = false;
            paused_ = false;
            status_ = "Signing out of Plex.";
            break;
        case wxl_video_shared::UiCommand::SetView:
            if (command.value < static_cast<LONG>(View::Login) ||
                command.value > static_cast<LONG>(View::Player))
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

    void NativeUi::PlayCurrentItem()
    {
        if (state_.item.ratingKey.empty())
        {
            state_.error = "Select a Plex item before playing.";
            return;
        }
        const auto playback = wxl_plex::BuildDirectPlayback(state_.item, selectedServer_);
        if (!playback.uri.empty() && player_->Load(playback))
        {
            playingItem_ = state_.item;
            playback_ = playback;
            paused_ = false;
            lastTimelineAt_ = 0;
            lastPositionSeconds_ = 0.0;
            player_->Play();
            state_.view = View::Player;
            status_ = "Playing " + playingItem_.title + ".";
        }
        else
        {
            state_.error = "This Plex item has no direct playable part.";
        }
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
        CopyText(snapshot.error, state_.error);
        CopyText(snapshot.status, status_.empty() ? DefaultStatus(state_.view) : status_);
        CopyText(snapshot.watchlistError, state_.watchlistError);
        if (!browseStack_.empty())
            CopyText(snapshot.browseTitle, browseStack_.back().title);
        else if (browsingWatchlist_)
            CopyText(snapshot.browseTitle, "Watchlist");
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
                status_ = "Playback complete.";
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
        if (ImGui::Button("Sign out")) plex_->SignOut();
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
        if (ImGui::Button("Play")) { paused_ = false; player_->Play(); }
        ImGui::SameLine();
        if (ImGui::Button("Pause")) { paused_ = true; player_->Pause(); }
        ImGui::SameLine();
        if (ImGui::Button("Stop"))
        {
            player_->Stop();
            if (!playingItem_.ratingKey.empty())
                plex_->ReportTimeline(selectedServer_, playingItem_, wxl_plex::TimelineState::Stopped, 0);
        }
        if (ImGui::Button("Hide helper")) Hide();
        if (!state_.error.empty()) ImGui::TextWrapped("%s", state_.error.c_str());
    }

    void NativeUi::Draw()
    {
        if (!initialized_ || !visible_) return;
        PumpPlexEvents();

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
    }
}
