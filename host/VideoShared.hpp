#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace wxl_video_shared
{
    constexpr wchar_t kMappingName[] = L"Local\\WXLVideoCinemaFrames-v1";
    constexpr wchar_t kControlMappingName[] = L"Local\\WXLVideoCinemaControl-v1";
    constexpr uint32_t kMagic = 0x46565857u; // "WXVF"
    constexpr uint32_t kVersion = 2;
    constexpr uint32_t kWidth = 640;
    constexpr uint32_t kHeight = 360;
    constexpr uint32_t kStride = kWidth * 4;
    constexpr uint32_t kFrameBytes = kStride * kHeight;
    constexpr uint32_t kControlMagic = 0x43565857u; // "WXVC"
    constexpr uint32_t kControlVersion = 1;

    enum class Command : LONG
    {
        None = 0,
        Show = 1,
        Play = 2,
        Pause = 3,
        Stop = 4,
        Hide = 5,
    };

#pragma pack(push, 1)
    struct FrameHeader
    {
        uint32_t magic;
        uint32_t version;
        uint32_t headerBytes;
        uint32_t width;
        uint32_t height;
        uint32_t stride;
        uint32_t frameBytes;
        volatile LONG activeIndex;
        volatile LONG sequence;
        volatile LONG slotSequence[2];
        volatile LONG hostPid;
        volatile LONG ready;
        uint32_t reserved[3];
    };
#pragma pack(pop)

    static_assert(sizeof(FrameHeader) == 64, "shared video header ABI changed");

    struct ControlBlock
    {
        uint32_t magic;
        uint32_t version;
        uint32_t structBytes;
        volatile LONG commandSequence;
        volatile LONG command;
        volatile LONG volumeSequence;
        volatile LONG volumePercent;
        uint32_t reserved[4];
    };

    static_assert(sizeof(ControlBlock) == 44, "shared video control ABI changed");

    constexpr size_t kMappingBytes = sizeof(FrameHeader) + size_t(kFrameBytes) * 2u;

    inline uint8_t* Pixels(void* mapping, unsigned index)
    {
        return static_cast<uint8_t*>(mapping) + sizeof(FrameHeader) +
               size_t(index & 1u) * kFrameBytes;
    }

    inline const uint8_t* Pixels(const void* mapping, unsigned index)
    {
        return static_cast<const uint8_t*>(mapping) + sizeof(FrameHeader) +
               size_t(index & 1u) * kFrameBytes;
    }
}
