#include "../FramePublisher.hpp"
#include "../VideoShared.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (false)

int main()
{
    wxl_video_shared::FrameHeader header{};
    std::vector<std::byte> mapping(wxl_video_shared::kMappingBytes);
    std::vector<uint8_t> first(wxl_video_shared::kFrameBytes, 0x11);
    std::vector<uint8_t> second(wxl_video_shared::kFrameBytes, 0x22);

    CHECK(wxl_frame::PublishFrameForTest(
        header, mapping, first, wxl_video_shared::kStride));
    CHECK(wxl_frame::PublishFrameForTest(
        header, mapping, second, wxl_video_shared::kStride));
    CHECK(header.sequence == 2);
    CHECK(header.ready == 1);

    const auto active = static_cast<unsigned>(header.activeIndex) & 1u;
    const auto* pixels = wxl_video_shared::Pixels(mapping.data(), active);
    CHECK(pixels[0] == 0x22);
    CHECK(pixels[wxl_video_shared::kFrameBytes - 1] == 0x22);
    CHECK(header.slotSequence[active] == header.sequence);
    return 0;
}
