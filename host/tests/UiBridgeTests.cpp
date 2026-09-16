#include "../VideoShared.hpp"

#include <cmath>
#include <cstring>

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (false)

int main()
{
    using namespace wxl_video_shared;

    UiBridge bridge{};
    bridge.magic = kUiMagic;
    bridge.version = kUiVersion;
    bridge.structBytes = sizeof(bridge);
    bridge.activeIndex = 1;
    bridge.sequence = 7;
    bridge.slotSequence[1] = 7;
    bridge.slots[1].structBytes = sizeof(UiSnapshot);
    bridge.slots[1].view = UiView::Details;
    std::strcpy(bridge.slots[1].error, "metadata failed");
    std::memset(bridge.slots[1].status, 'x', sizeof(bridge.slots[1].status));

    UiSnapshot snapshot{};
    CHECK(ReadUiSnapshot(&bridge, snapshot));
    CHECK(snapshot.view == UiView::Details);
    CHECK(std::strcmp(snapshot.error, "metadata failed") == 0);
    CHECK(snapshot.status[kUiStatusBytes - 1] == '\0');

    bridge.slotSequence[1] = 8;
    CHECK(!ReadUiSnapshot(&bridge, snapshot));

    float left = 0.9f;
    float top = 0.9f;
    float width = 0.9f;
    ClampPinnedLayout(left, top, width);
    CHECK(std::abs(width - 0.8f) < 0.0001f);
    CHECK(std::abs(left - 0.2f) < 0.0001f);
    CHECK(std::abs(top - 0.55f) < 0.0001f);
    return 0;
}
