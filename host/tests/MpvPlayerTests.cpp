#include "../MpvPlayer.hpp"

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (false)

int main()
{
    wxl_mpv::MpvPlayer player;
    CHECK(!player.Initialize(nullptr, nullptr, nullptr));
    return 0;
}
