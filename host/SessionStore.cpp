#include "SessionStore.hpp"

#include "json.hpp"

#include <windows.h>

#include <algorithm>
#include <fstream>

namespace wxl_party
{
    SessionStore::SessionStore(std::filesystem::path dataDirectory)
        : path_(std::move(dataDirectory) / L"party-settings.json")
    {
    }

    SessionSettings SessionStore::Load() const
    {
        SessionSettings settings;
        std::ifstream input(path_, std::ios::binary);
        if (!input) return settings;
        try
        {
            nlohmann::json object;
            input >> object;
            if (!object.is_object()) return settings;
            settings.relayUrl = object.value("relayUrl", settings.relayUrl);
            settings.displayName = object.value("displayName", settings.displayName);
            settings.lastRoomCode = object.value("lastRoomCode", std::string{});
            if (settings.relayUrl.size() > 512) settings.relayUrl.resize(512);
            if (settings.displayName.size() > 64) settings.displayName.resize(64);
            if (settings.lastRoomCode.size() > 16) settings.lastRoomCode.resize(16);

            const int permission = object.value("permissionMode", 0);
            if (permission >= static_cast<int>(PartyPermissionMode::HostOnly) &&
                permission <= static_cast<int>(PartyPermissionMode::RequestControl))
                settings.permissionMode = static_cast<PartyPermissionMode>(permission);

            const int disconnect = object.value("disconnectPolicy", 0);
            if (disconnect >= static_cast<int>(DisconnectPolicy::KeepPlaying) &&
                disconnect <= static_cast<int>(DisconnectPolicy::AskHost))
                settings.disconnectPolicy = static_cast<DisconnectPolicy>(disconnect);

            settings.syncAudio = object.value("syncAudio", false);
            settings.syncSubtitles = object.value("syncSubtitles", false);
            settings.playbackSpeed = std::clamp(object.value("playbackSpeed", 1.0), 0.5, 2.0);
            settings.noCorrectionDriftMs = std::clamp(object.value("noCorrectionDriftMs", 250), 50, 2000);
            settings.hardSeekDriftMs = std::clamp(
                object.value("hardSeekDriftMs", 1000), settings.noCorrectionDriftMs + 50, 10000);

            if (object.contains("features") && object["features"].is_object())
            {
                const auto& features = object["features"];
                settings.features.watchTogether = features.value("watchTogether", true);
                settings.features.sharedQueue = features.value("sharedQueue", true);
                settings.features.readyChecks = features.value("readyChecks", true);
                settings.features.sharedCinema = features.value("sharedCinema", true);
                settings.features.publicRooms = features.value("publicRooms", settings.features.publicRooms);
                settings.features.ambientLighting = features.value("ambientLighting", true);
            }
        }
        catch (const nlohmann::json::exception&)
        {
            return SessionSettings{};
        }
        return settings;
    }

    bool SessionStore::Save(const SessionSettings& settings) const
    {
        std::error_code error;
        std::filesystem::create_directories(path_.parent_path(), error);
        const auto temporary = path_.wstring() + L".tmp";
        nlohmann::json object = {
            {"relayUrl", settings.relayUrl},
            {"displayName", settings.displayName},
            {"lastRoomCode", settings.lastRoomCode},
            {"permissionMode", static_cast<int>(settings.permissionMode)},
            {"disconnectPolicy", static_cast<int>(settings.disconnectPolicy)},
            {"syncAudio", settings.syncAudio},
            {"syncSubtitles", settings.syncSubtitles},
            {"playbackSpeed", std::clamp(settings.playbackSpeed, 0.5, 2.0)},
            {"noCorrectionDriftMs", settings.noCorrectionDriftMs},
            {"hardSeekDriftMs", settings.hardSeekDriftMs},
            {"features", {
                {"watchTogether", settings.features.watchTogether},
                {"sharedQueue", settings.features.sharedQueue},
                {"readyChecks", settings.features.readyChecks},
                {"sharedCinema", settings.features.sharedCinema},
                {"publicRooms", settings.features.publicRooms},
                {"ambientLighting", settings.features.ambientLighting},
            }},
        };
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        output << object.dump(2);
        output.close();
        if (!output) return false;
        return MoveFileExW(temporary.c_str(), path_.c_str(),
                           MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    }
}
