#include "FramePublisher.hpp"

#include <cstring>

namespace wxl_frame
{
    namespace
    {
        using wxl_video_shared::FrameHeader;

        void InitializeHeader(FrameHeader& header)
        {
            header.magic = wxl_video_shared::kMagic;
            header.version = wxl_video_shared::kVersion;
            header.headerBytes = sizeof(FrameHeader);
            header.width = wxl_video_shared::kWidth;
            header.height = wxl_video_shared::kHeight;
            header.stride = wxl_video_shared::kStride;
            header.frameBytes = wxl_video_shared::kFrameBytes;
            InterlockedExchange(&header.activeIndex, 0);
            InterlockedExchange(&header.sequence, 0);
            InterlockedExchange(&header.slotSequence[0], 0);
            InterlockedExchange(&header.slotSequence[1], 0);
            InterlockedExchange(&header.hostPid, 0);
            InterlockedExchange(&header.ready, 0);
        }

        bool PublishFrame(FrameHeader& header, void* mapping,
                          std::span<const uint8_t> bgra, uint32_t sourceStride)
        {
            if (!mapping || sourceStride < wxl_video_shared::kStride ||
                bgra.size() < size_t(sourceStride) * wxl_video_shared::kHeight)
                return false;

            LONG nextSequence = header.sequence + 1;
            if (nextSequence <= 0) nextSequence = 1;
            const unsigned target = static_cast<unsigned>((header.activeIndex ^ 1) & 1);

            InterlockedExchange(&header.slotSequence[target], -nextSequence);
            uint8_t* destination = wxl_video_shared::Pixels(mapping, target);
            for (uint32_t row = 0; row < wxl_video_shared::kHeight; ++row)
            {
                std::memcpy(destination + size_t(row) * wxl_video_shared::kStride,
                            bgra.data() + size_t(row) * sourceStride,
                            wxl_video_shared::kStride);
            }
            MemoryBarrier();
            InterlockedExchange(&header.slotSequence[target], nextSequence);
            InterlockedExchange(&header.activeIndex, static_cast<LONG>(target));
            InterlockedExchange(&header.sequence, nextSequence);
            InterlockedExchange(&header.ready, 1);
            return true;
        }
    }

    bool PublishFrameForTest(FrameHeader& header, std::vector<std::byte>& mapping,
                             std::span<const uint8_t> bgra, uint32_t sourceStride)
    {
        if (mapping.size() < wxl_video_shared::kMappingBytes)
            return false;
        if (header.magic != wxl_video_shared::kMagic ||
            header.version != wxl_video_shared::kVersion)
            InitializeHeader(header);
        return PublishFrame(header, mapping.data(), bgra, sourceStride);
    }

    bool FramePublisher::Open()
    {
        if (mappingView_) return true;

        mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                      static_cast<DWORD>(wxl_video_shared::kMappingBytes),
                                      wxl_video_shared::kMappingName);
        if (!mapping_) return false;

        const bool created = GetLastError() != ERROR_ALREADY_EXISTS;
        mappingView_ = MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0,
                                     wxl_video_shared::kMappingBytes);
        if (!mappingView_)
        {
            CloseHandle(mapping_);
            mapping_ = nullptr;
            return false;
        }

        auto* header = static_cast<FrameHeader*>(mappingView_);
        if (created)
            std::memset(mappingView_, 0, wxl_video_shared::kMappingBytes);
        InitializeHeader(*header);
        InterlockedExchange(&header->hostPid, static_cast<LONG>(GetCurrentProcessId()));
        return true;
    }

    void FramePublisher::Close()
    {
        if (mappingView_)
        {
            auto* header = static_cast<FrameHeader*>(mappingView_);
            InterlockedExchange(&header->ready, 0);
            InterlockedExchange(&header->hostPid, 0);
            UnmapViewOfFile(mappingView_);
            mappingView_ = nullptr;
        }
        if (mapping_)
        {
            CloseHandle(mapping_);
            mapping_ = nullptr;
        }
    }

    bool FramePublisher::Publish(std::span<const uint8_t> bgra, uint32_t sourceStride)
    {
        if (!mappingView_ && !Open()) return false;
        auto* header = static_cast<FrameHeader*>(mappingView_);
        return PublishFrame(*header, mappingView_, bgra, sourceStride);
    }
}
