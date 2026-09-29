#include "PartyProtocol.hpp"

#include "json.hpp"

#include <algorithm>
#include <limits>

namespace wxl_party
{
    namespace
    {
        using json = nlohmann::json;

        std::string BoundedString(const json& value, const char* key, size_t maximum)
        {
            if (!value.contains(key) || !value[key].is_string()) return {};
            std::string result = value[key].get<std::string>();
            if (result.size() > maximum) result.resize(maximum);
            return result;
        }

        uint64_t Unsigned(const json& value, const char* key)
        {
            if (!value.contains(key) || !value[key].is_number_unsigned()) return 0;
            return value[key].get<uint64_t>();
        }

        int64_t Signed(const json& value, const char* key)
        {
            if (!value.contains(key) || !value[key].is_number_integer()) return 0;
            return value[key].get<int64_t>();
        }

        json MediaToJson(const MediaIdentity& media)
        {
            return {
                {"serverIdentifier", media.serverIdentifier},
                {"ratingKey", media.ratingKey},
                {"plexGuid", media.plexGuid},
                {"imdbId", media.imdbId},
                {"tmdbId", media.tmdbId},
                {"tvdbId", media.tvdbId},
                {"type", media.type},
                {"title", media.title},
                {"grandparentTitle", media.grandparentTitle},
                {"year", media.year},
                {"season", media.season},
                {"episode", media.episode},
                {"durationMs", media.durationMs},
            };
        }

        MediaIdentity MediaFromJson(const json& value)
        {
            MediaIdentity media;
            if (!value.is_object()) return media;
            media.serverIdentifier = BoundedString(value, "serverIdentifier", 128);
            media.ratingKey = BoundedString(value, "ratingKey", 64);
            media.plexGuid = BoundedString(value, "plexGuid", 256);
            media.imdbId = BoundedString(value, "imdbId", 64);
            media.tmdbId = BoundedString(value, "tmdbId", 64);
            media.tvdbId = BoundedString(value, "tvdbId", 64);
            media.type = BoundedString(value, "type", 32);
            media.title = BoundedString(value, "title", 256);
            media.grandparentTitle = BoundedString(value, "grandparentTitle", 256);
            media.year = value.value("year", 0);
            media.season = value.value("season", 0);
            media.episode = value.value("episode", 0);
            media.durationMs = std::max<int64_t>(0, Signed(value, "durationMs"));
            return media;
        }

        json StreamToJson(const StreamIdentity& stream)
        {
            return {
                {"language", stream.language}, {"title", stream.title}, {"codec", stream.codec},
                {"forced", stream.forced}, {"hearingImpaired", stream.hearingImpaired},
                {"valid", stream.valid},
            };
        }

        StreamIdentity StreamFromJson(const json& value)
        {
            StreamIdentity stream;
            if (!value.is_object()) return stream;
            stream.language = BoundedString(value, "language", 64);
            stream.title = BoundedString(value, "title", 128);
            stream.codec = BoundedString(value, "codec", 32);
            stream.forced = value.value("forced", false);
            stream.hearingImpaired = value.value("hearingImpaired", false);
            stream.valid = value.value("valid", false);
            return stream;
        }

        json PlaybackToJson(const PartyPlaybackState& playback)
        {
            return {
                {"media", MediaToJson(playback.media)},
                {"audio", StreamToJson(playback.audio)},
                {"subtitle", StreamToJson(playback.subtitle)},
                {"playing", playback.playing},
                {"paused", playback.paused},
                {"stopped", playback.stopped},
                {"positionMs", playback.positionMs},
                {"durationMs", playback.durationMs},
                {"authoritativeTimestampMs", playback.authoritativeTimestampMs},
                {"roomEpoch", playback.roomEpoch},
                {"sequence", playback.sequence},
            };
        }

