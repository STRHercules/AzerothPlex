#include "../FramePublisher.hpp"
#include "../VideoShared.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

int main()
{
    wxl_video_shared::FrameHeader header{};
    std::vector<std::byte> mapping(wxl_video_shared::kMappingBytes);
    std::vector<uint8_t> first(wxl_video_shared::kFrameBytes, 0x11);
    std::vector<uint8_t> second(wxl_video_shared::kFrameBytes, 0x22);

    assert(wxl_frame::PublishFrameForTest(
        header, mapping, first, wxl_video_shared::kStride));
    assert(wxl_frame::PublishFrameForTest(
        header, mapping, second, wxl_video_shared::kStride));
    assert(header.sequence == 2);
    assert(header.ready == 1);

    const auto active = static_cast<unsigned>(header.activeIndex) & 1u;
    const auto* pixels = wxl_video_shared::Pixels(mapping.data(), active);
    assert(pixels[0] == 0x22);
    assert(pixels[wxl_video_shared::kFrameBytes - 1] == 0x22);
    assert(header.slotSequence[active] == header.sequence);
    return 0;
}
