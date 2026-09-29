#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace wxl_party
{
    constexpr int kPartyProtocolVersion = 1;
    constexpr size_t kMaxPartyMessageBytes = 64 * 1024;
    constexpr size_t kMaxPartyMembers = 32;
    constexpr size_t kMaxPartyQueueItems = 64;
    constexpr int64_t kNoCorrectionDriftMs = 250;
    constexpr int64_t kHardSeekDriftMs = 1000;

    enum class PartyConnectionState
    {
        Disconnected,
        Connecting,
        Connected,
        Reconnecting,
        Failed,
    };

    enum class PartyRole
    {
        None,
        Host,
        Guest,
    };

    enum class PartyPermissionMode
    {
        HostOnly,
        SharedControl,
        RequestControl,
    };

    enum class PartyQueueMode
    {
        HostManaged,
        EveryoneMayAdd,
    };

    enum class DisconnectPolicy
    {
        KeepPlaying,
        PauseFor60Seconds,
        PauseUntilReconnect,
        AskHost,
    };

    enum class RoomVisibility
    {
        Private,
        Public,
    };

    enum class SyncCorrection
    {
        None,
        SlowDown,
        SpeedUp,
        HardSeek,
    };

    enum class MediaMatchQuality
    {
        None,
        Fallback,
        EpisodeIdentity,
        ExternalGuid,
        PlexGuid,
        ExactServerItem,
    };

    enum class PartyMessageType
    {
        Unknown,
        Hello,
        RoomCreate,
        RoomCreated,
        RoomJoin,
        RoomJoined,
        RoomLeave,
        MemberJoined,
        MemberLeft,
        RoomState,
        MediaChanged,
        PlaybackState,
        Play,
        Pause,
        Seek,
        Stop,
        HostChanged,
        Ping,
        Pong,
        Error,
        ControlRequest,
        ControlResponse,
        Permissions,
        QueueUpdate,
        ReadyCheck,
        ReadyState,
        ScreenPreset,
        PublicRoomsRequest,
        PublicRooms,
    };

    struct MediaIdentity
    {
        std::string serverIdentifier;
        std::string ratingKey;
        std::string plexGuid;
        std::string imdbId;
        std::string tmdbId;
        std::string tvdbId;
        std::string type;
        std::string title;
        std::string grandparentTitle;
        int year = 0;
        int season = 0;
        int episode = 0;
        int64_t durationMs = 0;
    };

    struct StreamIdentity
    {
        std::string language;
        std::string title;
        std::string codec;
        bool forced = false;
        bool hearingImpaired = false;
        bool valid = false;
    };

    struct PartyPlaybackState
    {
        MediaIdentity media;
        StreamIdentity audio;
        StreamIdentity subtitle;
        bool playing = false;
        bool paused = false;
        bool stopped = true;
        int64_t positionMs = 0;
        int64_t durationMs = 0;
        int64_t authoritativeTimestampMs = 0;
        uint64_t roomEpoch = 0;
        uint64_t sequence = 0;
    };

    struct PartyMember
    {
        std::string id;
        std::string displayName;
        PartyRole role = PartyRole::Guest;
        bool connected = true;
        bool ready = false;
        uint64_t joinedOrder = 0;
    };

    struct PartyQueueItem
    {
        MediaIdentity media;
        std::string addedBy;
        uint64_t order = 0;
        bool resolvedLocally = false;
    };

    struct PartyPermissions
    {
        PartyPermissionMode mode = PartyPermissionMode::HostOnly;
        bool syncAudio = false;
        bool syncSubtitles = false;
        PartyQueueMode queueMode = PartyQueueMode::HostManaged;
    };

    struct CinemaSettings
    {
        std::string presetName;
        float width = 8.0f;
        float heightOffset = 0.5f;
        float rotationDegrees = 0.0f;
        bool pinned = false;
        float pinnedX = 0.64f;
        float pinnedY = 0.04f;
        float pinnedWidth = 0.32f;
        bool depthOcclusion = true;
        float depthOffset = -0.010f;
        bool distanceAudio = true;
        float audioRange = 60.0f;
        int aspectRatioMode = 0;
        int fitMode = 0;
        int frameStyle = 0;
        bool ambientLighting = false;
    };

    struct PublicRoom
    {
        std::string code;
        std::string title;
        std::string hostName;
        uint32_t memberCount = 0;
    };

    struct PartyRoom
    {
        std::string code;
        std::string memberId;
        std::string hostMemberId;
        RoomVisibility visibility = RoomVisibility::Private;
        uint64_t roomEpoch = 0;
        uint64_t sequence = 0;
        PartyPermissions permissions;
        PartyPlaybackState playback;
        std::vector<PartyMember> members;
        std::vector<PartyQueueItem> queue;
        bool readyCheckActive = false;
        int64_t readyStartAtMs = 0;
        CinemaSettings cinema;
    };

    struct PartyWireMessage
    {
        PartyMessageType type = PartyMessageType::Unknown;
        std::string roomCode;
        std::string senderId;
        std::string memberId;
        std::string hostMemberId;
        std::string displayName;
        std::string error;
        std::string request;
        std::string requestId;
        RoomVisibility visibility = RoomVisibility::Private;
        uint64_t roomEpoch = 0;
        uint64_t sequence = 0;
        int64_t sentAtMs = 0;
        int64_t serverTimeMs = 0;
        int64_t readyStartAtMs = 0;
        bool readyCheckActive = false;
        bool allowed = false;
        bool ready = false;
        PartyPermissions permissions;
        PartyPlaybackState playback;
        CinemaSettings cinema;
        std::vector<PartyMember> members;
        std::vector<PartyQueueItem> queue;
        std::vector<PublicRoom> publicRooms;
    };

    struct FeatureFlags
    {
        bool watchTogether = true;
        bool sharedQueue = true;
        bool readyChecks = true;
        bool sharedCinema = true;
        // Public discovery stays opt-in until relay authentication/moderation requirements are met.
        bool publicRooms = false;
        bool ambientLighting = true;
    };

    MediaMatchQuality MatchMediaIdentity(const MediaIdentity& local, const MediaIdentity& remote);
    bool SameMedia(const MediaIdentity& left, const MediaIdentity& right);
    bool SameStream(const StreamIdentity& left, const StreamIdentity& right);
    const char* ToString(PartyConnectionState state);
    const char* ToString(PartyPermissionMode mode);
    const char* ToString(SyncCorrection correction);
    const char* ToString(MediaMatchQuality quality);
    const char* ToString(PartyMessageType type);
    PartyMessageType PartyMessageTypeFromString(std::string_view value);
}