        PartyPlaybackState PlaybackFromJson(const json& value)
        {
            PartyPlaybackState playback;
            if (!value.is_object()) return playback;
            if (value.contains("media")) playback.media = MediaFromJson(value["media"]);
            if (value.contains("audio")) playback.audio = StreamFromJson(value["audio"]);
            if (value.contains("subtitle")) playback.subtitle = StreamFromJson(value["subtitle"]);
            playback.playing = value.value("playing", false);
            playback.paused = value.value("paused", false);
            playback.stopped = value.value("stopped", true);
            playback.positionMs = std::max<int64_t>(0, Signed(value, "positionMs"));
            playback.durationMs = std::max<int64_t>(0, Signed(value, "durationMs"));
            playback.authoritativeTimestampMs = Signed(value, "authoritativeTimestampMs");
            playback.roomEpoch = Unsigned(value, "roomEpoch");
            playback.sequence = Unsigned(value, "sequence");
            return playback;
        }

        json MemberToJson(const PartyMember& member)
        {
            return {
                {"id", member.id},
                {"displayName", member.displayName},
                {"role", member.role == PartyRole::Host ? "host" : "guest"},
                {"connected", member.connected},
                {"ready", member.ready},
                {"joinedOrder", member.joinedOrder},
            };
        }

        PartyMember MemberFromJson(const json& value)
        {
            PartyMember member;
            if (!value.is_object()) return member;
            member.id = BoundedString(value, "id", 128);
            member.displayName = BoundedString(value, "displayName", 64);
            member.role = BoundedString(value, "role", 16) == "host" ? PartyRole::Host : PartyRole::Guest;
            member.connected = value.value("connected", true);
            member.ready = value.value("ready", false);
            member.joinedOrder = Unsigned(value, "joinedOrder");
            return member;
        }

        json PermissionsToJson(const PartyPermissions& permissions)
        {
            return {
                {"mode", static_cast<int>(permissions.mode)},
                {"syncAudio", permissions.syncAudio},
                {"syncSubtitles", permissions.syncSubtitles},
                {"queueMode", static_cast<int>(permissions.queueMode)},
            };
        }

        PartyPermissions PermissionsFromJson(const json& value)
        {
            PartyPermissions permissions;
            if (!value.is_object()) return permissions;
            const int mode = value.value("mode", 0);
            if (mode >= static_cast<int>(PartyPermissionMode::HostOnly) &&
                mode <= static_cast<int>(PartyPermissionMode::RequestControl))
                permissions.mode = static_cast<PartyPermissionMode>(mode);
            permissions.syncAudio = value.value("syncAudio", false);
            permissions.syncSubtitles = value.value("syncSubtitles", false);
            const int queueMode = value.value("queueMode", 0);
            if (queueMode >= static_cast<int>(PartyQueueMode::HostManaged) &&
                queueMode <= static_cast<int>(PartyQueueMode::EveryoneMayAdd))
                permissions.queueMode = static_cast<PartyQueueMode>(queueMode);
            return permissions;
        }

        json QueueItemToJson(const PartyQueueItem& item)
        {
            return {
                {"media", MediaToJson(item.media)},
                {"addedBy", item.addedBy},
                {"order", item.order},
            };
        }

        PartyQueueItem QueueItemFromJson(const json& value)
        {
            PartyQueueItem item;
            if (!value.is_object()) return item;
            if (value.contains("media")) item.media = MediaFromJson(value["media"]);
            item.addedBy = BoundedString(value, "addedBy", 128);
            item.order = Unsigned(value, "order");
            return item;
        }

        json CinemaToJson(const CinemaSettings& cinema)
        {
            return {
                {"presetName", cinema.presetName},
                {"width", cinema.width},
                {"heightOffset", cinema.heightOffset},
                {"rotationDegrees", cinema.rotationDegrees},
                {"pinned", cinema.pinned},
                {"pinnedX", cinema.pinnedX},
                {"pinnedY", cinema.pinnedY},
                {"pinnedWidth", cinema.pinnedWidth},
                {"depthOcclusion", cinema.depthOcclusion},
                {"depthOffset", cinema.depthOffset},
                {"distanceAudio", cinema.distanceAudio},
                {"audioRange", cinema.audioRange},
                {"aspectRatioMode", cinema.aspectRatioMode},
                {"fitMode", cinema.fitMode},
                {"frameStyle", cinema.frameStyle},
                {"ambientLighting", cinema.ambientLighting},
            };
        }

