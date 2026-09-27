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
        std::string mediaUri;
        std::string accessToken;
        std::string machineIdentifier;
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
        std::string codec;
        bool forced = false;
        bool hearingImpaired = false;
        bool selected = false;
    };

    struct PlexMarker
    {
        std::string type;
        int startMs = 0;
        int endMs = 0;
    };

    struct PlexItem
    {
        std::string ratingKey;
        std::string guid;
        std::string imdbId;
        std::string tmdbId;
        std::string tvdbId;
        std::string title;
        std::string type;
        std::string grandparentTitle;
        std::string parentRatingKey;
        std::string grandparentRatingKey;
        std::string thumb;
        std::string partKey;
        std::vector<PlexStream> streams;
        std::vector<PlexMarker> markers;
        int year = 0;
        int parentIndex = 0;
        int index = 0;
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
