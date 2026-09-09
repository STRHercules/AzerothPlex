#pragma once

#include "PlexTypes.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace wxl_plex
{
    bool ParsePinJson(std::string_view json, PinAuth& result);
    std::vector<PlexServer> ParseResourcesXml(std::string_view xml);
    std::vector<PlexSection> ParseSectionsXml(std::string_view xml);
    std::vector<PlexItem> ParseItemsXml(std::string_view xml);
    PlexItem ParseMetadataXml(std::string_view xml);
    PlexPlayback BuildDirectPlayback(const PlexItem& item, const PlexServer& server);
    PlexPlayback BuildTranscodePlayback(const PlexItem& item, const PlexServer& server,
                                        std::string_view sessionId);
    std::string BuildTimelineRequest(const PlexItem& item, TimelineState state, int positionMs);

    struct PlexEvent
    {
        enum class Kind
        {
            LoginRequired,
            LoginPending,
            LoginSucceeded,
            Servers,
            Sections,
            Items,
            Metadata,
            Error,
        };

        Kind kind = Kind::Error;
        std::string message;
        std::vector<PlexServer> servers;
        std::vector<PlexSection> sections;
        std::vector<PlexItem> items;
        PlexItem item;
    };

    class PlexClient final
    {
    public:
        explicit PlexClient(std::filesystem::path dataDirectory);
        ~PlexClient();

        void StartLogin();
        void LoadResources();
        void LoadSections(const PlexServer& server);
        void LoadItems(const PlexServer& server, const PlexSection& section);
        void Search(const PlexServer& server, std::string query);
        void LoadMetadata(const PlexServer& server, const PlexItem& item);
        void ReportTimeline(const PlexServer& server, const PlexItem& item,
                            TimelineState state, int positionMs);
        void SignOut();
        bool PopEvent(PlexEvent& event);

    private:
        using Job = std::function<void(std::stop_token)>;

        void Enqueue(Job job);
        void Emit(PlexEvent event);
        void PollLogin(std::stop_token stop, int pinId);

        std::filesystem::path dataDirectory_;
        std::string token_;
        std::string clientId_ = "AzerothPlex-native-client";
        std::mutex jobsMutex_;
        std::condition_variable_any jobsChanged_;
        std::deque<Job> jobs_;
        std::mutex eventsMutex_;
        std::deque<PlexEvent> events_;
        std::jthread worker_;
    };
}
