#pragma once

#include "VideoShared.hpp"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace wxl_frame
{
    class FramePublisher final
    {
    public:
        bool Open();
        void Close();
        bool Publish(std::span<const uint8_t> bgra, uint32_t sourceStride);

    private:
        HANDLE mapping_ = nullptr;
        void* mappingView_ = nullptr;
    };

    bool PublishFrameForTest(wxl_video_shared::FrameHeader& header,
                             std::vector<std::byte>& mapping,
                             std::span<const uint8_t> bgra,
                             uint32_t sourceStride);
}
