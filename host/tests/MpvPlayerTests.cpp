#include "../MpvPlayer.hpp"

#include <cassert>

int main()
{
    wxl_mpv::MpvPlayer player;
    assert(!player.Initialize(nullptr, nullptr, nullptr));
    return 0;
}
