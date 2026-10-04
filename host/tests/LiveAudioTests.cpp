#include "../LiveAudio.hpp"
#include "../LiveCodec.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (false)

int main()
{
    using namespace wxl_live;

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (!StartupMediaFoundation()) return 1;

    constexpr uint32_t kSampleRate = 48000;
    constexpr uint32_t kChannels = 2;
    constexpr uint32_t kBlockAlign = kChannels * 2;
    constexpr uint32_t kSeconds = 1;
    constexpr uint32_t kTotalFrames = kSampleRate * kSeconds;

    std::vector<uint8_t> pcm(size_t(kTotalFrames) * kBlockAlign);
    for (uint32_t frame = 0; frame < kTotalFrames; ++frame)
    {
        const double time = static_cast<double>(frame) / kSampleRate;
        const int16_t sample = static_cast<int16_t>(12000.0 * std::sin(2.0 * 3.14159265358979 * 440.0 * time));
        for (uint32_t channel = 0; channel < kChannels; ++channel)
        {
            uint8_t* destination = pcm.data() + size_t(frame) * kBlockAlign + channel * 2;
            destination[0] = static_cast<uint8_t>(sample & 0xFF);
            destination[1] = static_cast<uint8_t>((sample >> 8) & 0xFF);
        }
    }

    LiveAudioEncoder encoder;
    if (!encoder.Initialize(kSampleRate, kChannels, 128000))
    {
        ShutdownMediaFoundation();
        return 2;
    }

    LiveAudioDecoder decoder;
    if (!decoder.Initialize(kSampleRate, kChannels))
    {
        ShutdownMediaFoundation();
        return 3;
    }

    std::vector<uint8_t> encoded;
    const uint32_t chunkBytes = kLiveAudioChunkFrames * kBlockAlign;
    for (size_t offset = 0; offset + chunkBytes <= pcm.size(); offset += chunkBytes)
    {
        std::vector<uint8_t> accessUnit;
        CHECK(encoder.Encode(pcm.data() + offset, chunkBytes, accessUnit));
        if (!accessUnit.empty())
        {
            CHECK(accessUnit[0] == 0xFF);
            CHECK((accessUnit[1] & 0xF0) == 0xF0);
            encoded.insert(encoded.end(), accessUnit.begin(), accessUnit.end());
        }
    }
    CHECK(encoded.size() > 2000);
    CHECK(encoded[0] == 0xFF);
    CHECK((encoded[1] & 0xF0) == 0xF0);

    std::vector<uint8_t> decoded;
    size_t offset = 0;
    while (offset + 7 <= encoded.size())
    {
        const uint32_t frameLength =
            (static_cast<uint32_t>(encoded[offset + 3] & 0x03) << 11) |
            (static_cast<uint32_t>(encoded[offset + 4]) << 3) |
            (static_cast<uint32_t>(encoded[offset + 5] & 0xE0) >> 5);
        CHECK(frameLength >= 7);
        CHECK(offset + frameLength <= encoded.size());
        std::span<const uint8_t> accessUnit(encoded.data() + offset, frameLength);
        std::vector<uint8_t> chunk;
        if (decoder.Decode(accessUnit, chunk) && !chunk.empty())
            decoded.insert(decoded.end(), chunk.begin(), chunk.end());
        offset += frameLength;
    }

    CHECK(decoded.size() > size_t(kSampleRate) * kBlockAlign / 2);

    int crossings = 0;
    const size_t startFrame = decoded.size() / kBlockAlign / 3;
    const size_t endFrame = decoded.size() / kBlockAlign * 2 / 3;
    int16_t previous = 0;
    for (size_t frame = startFrame; frame < endFrame; ++frame)
    {
        const int16_t current = static_cast<int16_t>(
            decoded[frame * kBlockAlign] | (decoded[frame * kBlockAlign + 1] << 8));
        if ((previous < 0 && current >= 0) || (previous >= 0 && current < 0)) ++crossings;
        previous = current;
    }
    const double seconds = static_cast<double>(endFrame - startFrame) / kSampleRate;
    const double frequency = crossings / 2.0 / seconds;
    CHECK(frequency > 380.0 && frequency < 500.0);

    decoder.Shutdown();
    encoder.Shutdown();
    ShutdownMediaFoundation();
    CoUninitialize();
    return 0;
}
