#include "PartyTypes.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace wxl_party
{
    namespace
    {
        std::string Fold(std::string_view value)
        {
            std::string result(value);
            std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
            return result;
        }

        bool EqualNonEmpty(std::string_view left, std::string_view right)
        {
            return !left.empty() && !right.empty() && left == right;
        }

        bool FallbackMatch(const MediaIdentity& left, const MediaIdentity& right)
        {
            if (left.title.empty() || right.title.empty() || Fold(left.title) != Fold(right.title))
                return false;
            if (left.year > 0 && right.year > 0 && left.year != right.year)
                return false;
            if (left.type == "episode" || right.type == "episode")
            {
                if (left.season <= 0 || right.season <= 0 || left.episode <= 0 || right.episode <= 0)
                    return false;
                if (left.season != right.season || left.episode != right.episode)
                    return false;
                if (!left.grandparentTitle.empty() && !right.grandparentTitle.empty() &&
                    Fold(left.grandparentTitle) != Fold(right.grandparentTitle))
                    return false;
            }
            if (left.durationMs > 0 && right.durationMs > 0)
            {
                const int64_t tolerance = std::max<int64_t>(5000, std::min(left.durationMs, right.durationMs) / 100);
                if (std::llabs(left.durationMs - right.durationMs) > tolerance)
                    return false;
            }
            return true;
        }
    }

    MediaMatchQuality MatchMediaIdentity(const MediaIdentity& local, const MediaIdentity& remote)
    {
        if (EqualNonEmpty(local.serverIdentifier, remote.serverIdentifier) &&
            EqualNonEmpty(local.ratingKey, remote.ratingKey))
            return MediaMatchQuality::ExactServerItem;
        if (EqualNonEmpty(local.plexGuid, remote.plexGuid))
            return MediaMatchQuality::PlexGuid;
        if (EqualNonEmpty(local.tmdbId, remote.tmdbId) ||
            EqualNonEmpty(local.tvdbId, remote.tvdbId) ||
            EqualNonEmpty(local.imdbId, remote.imdbId))
            return MediaMatchQuality::ExternalGuid;
        if (local.type == "episode" && remote.type == "episode" &&
            local.season > 0 && remote.season > 0 && local.episode > 0 && remote.episode > 0 &&
            local.season == remote.season && local.episode == remote.episode &&
            !local.grandparentTitle.empty() && !remote.grandparentTitle.empty() &&
            Fold(local.grandparentTitle) == Fold(remote.grandparentTitle))
            return MediaMatchQuality::EpisodeIdentity;
        if (FallbackMatch(local, remote))
            return MediaMatchQuality::Fallback;
        return MediaMatchQuality::None;
    }

    bool SameMedia(const MediaIdentity& left, const MediaIdentity& right)
    {
        return MatchMediaIdentity(left, right) != MediaMatchQuality::None;
    }

    bool SameStream(const StreamIdentity& left, const StreamIdentity& right)
    {
        if (!left.valid || !right.valid) return false;
        if (!left.language.empty() && !right.language.empty() && Fold(left.language) != Fold(right.language))
            return false;
        if (!left.codec.empty() && !right.codec.empty() && Fold(left.codec) != Fold(right.codec))
            return false;
        if (!left.title.empty() && !right.title.empty() && Fold(left.title) != Fold(right.title))
            return false;
        return left.forced == right.forced && left.hearingImpaired == right.hearingImpaired;
    }

    const char* ToString(PartyConnectionState state)
    {
        switch (state)
        {
        case PartyConnectionState::Disconnected: return "Disconnected";
        case PartyConnectionState::Connecting: return "Connecting";
        case PartyConnectionState::Connected: return "Connected";
        case PartyConnectionState::Reconnecting: return "Reconnecting";
        case PartyConnectionState::Failed: return "Failed";
        }
        return "Unknown";
    }

    const char* ToString(PartyPermissionMode mode)
    {
        switch (mode)
        {
        case PartyPermissionMode::HostOnly: return "Host Only";
        case PartyPermissionMode::SharedControl: return "Shared Control";
        case PartyPermissionMode::RequestControl: return "Request Control";
        }
        return "Host Only";
    }

    const char* ToString(SyncCorrection correction)
    {
        switch (correction)
        {
        case SyncCorrection::None: return "None";
        case SyncCorrection::SlowDown: return "Slow Down";
        case SyncCorrection::SpeedUp: return "Speed Up";
        case SyncCorrection::HardSeek: return "Hard Seek";
        }
        return "None";
    }

    const char* ToString(MediaMatchQuality quality)
    {
        switch (quality)
        {
        case MediaMatchQuality::None: return "No Match";
        case MediaMatchQuality::Fallback: return "Fallback Match";
        case MediaMatchQuality::EpisodeIdentity: return "Episode Match";
        case MediaMatchQuality::ExternalGuid: return "External GUID Match";
        case MediaMatchQuality::PlexGuid: return "Plex GUID Match";
        case MediaMatchQuality::ExactServerItem: return "Exact Server Item";
        }
        return "No Match";
    }

    const char* ToString(PartyMessageType type)
    {
        switch (type)
        {
        case PartyMessageType::Hello: return "hello";
        case PartyMessageType::RoomCreate: return "room_create";
        case PartyMessageType::RoomCreated: return "room_created";
        case PartyMessageType::RoomJoin: return "room_join";
        case PartyMessageType::RoomJoined: return "room_joined";
        case PartyMessageType::RoomLeave: return "room_leave";
        case PartyMessageType::MemberJoined: return "member_joined";
        case PartyMessageType::MemberLeft: return "member_left";
        case PartyMessageType::RoomState: return "room_state";
        case PartyMessageType::MediaChanged: return "media_changed";
        case PartyMessageType::PlaybackState: return "playback_state";
        case PartyMessageType::Play: return "play";
        case PartyMessageType::Pause: return "pause";
        case PartyMessageType::Seek: return "seek";
        case PartyMessageType::Stop: return "stop";
        case PartyMessageType::HostChanged: return "host_changed";
        case PartyMessageType::Ping: return "ping";
        case PartyMessageType::Pong: return "pong";
        case PartyMessageType::Error: return "error";
        case PartyMessageType::ControlRequest: return "control_request";
        case PartyMessageType::ControlResponse: return "control_response";
        case PartyMessageType::Permissions: return "permissions";
        case PartyMessageType::QueueUpdate: return "queue_update";
        case PartyMessageType::ReadyCheck: return "ready_check";
        case PartyMessageType::ReadyState: return "ready_state";
        case PartyMessageType::ScreenPreset: return "screen_preset";
        case PartyMessageType::PublicRoomsRequest: return "public_rooms_request";
        case PartyMessageType::PublicRooms: return "public_rooms";
        case PartyMessageType::LiveStart: return "live_start";
        case PartyMessageType::LiveStop: return "live_stop";
        case PartyMessageType::LiveWatch: return "live_watch";
        case PartyMessageType::LiveUnwatch: return "live_unwatch";
        case PartyMessageType::LiveStarted: return "live_started";
        case PartyMessageType::LiveStopped: return "live_stopped";
        case PartyMessageType::LiveState: return "live_state";
        case PartyMessageType::Unknown: break;
        }
        return "unknown";
    }

    PartyMessageType PartyMessageTypeFromString(std::string_view value)
    {
        constexpr PartyMessageType values[] = {
            PartyMessageType::Hello, PartyMessageType::RoomCreate, PartyMessageType::RoomCreated,
            PartyMessageType::RoomJoin, PartyMessageType::RoomJoined, PartyMessageType::RoomLeave,
            PartyMessageType::MemberJoined, PartyMessageType::MemberLeft, PartyMessageType::RoomState,
            PartyMessageType::MediaChanged, PartyMessageType::PlaybackState, PartyMessageType::Play,
            PartyMessageType::Pause, PartyMessageType::Seek, PartyMessageType::Stop,
            PartyMessageType::HostChanged, PartyMessageType::Ping, PartyMessageType::Pong,
            PartyMessageType::Error, PartyMessageType::ControlRequest, PartyMessageType::ControlResponse,
            PartyMessageType::Permissions, PartyMessageType::QueueUpdate, PartyMessageType::ReadyCheck,
            PartyMessageType::ReadyState, PartyMessageType::ScreenPreset,
            PartyMessageType::PublicRoomsRequest, PartyMessageType::PublicRooms,
            PartyMessageType::LiveStart, PartyMessageType::LiveStop, PartyMessageType::LiveWatch,
            PartyMessageType::LiveUnwatch, PartyMessageType::LiveStarted, PartyMessageType::LiveStopped,
            PartyMessageType::LiveState,
        };
        for (const auto type : values)
            if (value == ToString(type)) return type;
        return PartyMessageType::Unknown;
    }
}
