#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

struct IMFMediaType;
struct IMFTransform;

namespace wxl_live
{
    constexpr uint32_t kLiveAudioSampleRate = 48000;
    constexpr uint32_t kLiveAudioChannels = 2;
    constexpr uint32_t kLiveAudioBitrate = 128000;
    constexpr uint32_t kLiveAudioChunkFrames = 960;

    class LiveAudioCapture final
    {
    public:
        LiveAudioCapture();
        ~LiveAudioCapture();

        LiveAudioCapture(const LiveAudioCapture&) = delete;
        LiveAudioCapture& operator=(const LiveAudioCapture&) = delete;

        bool Start(uint32_t processId);
        void Stop();
        bool IsRunning() const { return running_.load(); }
        size_t Read(uint8_t* destination, size_t maximumBytes);

    private:
        struct Impl;
        void CaptureLoop(std::stop_token stop);

        std::unique_ptr<Impl> impl_;
        std::jthread thread_;
        std::atomic<bool> running_{false};
        std::mutex mutex_;
        std::deque<uint8_t> queue_;
    };

    class LiveAudioEncoder final
    {
    public:
        LiveAudioEncoder();
        ~LiveAudioEncoder();

        LiveAudioEncoder(const LiveAudioEncoder&) = delete;
        LiveAudioEncoder& operator=(const LiveAudioEncoder&) = delete;

        bool Initialize(uint32_t sampleRate = kLiveAudioSampleRate,
                        uint32_t channels = kLiveAudioChannels,
                        uint32_t bitrate = kLiveAudioBitrate);
        void Shutdown();
        bool IsInitialized() const { return encoder_ != nullptr; }

        bool Encode(const uint8_t* pcm, uint32_t bytes, std::vector<uint8_t>& accessUnit);

    private:
        bool DrainOutput(std::vector<uint8_t>& accessUnit);
        void UpdateOutputMode();

        IMFTransform* encoder_ = nullptr;
        IMFMediaType* inputType_ = nullptr;
        IMFMediaType* outputType_ = nullptr;
        uint32_t sampleRate_ = kLiveAudioSampleRate;
        uint32_t channels_ = kLiveAudioChannels;
        uint32_t bitrate_ = kLiveAudioBitrate;
        bool wrapAdts_ = true;
        int64_t nextSampleTime_ = 0;
        int64_t lastDuration_ = 0;
    };

    class LiveAudioDecoder final
    {
    public:
        LiveAudioDecoder();
        ~LiveAudioDecoder();

        LiveAudioDecoder(const LiveAudioDecoder&) = delete;
        LiveAudioDecoder& operator=(const LiveAudioDecoder&) = delete;

        bool Initialize(uint32_t sampleRate = kLiveAudioSampleRate,
                        uint32_t channels = kLiveAudioChannels);
        void Shutdown();
        bool IsInitialized() const { return decoder_ != nullptr; }

        bool Decode(std::span<const uint8_t> accessUnit, std::vector<uint8_t>& pcm);

    private:
        bool SelectOutputType();
        bool DrainOutput(std::vector<uint8_t>& pcm, bool& produced);

        IMFTransform* decoder_ = nullptr;
        IMFMediaType* inputType_ = nullptr;
        IMFMediaType* outputType_ = nullptr;
        uint32_t sampleRate_ = kLiveAudioSampleRate;
        uint32_t channels_ = kLiveAudioChannels;
    };

    class LiveAudioOutput final
    {
    public:
        LiveAudioOutput();
        ~LiveAudioOutput();

        LiveAudioOutput(const LiveAudioOutput&) = delete;
        LiveAudioOutput& operator=(const LiveAudioOutput&) = delete;

        bool Start(uint32_t sampleRate = kLiveAudioSampleRate,
                   uint32_t channels = kLiveAudioChannels);
        void Stop();
        bool IsRunning() const { return running_.load(); }
        void Submit(const uint8_t* pcm, size_t bytes);
        void SetVolumePercent(int percent);

    private:
        struct Impl;
        void RenderLoop(std::stop_token stop);

        std::unique_ptr<Impl> impl_;
        std::jthread thread_;
        std::atomic<bool> running_{false};
        std::mutex mutex_;
        std::deque<uint8_t> queue_;
        int volumePercent_ = 100;
    };
}
