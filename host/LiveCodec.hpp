#pragma once

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mftransform.h>

#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace wxl_live
{
    constexpr uint32_t kFrameMagic = 0x564C5857u;
    constexpr uint8_t kFrameVersion = 1;
    constexpr uint8_t kFrameFlagKeyframe = 0x01;
    constexpr uint8_t kFrameFlagAudio = 0x02;
    constexpr size_t kFrameHeaderBytes = 12;

#pragma pack(push, 1)
    struct FrameHeader
    {
        uint32_t magic;
        uint8_t version;
        uint8_t flags;
        uint16_t reserved;
        uint32_t sequence;
    };
#pragma pack(pop)
    static_assert(sizeof(FrameHeader) == 12, "live frame header ABI changed");

    inline void WriteFrameHeader(std::vector<uint8_t>& packet, bool keyframe, uint32_t sequence,
                                 bool audio = false)
    {
        uint8_t flags = keyframe ? kFrameFlagKeyframe : 0;
        if (audio) flags |= kFrameFlagAudio;
        const FrameHeader header{kFrameMagic, kFrameVersion, flags, 0, sequence};
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&header);
        packet.insert(packet.begin(), bytes, bytes + sizeof(header));
    }

    inline bool ReadFrameHeader(std::span<const uint8_t> packet, FrameHeader& header)
    {
        if (packet.size() < kFrameHeaderBytes) return false;
        std::memcpy(&header, packet.data(), sizeof(header));
        return header.magic == kFrameMagic && header.version == kFrameVersion;
    }

    bool StartupMediaFoundation();
    void ShutdownMediaFoundation();

    void BgraToNv12(const uint8_t* bgra, uint32_t sourceStride,
                    uint8_t* y, uint32_t yStride, uint8_t* uv, uint32_t uvStride,
                    uint32_t width, uint32_t height);
    void Nv12ToBgra(const uint8_t* y, uint32_t yStride, const uint8_t* uv, uint32_t uvStride,
                    uint8_t* bgra, uint32_t destinationStride,
                    uint32_t width, uint32_t height);

    class LiveEncoder final
    {
    public:
        LiveEncoder();
        ~LiveEncoder();

        LiveEncoder(const LiveEncoder&) = delete;
        LiveEncoder& operator=(const LiveEncoder&) = delete;

        bool Initialize(uint32_t width, uint32_t height, uint32_t fps,
                        uint32_t bitrate, uint32_t keyframeIntervalFrames);
        void Shutdown();
        bool IsInitialized() const { return encoder_ != nullptr; }

        bool Encode(const uint8_t* bgra, uint32_t stride,
                    std::vector<uint8_t>& accessUnit, uint32_t& sequence, bool& keyframe);

    private:
        IMFSample* CreateInputSample(const uint8_t* bgra, uint32_t stride, int64_t sampleTime);
        bool DrainOutput(std::vector<uint8_t>& accessUnit, bool& keyframe);
        bool NegotiateOutputType();

        IMFTransform* encoder_ = nullptr;
        IMFMediaType* inputType_ = nullptr;
        IMFMediaType* outputType_ = nullptr;
        uint32_t width_ = 0;
        uint32_t height_ = 0;
        uint32_t fps_ = 30;
        uint32_t bitrate_ = 0;
        uint32_t keyframeInterval_ = 60;
        int64_t frameDuration_ = 333333;
        int64_t frameIndex_ = 0;
        uint32_t sequence_ = 0;
    };

    class LiveDecoder final
    {
    public:
        LiveDecoder();
        ~LiveDecoder();

        LiveDecoder(const LiveDecoder&) = delete;
        LiveDecoder& operator=(const LiveDecoder&) = delete;

        bool Initialize();
        void Shutdown();
        bool IsInitialized() const { return decoder_ != nullptr; }

        bool Decode(std::span<const uint8_t> accessUnit, uint32_t sequence,
                    std::vector<uint8_t>& bgra, uint32_t& width, uint32_t& height);

    private:
        IMFSample* CreateInputSample(std::span<const uint8_t> accessUnit, int64_t sampleTime);
        void ReadFrameSize();
        bool SelectOutputType();
        bool DrainOutput(std::vector<uint8_t>& bgra, uint32_t& width, uint32_t& height,
                         bool& produced);

        IMFTransform* decoder_ = nullptr;
        IMFMediaType* inputType_ = nullptr;
        IMFMediaType* outputType_ = nullptr;
        uint32_t width_ = 0;
        uint32_t height_ = 0;
        uint32_t displayWidth_ = 0;
        uint32_t displayHeight_ = 0;
        bool apertureResolved_ = false;
        int64_t frameDuration_ = 333333;
        int64_t frameIndex_ = 0;
    };
}
