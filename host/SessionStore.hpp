#pragma once

#include "PartyTypes.hpp"

#include <filesystem>
#include <string>

namespace wxl_party
{
    struct SessionSettings
    {
        std::string relayUrl = "ws://127.0.0.1:17777/ws";
        std::string displayName = "Player";
        std::string lastRoomCode;
        PartyPermissionMode permissionMode = PartyPermissionMode::HostOnly;
        DisconnectPolicy disconnectPolicy = DisconnectPolicy::KeepPlaying;
        bool syncAudio = false;
        bool syncSubtitles = false;
        double playbackSpeed = 1.0;
        int noCorrectionDriftMs = static_cast<int>(kNoCorrectionDriftMs);
        int hardSeekDriftMs = static_cast<int>(kHardSeekDriftMs);
        FeatureFlags features{};
    };

    class SessionStore final
    {
    public:
        explicit SessionStore(std::filesystem::path dataDirectory);

        SessionSettings Load() const;
        bool Save(const SessionSettings& settings) const;

    private:
        std::filesystem::path path_;
    };
}