        CinemaSettings CinemaFromJson(const json& value)
        {
            CinemaSettings cinema;
            if (!value.is_object()) return cinema;
            cinema.presetName = BoundedString(value, "presetName", 64);
            cinema.width = value.value("width", 8.0f);
            cinema.heightOffset = value.value("heightOffset", 0.5f);
            cinema.rotationDegrees = value.value("rotationDegrees", 0.0f);
            cinema.pinned = value.value("pinned", false);
            cinema.pinnedX = value.value("pinnedX", 0.64f);
            cinema.pinnedY = value.value("pinnedY", 0.04f);
            cinema.pinnedWidth = value.value("pinnedWidth", 0.32f);
            cinema.depthOcclusion = value.value("depthOcclusion", true);
            cinema.depthOffset = value.value("depthOffset", -0.010f);
            cinema.distanceAudio = value.value("distanceAudio", true);
            cinema.audioRange = value.value("audioRange", 60.0f);
            cinema.aspectRatioMode = value.value("aspectRatioMode", 0);
            cinema.fitMode = value.value("fitMode", 0);
            cinema.frameStyle = value.value("frameStyle", 0);
            cinema.ambientLighting = value.value("ambientLighting", false);
            return cinema;
        }
    }

    std::string SerializePartyMessage(const PartyWireMessage& message)
    {
        json object = {
            {"protocol", kPartyProtocolVersion},
            {"type", ToString(message.type)},
            {"room", message.roomCode},
            {"sender", message.senderId},
            {"memberId", message.memberId},
            {"hostMemberId", message.hostMemberId},
            {"displayName", message.displayName},
            {"roomEpoch", message.roomEpoch},
            {"sequence", message.sequence},
            {"sentAtMs", message.sentAtMs},
        };
        json payload = json::object();
        if (!message.error.empty()) payload["error"] = message.error;
        if (!message.request.empty()) payload["request"] = message.request;
        if (!message.requestId.empty()) payload["requestId"] = message.requestId;
        payload["visibility"] = message.visibility == RoomVisibility::Public ? "public" : "private";
        payload["allowed"] = message.allowed;
        payload["ready"] = message.ready;
        payload["readyCheckActive"] = message.readyCheckActive;
        if (message.serverTimeMs != 0) payload["serverTimeMs"] = message.serverTimeMs;
        if (message.readyStartAtMs != 0) payload["readyStartAtMs"] = message.readyStartAtMs;

        if (message.type == PartyMessageType::PlaybackState ||
            message.type == PartyMessageType::MediaChanged ||
            message.type == PartyMessageType::Play || message.type == PartyMessageType::Pause ||
            message.type == PartyMessageType::Seek || message.type == PartyMessageType::Stop ||
            message.type == PartyMessageType::RoomState || message.type == PartyMessageType::RoomCreated ||
            message.type == PartyMessageType::RoomJoined)
            payload["playback"] = PlaybackToJson(message.playback);

        if (message.type == PartyMessageType::Permissions || message.type == PartyMessageType::RoomState ||
            message.type == PartyMessageType::RoomCreated || message.type == PartyMessageType::RoomJoined)
            payload["permissions"] = PermissionsToJson(message.permissions);

        if (!message.members.empty() || message.type == PartyMessageType::RoomState ||
            message.type == PartyMessageType::RoomCreated || message.type == PartyMessageType::RoomJoined)
        {
            payload["members"] = json::array();
            for (const auto& member : message.members) payload["members"].push_back(MemberToJson(member));
        }

        if (!message.queue.empty() || message.type == PartyMessageType::QueueUpdate ||
            message.type == PartyMessageType::RoomState || message.type == PartyMessageType::RoomCreated ||
            message.type == PartyMessageType::RoomJoined)
        {
            payload["queue"] = json::array();
            for (const auto& item : message.queue) payload["queue"].push_back(QueueItemToJson(item));
        }

        if (message.type == PartyMessageType::ScreenPreset || message.type == PartyMessageType::RoomState ||
            message.type == PartyMessageType::RoomCreated || message.type == PartyMessageType::RoomJoined)
            payload["cinema"] = CinemaToJson(message.cinema);

        if (!message.publicRooms.empty() || message.type == PartyMessageType::PublicRooms)
        {
            payload["publicRooms"] = json::array();
            for (const auto& room : message.publicRooms)
            {
                payload["publicRooms"].push_back({
                    {"code", room.code}, {"title", room.title}, {"hostName", room.hostName},
                    {"memberCount", room.memberCount},
                });
            }
        }

        object["payload"] = std::move(payload);
        return object.dump();
    }

