#include "../SessionStore.hpp"

#include <windows.h>

#include <cmath>
#include <filesystem>

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (false)

int main()
{
    namespace fs = std::filesystem;
    using namespace wxl_party;

    const fs::path directory = fs::temp_directory_path() /
        (L"azerothplex-session-tests-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code error;
    fs::remove_all(directory, error);

    SessionStore store(directory);
    SessionSettings settings;
    settings.displayName = "Test Viewer";
    settings.lastRoomCode = "AZ-TEST";
    settings.permissionMode = PartyPermissionMode::SharedControl;
    settings.syncAudio = true;
    settings.playbackSpeed = 1.5;
    settings.noCorrectionDriftMs = 300;
    settings.hardSeekDriftMs = 1400;
    settings.features.publicRooms = false;

    CHECK(store.Save(settings));
    const SessionSettings loaded = store.Load();
    CHECK(loaded.displayName == settings.displayName);
    CHECK(loaded.lastRoomCode == settings.lastRoomCode);
    CHECK(loaded.permissionMode == PartyPermissionMode::SharedControl);
    CHECK(loaded.syncAudio);
    CHECK(std::abs(loaded.playbackSpeed - 1.5) < 0.001);
    CHECK(loaded.noCorrectionDriftMs == 300);
    CHECK(loaded.hardSeekDriftMs == 1400);
    CHECK(!loaded.features.publicRooms);

    fs::remove_all(directory, error);
    return 0;
}
