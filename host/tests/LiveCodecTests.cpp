#include "../LiveCodec.hpp"

#include <algorithm>
#include <cstdlib>
#include <vector>

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (false)

int main()
{
    using namespace wxl_live;

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (!StartupMediaFoundation()) return 1;

    constexpr uint32_t kWidth = 320;
    constexpr uint32_t kHeight = 180;
    constexpr uint32_t kStride = kWidth * 4;

    std::vector<uint8_t> frame(size_t(kStride) * kHeight);
    for (uint32_t row = 0; row < kHeight; ++row)
    {
        for (uint32_t column = 0; column < kWidth; ++column)
        {
            const bool block = row < 48 && column < 48;
            uint8_t* pixel = frame.data() + size_t(row) * kStride + size_t(column) * 4;
            pixel[0] = block ? 30 : 220;
            pixel[1] = block ? 60 : 160;
            pixel[2] = block ? 220 : 80;
            pixel[3] = 255;
        }
    }

    {
        std::vector<uint8_t> y(size_t(kWidth) * kHeight);
        std::vector<uint8_t> uv(size_t(kWidth / 2) * (kHeight / 2) * 2);
        std::vector<uint8_t> back(size_t(kStride) * kHeight);
        BgraToNv12(frame.data(), kStride, y.data(), kWidth, uv.data(), kWidth, kWidth, kHeight);
        Nv12ToBgra(y.data(), kWidth, uv.data(), kWidth, back.data(), kStride, kWidth, kHeight);
        const size_t center = (size_t(kHeight / 2) * kWidth + kWidth / 2) * 4;
        CHECK(std::abs(int(back[0]) - 30) <= 3);
        CHECK(std::abs(int(back[2]) - 220) <= 3);
        CHECK(std::abs(int(back[center + 0]) - 220) <= 3);
        CHECK(std::abs(int(back[center + 1]) - 160) <= 3);
        CHECK(std::abs(int(back[center + 2]) - 80) <= 3);
        CHECK(back[3] == 255);
    }

    LiveEncoder encoder;
    if (!encoder.Initialize(kWidth, kHeight, 30, 600000, 30))
    {
        ShutdownMediaFoundation();
        return 2;
    }

    LiveDecoder decoder;
    if (!decoder.Initialize())
    {
        ShutdownMediaFoundation();
        return 3;
    }

    int decodedFrames = 0;
    for (uint32_t index = 0; index < 60; ++index)
    {
        std::vector<uint8_t> accessUnit;
        uint32_t sequence = 0;
        bool keyframe = false;
        CHECK(encoder.Encode(frame.data(), kStride, accessUnit, sequence, keyframe));
        if (accessUnit.empty()) continue;

        std::vector<uint8_t> packet = accessUnit;
        WriteFrameHeader(packet, keyframe, sequence);

        FrameHeader header{};
        CHECK(ReadFrameHeader(packet, header));
        CHECK(header.sequence == sequence);
        CHECK((header.flags & kFrameFlagKeyframe) == (keyframe ? kFrameFlagKeyframe : 0));

        std::span<const uint8_t> payload(packet.data() + kFrameHeaderBytes,
                                         packet.size() - kFrameHeaderBytes);
        std::vector<uint8_t> bgra;
        uint32_t width = 0;
        uint32_t height = 0;
        if (!decoder.Decode(payload, header.sequence, bgra, width, height)) continue;

        CHECK(width == kWidth);
        CHECK(height == kHeight);
        CHECK(bgra.size() == size_t(kWidth) * kHeight * 4);

        const uint8_t* background = bgra.data() + size_t(kHeight / 2) * kWidth * 4 + size_t(kWidth / 2) * 4;
        const uint8_t* foreground = bgra.data() + size_t(16) * kWidth * 4 + size_t(16) * 4;
        CHECK(std::abs(int(background[0]) - 220) <= 40);
        CHECK(std::abs(int(background[1]) - 160) <= 40);
        CHECK(std::abs(int(background[2]) - 80) <= 40);
        CHECK(std::abs(int(foreground[0]) - 30) <= 40);
        CHECK(std::abs(int(foreground[2]) - 220) <= 40);
        ++decodedFrames;
    }
    CHECK(decodedFrames >= 40);

    decoder.Shutdown();
    encoder.Shutdown();
    ShutdownMediaFoundation();
    CoUninitialize();
    return 0;
}