    bool ParsePartyMessage(std::string_view text, PartyWireMessage& message, std::string& error)
    {
        message = {};
        error.clear();
        if (text.empty() || text.size() > kMaxPartyMessageBytes)
        {
            error = "Party message is empty or exceeds the maximum size.";
            return false;
        }
        try
        {
            const json object = json::parse(text.begin(), text.end());
            if (!object.is_object())
            {
                error = "Party message is not an object.";
                return false;
            }
            if (object.value("protocol", 0) != kPartyProtocolVersion)
            {
                error = "Party protocol version mismatch.";
                return false;
            }
            message.type = PartyMessageTypeFromString(BoundedString(object, "type", 64));
            if (message.type == PartyMessageType::Unknown)
            {
                error = "Unknown party message type.";
                return false;
            }
            message.roomCode = BoundedString(object, "room", 16);
            message.senderId = BoundedString(object, "sender", 128);
            message.memberId = BoundedString(object, "memberId", 128);
            message.hostMemberId = BoundedString(object, "hostMemberId", 128);
            message.displayName = BoundedString(object, "displayName", 64);
            message.roomEpoch = Unsigned(object, "roomEpoch");
            message.sequence = Unsigned(object, "sequence");
            message.sentAtMs = Signed(object, "sentAtMs");

            if (!object.contains("payload") || !object["payload"].is_object()) return true;
            const json& payload = object["payload"];
            message.error = BoundedString(payload, "error", 256);
            message.request = BoundedString(payload, "request", 64);
            message.requestId = BoundedString(payload, "requestId", 128);
            message.visibility = BoundedString(payload, "visibility", 16) == "public"
                ? RoomVisibility::Public : RoomVisibility::Private;
            message.allowed = payload.value("allowed", false);
            message.ready = payload.value("ready", false);
            message.readyCheckActive = payload.value("readyCheckActive", false);
            message.serverTimeMs = Signed(payload, "serverTimeMs");
            message.readyStartAtMs = Signed(payload, "readyStartAtMs");
            if (payload.contains("playback")) message.playback = PlaybackFromJson(payload["playback"]);
            if (payload.contains("permissions")) message.permissions = PermissionsFromJson(payload["permissions"]);
            if (payload.contains("cinema")) message.cinema = CinemaFromJson(payload["cinema"]);

            if (payload.contains("members") && payload["members"].is_array())
            {
                for (const auto& item : payload["members"])
                {
                    if (message.members.size() >= kMaxPartyMembers) break;
                    PartyMember member = MemberFromJson(item);
                    if (!member.id.empty()) message.members.push_back(std::move(member));
                }
            }
            if (payload.contains("queue") && payload["queue"].is_array())
            {
                for (const auto& item : payload["queue"])
                {
                    if (message.queue.size() >= kMaxPartyQueueItems) break;
                    message.queue.push_back(QueueItemFromJson(item));
                }
            }
            if (payload.contains("publicRooms") && payload["publicRooms"].is_array())
            {
                for (const auto& item : payload["publicRooms"])
                {
                    if (message.publicRooms.size() >= 64 || !item.is_object()) break;
                    PublicRoom room;
                    room.code = BoundedString(item, "code", 16);
                    room.title = BoundedString(item, "title", 256);
                    room.hostName = BoundedString(item, "hostName", 64);
                    room.memberCount = static_cast<uint32_t>(std::clamp<int64_t>(
                        Signed(item, "memberCount"), 0, static_cast<int64_t>(kMaxPartyMembers)));
                    if (!room.code.empty()) message.publicRooms.push_back(std::move(room));
                }
            }
            return true;
        }
        catch (const nlohmann::json::exception& exception)
        {
            error = std::string("Invalid party JSON: ") + exception.what();
            return false;
        }
    }
}
