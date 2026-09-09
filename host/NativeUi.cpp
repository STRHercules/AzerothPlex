#include "NativeUi.hpp"

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace wxl_ui
{
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
            break;
        case wxl_plex::PlexEvent::Kind::Metadata:
            item = event.item;
            view = View::Details;
            break;
        case wxl_plex::PlexEvent::Kind::Error:
            error = event.message;
            view = View::Player;
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

    void NativeUi::HandleMpvEvents(const std::vector<wxl_mpv::MpvEvent>& events)
    {
        for (const auto& event : events)
        {
            if (event.kind == wxl_mpv::MpvEvent::Kind::PositionChanged &&
                !playingItem_.ratingKey.empty())
            {
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
                plex_->ReportTimeline(selectedServer_, playingItem_, wxl_plex::TimelineState::Watched,
                                      playingItem_.durationMs);
                state_.view = View::Details;
                paused_ = false;
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
                        continue;
                    }
                }
                state_.error = event.message.empty() ? "Plex playback failed." : event.message;
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
            const auto playback = wxl_plex::BuildDirectPlayback(state_.item, selectedServer_);
            if (!playback.uri.empty() && player_->Load(playback))
            {
                playingItem_ = state_.item;
                playback_ = playback;
                paused_ = false;
                lastTimelineAt_ = 0;
                player_->Play();
                state_.view = View::Player;
            }
            else state_.error = "This Plex item has no direct playable part.";
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
        wxl_plex::PlexEvent event;
        while (plex_->PopEvent(event)) state_.Apply(event);

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

    void NativeUi::Show()
    {
        visible_ = true;
        if (window_) ShowWindow(window_, SW_SHOW);
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
