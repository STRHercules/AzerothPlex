#include "../MpvPlayer.hpp"

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (false)

int main()
{
    CHECK(wxl_mpv::ClampSeekMilliseconds(-1, 120000) == 0);
    CHECK(wxl_mpv::ClampSeekMilliseconds(130000, 120000) == 120000);
    CHECK(wxl_mpv::ClampSeekMilliseconds(42000, 120000) == 42000);
    CHECK(wxl_mpv::IsPrematureEnd(MPV_END_FILE_REASON_EOF, 120000, 1.0));
    CHECK(!wxl_mpv::IsPrematureEnd(MPV_END_FILE_REASON_EOF, 120000, 119.0));
    CHECK(!wxl_mpv::IsPrematureEnd(MPV_END_FILE_REASON_STOP, 120000, 1.0));
    wxl_mpv::MpvPlayer player;
    CHECK(!player.Initialize(nullptr, nullptr, nullptr));
    return 0;
}
