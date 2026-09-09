#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wxl_plex
{
    struct PinAuth
    {
        int id = 0;
        std::string code;
        std::string authToken;
    };

    struct PlexServer
    {
        std::string name;
        std::string uri;
        std::string accessToken;
    };

    struct PlexSection
    {
        std::string key;
        std::string title;
        std::string type;
    };

    struct PlexStream
    {
        int id = 0;
        int streamType = 0;
        std::string language;
        std::string title;
    };

    struct PlexItem
    {
        std::string ratingKey;
        std::string title;
        std::string type;
        std::string grandparentTitle;
        std::string thumb;
        std::string partKey;
        std::vector<PlexStream> streams;
        int viewOffsetMs = 0;
        int durationMs = 0;
        bool viewed = false;
    };

    struct PlexPlayback
    {
        std::string uri;
        std::string sessionId;
        bool transcoded = false;
    };

    enum class TimelineState
    {
        Playing,
        Paused,
        Stopped,
        Watched,
    };
}
