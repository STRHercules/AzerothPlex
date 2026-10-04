#include "LiveAudio.hpp"

#include "LiveCodec.hpp"

#include <windows.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <codecapi.h>
#include <mferror.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mmdeviceapi.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <algorithm>
#include <cstring>

namespace wxl_live
{
    namespace
    {
        using Microsoft::WRL::ComPtr;

        constexpr DWORD kInputBufferBytes = 65536;

        bool ProvidesOutputSamples(DWORD flags)
        {
            return (flags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                             MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        }

        void AppendBytes(IMFSample* sample, std::vector<uint8_t>& out, size_t padding = 0)
        {
            if (!sample) return;
            IMFMediaBuffer* media = nullptr;
            if (FAILED(sample->ConvertToContiguousBuffer(&media)) || !media) return;
            BYTE* data = nullptr;
            DWORD maximum = 0;
            DWORD current = 0;
            if (SUCCEEDED(media->Lock(&data, &maximum, &current)) && current > 0)
            {
                if (padding > 0) out.insert(out.end(), padding, 0);
                out.insert(out.end(), data, data + current);
                media->Unlock();
            }
            media->Release();
        }

        void AppendAdtsFrame(const uint8_t* payload, size_t bytes, uint32_t sampleRate,
                             uint32_t channels, std::vector<uint8_t>& out)
        {
            static const uint32_t rates[] = {
                96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
                16000, 12000, 11025, 8000, 7350,
            };
            uint32_t rateIndex = 3;
            for (uint32_t index = 0; index < std::size(rates); ++index)
            {
                if (rates[index] == sampleRate)
                {
                    rateIndex = index;
                    break;
                }
            }
            const uint32_t channelConfig = std::clamp<uint32_t>(channels, 1, 7);
            const uint32_t frameLength = static_cast<uint32_t>(bytes) + 7;
            const uint8_t header[7] = {
                0xFF,
                0xF1,
                static_cast<uint8_t>((1u << 6) | (rateIndex << 2) | ((channelConfig >> 2) & 1u)),
                static_cast<uint8_t>(((channelConfig & 3u) << 6) | ((frameLength >> 11) & 0x1Fu)),
                static_cast<uint8_t>((frameLength >> 3) & 0xFFu),
                static_cast<uint8_t>(((frameLength & 7u) << 5) | 0x1Fu),
                0xFC,
            };
            out.insert(out.end(), header, header + sizeof(header));
            out.insert(out.end(), payload, payload + bytes);
        }

        bool CreateAacEncoder(IMFTransform** transform)
        {
            *transform = nullptr;
            MFT_REGISTER_TYPE_INFO outputInfo{MFMediaType_Audio, MFAudioFormat_AAC};
            IMFActivate** activates = nullptr;
            UINT32 count = 0;
            if (FAILED(MFTEnumEx(MFT_CATEGORY_AUDIO_ENCODER,
                                 MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                                 nullptr, &outputInfo, &activates, &count)))
                return false;
            for (UINT32 index = 0; index < count && !*transform; ++index)
                activates[index]->ActivateObject(IID_PPV_ARGS(transform));
            for (UINT32 index = 0; index < count; ++index) activates[index]->Release();
            CoTaskMemFree(activates);
            return *transform != nullptr;
        }

        bool BuildAacWaveInfo(IMFMediaType* type, uint32_t sampleRate, uint32_t channels,
                              uint32_t bitrate, uint16_t payloadType)
        {
            HEAACWAVEINFO info{};
            info.wfx.wFormatTag = WAVE_FORMAT_MPEG_HEAAC;
            info.wfx.nChannels = static_cast<WORD>(channels);
            info.wfx.nSamplesPerSec = sampleRate;
            info.wfx.nAvgBytesPerSec = bitrate / 8;
            info.wfx.nBlockAlign = 1;
            info.wfx.wBitsPerSample = 0;
            info.wfx.cbSize = sizeof(HEAACWAVEINFO) - sizeof(WAVEFORMATEX);
            info.wPayloadType = payloadType;
            info.wAudioProfileLevelIndication = 0x29;
            info.wStructType = 0;
            return SUCCEEDED(type->SetBlob(MF_MT_USER_DATA,
                                           reinterpret_cast<const UINT8*>(&info), sizeof(info)));
        }

        bool TrySetType(IMFTransform* transform, IMFMediaType* type, bool input)
        {
            if (input) return SUCCEEDED(transform->SetInputType(0, type, 0));
            return SUCCEEDED(transform->SetOutputType(0, type, 0));
        }

        bool PickFirstSupportedType(IMFTransform* transform, bool input, const GUID& subtype,
                                    IMFMediaType** result)
        {
            for (DWORD index = 0;; ++index)
            {
                IMFMediaType* candidate = nullptr;
                const HRESULT hr = input ? transform->GetInputAvailableType(0, index, &candidate)
                                         : transform->GetOutputAvailableType(0, index, &candidate);
                if (FAILED(hr)) break;
                GUID candidateSubtype{};
                candidate->GetGUID(MF_MT_SUBTYPE, &candidateSubtype);
                if (candidateSubtype == subtype)
                {
                    *result = candidate;
                    return true;
                }
                candidate->Release();
            }
            return false;
        }

        class ActivationHandler final
            : public Microsoft::WRL::RuntimeClass<
                  Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                  IActivateAudioInterfaceCompletionHandler>
        {
        public:
            HRESULT RuntimeClassInitialize()
            {
                completed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                return completed ? S_OK : HRESULT_FROM_WIN32(GetLastError());
            }

            ~ActivationHandler() override
            {
                if (completed) CloseHandle(completed);
            }

            STDMETHOD(ActivateCompleted)(IActivateAudioInterfaceAsyncOperation* operation) override
            {
                HRESULT hr = E_FAIL;
                ComPtr<IUnknown> activated;
                const HRESULT request = operation->GetActivateResult(&hr, &activated);
                if (FAILED(request))
                    hr = request;
                else if (SUCCEEDED(hr) && activated)
                    hr = activated.As(&client);
                activationResult = hr;
                SetEvent(completed);
                return S_OK;
            }

            HANDLE completed = nullptr;
            HRESULT activationResult = E_FAIL;
            ComPtr<IAudioClient> client;
        };
    }

    struct LiveAudioCapture::Impl
    {
        ComPtr<IAudioClient> client;
        ComPtr<IAudioCaptureClient> capture;
        HANDLE event = nullptr;
        UINT32 bufferFrames = 0;
        WAVEFORMATEX format{};

        ~Impl()
        {
            Release();
        }

        void Release()
        {
            if (client) client->Stop();
            capture.Reset();
            client.Reset();
            if (event)
            {
                CloseHandle(event);
                event = nullptr;
            }
        }
    };

    LiveAudioCapture::LiveAudioCapture() = default;

    LiveAudioCapture::~LiveAudioCapture()
    {
        Stop();
    }

    bool LiveAudioCapture::Start(uint32_t processId)
    {
        Stop();
        auto impl = std::make_unique<Impl>();

        AUDIOCLIENT_ACTIVATION_PARAMS parameters{};
        parameters.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        parameters.ProcessLoopbackParams.TargetProcessId = processId;
        parameters.ProcessLoopbackParams.ProcessLoopbackMode =
            PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

        PROPVARIANT variant;
        PropVariantInit(&variant);
        variant.vt = VT_BLOB;
        variant.blob.cbSize = sizeof(parameters);
        variant.blob.pBlobData = reinterpret_cast<BYTE*>(&parameters);

        auto handler = Microsoft::WRL::Make<ActivationHandler>();
        if (!handler)
        {
            PropVariantClear(&variant);
            return false;
        }
        ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
        const HRESULT activation = ActivateAudioInterfaceAsync(
            VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &variant,
            handler.Get(), &operation);
        PropVariantClear(&variant);
        if (FAILED(activation)) return false;
        if (WaitForSingleObject(handler->completed, 5000) != WAIT_OBJECT_0) return false;
        if (FAILED(handler->activationResult) || !handler->client) return false;

        impl->client = handler->client;
        impl->format.wFormatTag = WAVE_FORMAT_PCM;
        impl->format.nChannels = static_cast<WORD>(kLiveAudioChannels);
        impl->format.nSamplesPerSec = kLiveAudioSampleRate;
        impl->format.wBitsPerSample = 16;
        impl->format.nBlockAlign =
            static_cast<WORD>(impl->format.nChannels * impl->format.wBitsPerSample / 8);
        impl->format.nAvgBytesPerSec =
            impl->format.nSamplesPerSec * impl->format.nBlockAlign;
        impl->format.cbSize = 0;

        HRESULT hr = impl->client->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            200000, 0, &impl->format, nullptr);
        if (FAILED(hr)) return false;

        impl->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!impl->event) return false;
        if (FAILED(impl->client->SetEventHandle(impl->event))) return false;
        if (FAILED(impl->client->GetBufferSize(&impl->bufferFrames))) return false;
        if (FAILED(impl->client->GetService(IID_PPV_ARGS(&impl->capture)))) return false;
        if (FAILED(impl->client->Start())) return false;

        impl_ = std::move(impl);
        running_.store(true);
        thread_ = std::jthread([this](std::stop_token stop) { CaptureLoop(stop); });
        return true;
    }

    void LiveAudioCapture::Stop()
    {
        running_.store(false);
        if (thread_.joinable())
        {
            thread_.request_stop();
            thread_.join();
        }
        if (impl_)
        {
            impl_->Release();
            impl_.reset();
        }
        std::lock_guard lock(mutex_);
        queue_.clear();
    }

    void LiveAudioCapture::CaptureLoop(std::stop_token stop)
    {
        while (!stop.stop_requested() && running_.load() && impl_)
        {
            if (WaitForSingleObject(impl_->event, 100) != WAIT_OBJECT_0) continue;
            UINT32 packetFrames = 0;
            while (!stop.stop_requested() &&
                   SUCCEEDED(impl_->capture->GetNextPacketSize(&packetFrames)) &&
                   packetFrames > 0)
            {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                if (FAILED(impl_->capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr)))
                    break;
                const size_t bytes = size_t(frames) * impl_->format.nBlockAlign;
                {
                    std::lock_guard lock(mutex_);
                    if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
                        queue_.insert(queue_.end(), bytes, 0);
                    else
                        queue_.insert(queue_.end(), data, data + bytes);
                    constexpr size_t kMaximumQueued =
                        size_t(kLiveAudioSampleRate) * kLiveAudioChannels * 2 * 2;
                    while (queue_.size() > kMaximumQueued) queue_.pop_front();
                }
                impl_->capture->ReleaseBuffer(frames);
            }
        }
    }

    size_t LiveAudioCapture::Read(uint8_t* destination, size_t maximumBytes)
    {
        if (!destination || maximumBytes == 0) return 0;
        std::lock_guard lock(mutex_);
        const size_t count = std::min(maximumBytes, queue_.size());
        for (size_t index = 0; index < count; ++index)
        {
            destination[index] = queue_.front();
            queue_.pop_front();
        }
        return count;
    }

    LiveAudioEncoder::LiveAudioEncoder() = default;

    LiveAudioEncoder::~LiveAudioEncoder()
    {
        Shutdown();
    }

    void LiveAudioEncoder::Shutdown()
    {
        if (encoder_) encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        if (outputType_) { outputType_->Release(); outputType_ = nullptr; }
        if (inputType_) { inputType_->Release(); inputType_ = nullptr; }
        if (encoder_) { encoder_->Release(); encoder_ = nullptr; }
        nextSampleTime_ = 0;
        lastDuration_ = 0;
    }

    bool LiveAudioEncoder::Initialize(uint32_t sampleRate, uint32_t channels, uint32_t bitrate)
    {
        Shutdown();
        if (sampleRate == 0 || channels == 0 || bitrate == 0) return false;
        sampleRate_ = sampleRate;
        channels_ = channels;
        bitrate_ = bitrate;

        if (!CreateAacEncoder(&encoder_)) return false;

        IMFAttributes* attributes = nullptr;
        if (SUCCEEDED(encoder_->GetAttributes(&attributes)))
        {
            attributes->SetUINT32(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
            attributes->SetUINT32(CODECAPI_AVEncCommonMeanBitRate, bitrate);
            attributes->Release();
        }

        auto makeOutputType = [&](const GUID& subtype, uint16_t payloadType,
                                  bool withWaveInfo) -> IMFMediaType*
        {
            IMFMediaType* type = nullptr;
            if (FAILED(MFCreateMediaType(&type))) return nullptr;
            type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            type->SetGUID(MF_MT_SUBTYPE, subtype);
            type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate);
            type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
            type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bitrate / 8);
            type->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, payloadType);
            type->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
            type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, FALSE);
            if (withWaveInfo) BuildAacWaveInfo(type, sampleRate, channels, bitrate, payloadType);
            return type;
        };

        outputType_ = makeOutputType(MFAudioFormat_AAC, 1, true);
        if (!outputType_ || FAILED(encoder_->SetOutputType(0, outputType_, 0)))
        {
            if (outputType_) { outputType_->Release(); outputType_ = nullptr; }
            outputType_ = makeOutputType(MFAudioFormat_ADTS, 0, false);
            if (!outputType_ || FAILED(encoder_->SetOutputType(0, outputType_, 0)))
            {
                if (outputType_) { outputType_->Release(); outputType_ = nullptr; }
                outputType_ = makeOutputType(MFAudioFormat_AAC, 0, true);
                if (!outputType_ || FAILED(encoder_->SetOutputType(0, outputType_, 0)))
                {
                    Shutdown();
                    return false;
                }
            }
        }

        UpdateOutputMode();

        if (FAILED(MFCreateMediaType(&inputType_))) { Shutdown(); return false; }
        inputType_->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        inputType_->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        inputType_->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        inputType_->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate);
        inputType_->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
        inputType_->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, channels * 2);
        inputType_->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, sampleRate * channels * 2);
        inputType_->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
        if (FAILED(encoder_->SetInputType(0, inputType_, 0))) { Shutdown(); return false; }

        encoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        if (FAILED(encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0)) ||
            FAILED(encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0)))
        {
            Shutdown();
            return false;
        }
        nextSampleTime_ = 0;
        lastDuration_ = 0;
        return true;
    }

    void LiveAudioEncoder::UpdateOutputMode()
    {
        wrapAdts_ = true;
        if (!outputType_) return;
        GUID subtype{};
        if (FAILED(outputType_->GetGUID(MF_MT_SUBTYPE, &subtype))) return;
        if (subtype == MFAudioFormat_ADTS)
        {
            wrapAdts_ = false;
            return;
        }
        UINT32 payload = 0;
        if (subtype == MFAudioFormat_AAC &&
            SUCCEEDED(outputType_->GetUINT32(MF_MT_AAC_PAYLOAD_TYPE, &payload)))
            wrapAdts_ = payload == 0;
    }

    bool LiveAudioEncoder::DrainOutput(std::vector<uint8_t>& accessUnit)
    {
        for (;;)
        {
            MFT_OUTPUT_STREAM_INFO info{};
            if (FAILED(encoder_->GetOutputStreamInfo(0, &info))) return false;

            MFT_OUTPUT_DATA_BUFFER output{};
            IMFSample* sample = nullptr;
            if (!ProvidesOutputSamples(info.dwFlags))
            {
                const DWORD size = std::max<DWORD>(info.cbSize, kInputBufferBytes);
                IMFMediaBuffer* buffer = nullptr;
                if (FAILED(MFCreateMemoryBuffer(size, &buffer))) return false;
                if (FAILED(MFCreateSample(&sample)) || FAILED(sample->AddBuffer(buffer)))
                {
                    buffer->Release();
                    if (sample) sample->Release();
                    return false;
                }
                buffer->Release();
                output.pSample = sample;
            }

            DWORD status = 0;
            const HRESULT result = encoder_->ProcessOutput(0, 1, &output, &status);
            if (output.pEvents) output.pEvents->Release();

            if (result == MF_E_TRANSFORM_NEED_MORE_INPUT)
            {
                if (sample) sample->Release();
                return true;
            }
            if (result == MF_E_TRANSFORM_STREAM_CHANGE)
            {
                if (sample) sample->Release();
                IMFMediaType* type = nullptr;
                if (FAILED(encoder_->GetOutputAvailableType(0, 0, &type))) return false;
                if (outputType_) outputType_->Release();
                outputType_ = type;
                if (FAILED(encoder_->SetOutputType(0, outputType_, 0))) return false;
                UpdateOutputMode();
                continue;
            }
            if (FAILED(result))
            {
                if (sample) sample->Release();
                return false;
            }

            if (wrapAdts_)
            {
                IMFMediaBuffer* media = nullptr;
                if (SUCCEEDED(output.pSample->ConvertToContiguousBuffer(&media)) && media)
                {
                    BYTE* data = nullptr;
                    DWORD maximum = 0;
                    DWORD current = 0;
                    if (SUCCEEDED(media->Lock(&data, &maximum, &current)) && current > 0)
                    {
                        AppendAdtsFrame(data, current, sampleRate_, channels_, accessUnit);
                        media->Unlock();
                    }
                    media->Release();
                }
            }
            else
            {
                AppendBytes(output.pSample, accessUnit);
            }

            if (output.pSample) output.pSample->Release();
            if (sample && sample != output.pSample) sample->Release();
        }
    }

    bool LiveAudioEncoder::Encode(const uint8_t* pcm, uint32_t bytes, std::vector<uint8_t>& accessUnit)
    {
        accessUnit.clear();
        if (!encoder_ || !pcm || bytes == 0) return false;
        const uint32_t blockAlign = channels_ * 2;
        if (bytes < blockAlign) return false;

        IMFMediaBuffer* buffer = nullptr;
        if (FAILED(MFCreateMemoryBuffer(bytes, &buffer))) return false;
        BYTE* data = nullptr;
        DWORD maximum = 0;
        DWORD current = 0;
        if (FAILED(buffer->Lock(&data, &maximum, &current)) || maximum < bytes)
        {
            buffer->Release();
            return false;
        }
        std::memcpy(data, pcm, bytes);
        buffer->Unlock();
        buffer->SetCurrentLength(bytes);

        IMFSample* sample = nullptr;
        if (FAILED(MFCreateSample(&sample)) || FAILED(sample->AddBuffer(buffer)))
        {
            if (sample) sample->Release();
            buffer->Release();
            return false;
        }
        buffer->Release();
        const int64_t duration =
            static_cast<int64_t>(bytes / blockAlign) * 10'000'000LL / sampleRate_;
        sample->SetSampleTime(nextSampleTime_);
        sample->SetSampleDuration(duration);
        lastDuration_ = duration;

        HRESULT result = encoder_->ProcessInput(0, sample, 0);
        if (result == MF_E_NOTACCEPTING)
        {
            if (!DrainOutput(accessUnit))
            {
                sample->Release();
                return false;
            }
            result = encoder_->ProcessInput(0, sample, 0);
        }
        sample->Release();
        if (FAILED(result)) return false;

        nextSampleTime_ += lastDuration_;
        return DrainOutput(accessUnit);
    }

    LiveAudioDecoder::LiveAudioDecoder() = default;

    LiveAudioDecoder::~LiveAudioDecoder()
    {
        Shutdown();
    }

    void LiveAudioDecoder::Shutdown()
    {
        if (decoder_) decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        if (outputType_) { outputType_->Release(); outputType_ = nullptr; }
        if (inputType_) { inputType_->Release(); inputType_ = nullptr; }
        if (decoder_) { decoder_->Release(); decoder_ = nullptr; }
    }

    bool LiveAudioDecoder::SelectOutputType()
    {
        IMFMediaType* type = nullptr;
        if (FAILED(MFCreateMediaType(&type))) return false;
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate_);
        type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels_);
        type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, channels_ * 2);
        type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, sampleRate_ * channels_ * 2);
        HRESULT result = decoder_->SetOutputType(0, type, 0);
        if (FAILED(result))
        {
            type->Release();
            type = nullptr;
            if (PickFirstSupportedType(decoder_, false, MFAudioFormat_PCM, &type))
                result = decoder_->SetOutputType(0, type, 0);
        }
        if (!type || FAILED(result))
        {
            if (type) type->Release();
            return false;
        }
        if (outputType_) outputType_->Release();
        outputType_ = type;
        return true;
    }

    bool LiveAudioDecoder::Initialize(uint32_t sampleRate, uint32_t channels)
    {
        Shutdown();
        if (sampleRate == 0 || channels == 0) return false;
        sampleRate_ = sampleRate;
        channels_ = channels;
        if (FAILED(CoCreateInstance(CLSID_CMSAACDecMFT, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&decoder_))))
            return false;

        if (FAILED(MFCreateMediaType(&inputType_))) { Shutdown(); return false; }
        inputType_->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        inputType_->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_ADTS);
        inputType_->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate);
        inputType_->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
        HRESULT result = decoder_->SetInputType(0, inputType_, 0);
        if (FAILED(result))
        {
            inputType_->Release();
            inputType_ = nullptr;
            if (PickFirstSupportedType(decoder_, true, MFAudioFormat_ADTS, &inputType_))
                result = decoder_->SetInputType(0, inputType_, 0);
            else
            {
                Shutdown();
                return false;
            }
        }
        if (FAILED(result) || !SelectOutputType())
        {
            Shutdown();
            return false;
        }

        decoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        if (FAILED(decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0)) ||
            FAILED(decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0)))
        {
            Shutdown();
            return false;
        }
        return true;
    }

    bool LiveAudioDecoder::DrainOutput(std::vector<uint8_t>& pcm, bool& produced)
    {
        for (;;)
        {
            MFT_OUTPUT_STREAM_INFO info{};
            if (FAILED(decoder_->GetOutputStreamInfo(0, &info))) return false;

            MFT_OUTPUT_DATA_BUFFER output{};
            IMFSample* sample = nullptr;
            if (!ProvidesOutputSamples(info.dwFlags))
            {
                const DWORD size = std::max<DWORD>(info.cbSize, kInputBufferBytes);
                IMFMediaBuffer* buffer = nullptr;
                if (FAILED(MFCreateMemoryBuffer(size, &buffer))) return false;
                if (FAILED(MFCreateSample(&sample)) || FAILED(sample->AddBuffer(buffer)))
                {
                    buffer->Release();
                    if (sample) sample->Release();
                    return false;
                }
                buffer->Release();
                output.pSample = sample;
            }

            DWORD status = 0;
            const HRESULT result = decoder_->ProcessOutput(0, 1, &output, &status);
            if (output.pEvents) output.pEvents->Release();

            if (result == MF_E_TRANSFORM_NEED_MORE_INPUT)
            {
                if (sample) sample->Release();
                return true;
            }
            if (result == MF_E_TRANSFORM_STREAM_CHANGE)
            {
                if (sample) sample->Release();
                if (!SelectOutputType()) return false;
                continue;
            }
            if (FAILED(result))
            {
                if (sample) sample->Release();
                return false;
            }

            IMFMediaBuffer* media = nullptr;
            if (SUCCEEDED(output.pSample->ConvertToContiguousBuffer(&media)) && media)
            {
                BYTE* data = nullptr;
                DWORD maximum = 0;
                DWORD current = 0;
                if (SUCCEEDED(media->Lock(&data, &maximum, &current)) && current > 0)
                {
                    pcm.insert(pcm.end(), data, data + current);
                    media->Unlock();
                    produced = true;
                }
                media->Release();
            }

            if (output.pSample) output.pSample->Release();
            if (sample && sample != output.pSample) sample->Release();
        }
    }

    bool LiveAudioDecoder::Decode(std::span<const uint8_t> accessUnit, std::vector<uint8_t>& pcm)
    {
        pcm.clear();
        if (!decoder_ || accessUnit.empty()) return false;

        IMFMediaBuffer* buffer = nullptr;
        if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(accessUnit.size()), &buffer)))
            return false;
        BYTE* data = nullptr;
        DWORD maximum = 0;
        DWORD current = 0;
        if (FAILED(buffer->Lock(&data, &maximum, &current)) || maximum < accessUnit.size())
        {
            buffer->Release();
            return false;
        }
        std::memcpy(data, accessUnit.data(), accessUnit.size());
        buffer->Unlock();
        buffer->SetCurrentLength(static_cast<DWORD>(accessUnit.size()));

        IMFSample* sample = nullptr;
        if (FAILED(MFCreateSample(&sample)) || FAILED(sample->AddBuffer(buffer)))
        {
            if (sample) sample->Release();
            buffer->Release();
            return false;
        }
        buffer->Release();
        sample->SetSampleTime(0);
        sample->SetSampleDuration(0);

        HRESULT result = decoder_->ProcessInput(0, sample, 0);
        if (result == MF_E_NOTACCEPTING)
        {
            bool drained = false;
            if (!DrainOutput(pcm, drained))
            {
                sample->Release();
                return false;
            }
            result = decoder_->ProcessInput(0, sample, 0);
        }
        sample->Release();
        if (FAILED(result)) return false;

        bool produced = false;
        if (!DrainOutput(pcm, produced)) return false;
        return produced;
    }

    struct LiveAudioOutput::Impl
    {
        ComPtr<IAudioClient> client;
        ComPtr<IAudioRenderClient> render;
        ComPtr<ISimpleAudioVolume> volume;
        HANDLE event = nullptr;
        UINT32 bufferFrames = 0;
        WAVEFORMATEX format{};

        ~Impl()
        {
            Release();
        }

        void Release()
        {
            if (client) client->Stop();
            volume.Reset();
            render.Reset();
            client.Reset();
            if (event)
            {
                CloseHandle(event);
                event = nullptr;
            }
        }
    };

    LiveAudioOutput::LiveAudioOutput() = default;

    LiveAudioOutput::~LiveAudioOutput()
    {
        Stop();
    }

    bool LiveAudioOutput::Start(uint32_t sampleRate, uint32_t channels)
    {
        Stop();
        auto impl = std::make_unique<Impl>();

        ComPtr<IMMDeviceEnumerator> enumerator;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    IID_PPV_ARGS(&enumerator))))
            return false;
        ComPtr<IMMDevice> device;
        if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) return false;
        if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void**>(impl->client.GetAddressOf()))))
            return false;

        impl->format.wFormatTag = WAVE_FORMAT_PCM;
        impl->format.nChannels = static_cast<WORD>(channels);
        impl->format.nSamplesPerSec = sampleRate;
        impl->format.wBitsPerSample = 16;
        impl->format.nBlockAlign =
            static_cast<WORD>(impl->format.nChannels * impl->format.wBitsPerSample / 8);
        impl->format.nAvgBytesPerSec = impl->format.nSamplesPerSec * impl->format.nBlockAlign;
        impl->format.cbSize = 0;

        HRESULT hr = impl->client->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
            200000, 0, &impl->format, nullptr);
        if (FAILED(hr)) return false;

        impl->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!impl->event) return false;
        if (FAILED(impl->client->SetEventHandle(impl->event))) return false;
        if (FAILED(impl->client->GetBufferSize(&impl->bufferFrames))) return false;
        if (FAILED(impl->client->GetService(IID_PPV_ARGS(&impl->render)))) return false;
        impl->client->GetService(IID_PPV_ARGS(&impl->volume));

        impl_ = std::move(impl);
        if (impl_->volume)
            impl_->volume->SetMasterVolume(
                std::clamp(volumePercent_, 0, 100) / 100.0f, nullptr);
        if (FAILED(impl_->client->Start())) return false;

        running_.store(true);
        thread_ = std::jthread([this](std::stop_token stop) { RenderLoop(stop); });
        return true;
    }

    void LiveAudioOutput::Stop()
    {
        running_.store(false);
        if (thread_.joinable())
        {
            thread_.request_stop();
            thread_.join();
        }
        if (impl_)
        {
            impl_->Release();
            impl_.reset();
        }
        std::lock_guard lock(mutex_);
        queue_.clear();
    }

    void LiveAudioOutput::RenderLoop(std::stop_token stop)
    {
        while (!stop.stop_requested() && running_.load() && impl_)
        {
            if (WaitForSingleObject(impl_->event, 100) != WAIT_OBJECT_0) continue;
            UINT32 padding = 0;
            if (FAILED(impl_->client->GetCurrentPadding(&padding))) break;
            const UINT32 available = impl_->bufferFrames > padding
                ? impl_->bufferFrames - padding : 0;
            if (available == 0) continue;
            BYTE* data = nullptr;
            if (FAILED(impl_->render->GetBuffer(available, &data))) break;
            const size_t wanted = size_t(available) * impl_->format.nBlockAlign;
            size_t copied = 0;
            {
                std::lock_guard lock(mutex_);
                copied = std::min(wanted, queue_.size());
                for (size_t index = 0; index < copied; ++index)
                {
                    data[index] = queue_.front();
                    queue_.pop_front();
                }
            }
            if (copied < wanted) std::memset(data + copied, 0, wanted - copied);
            impl_->render->ReleaseBuffer(available, 0);
        }
    }

    void LiveAudioOutput::Submit(const uint8_t* pcm, size_t bytes)
    {
        if (!pcm || bytes == 0) return;
        std::lock_guard lock(mutex_);
        queue_.insert(queue_.end(), pcm, pcm + bytes);
        constexpr size_t kMaximumQueued =
            size_t(kLiveAudioSampleRate) * kLiveAudioChannels * 2 / 2;
        while (queue_.size() > kMaximumQueued) queue_.pop_front();
    }

    void LiveAudioOutput::SetVolumePercent(int percent)
    {
        volumePercent_ = std::clamp(percent, 0, 100);
        if (impl_ && impl_->volume)
            impl_->volume->SetMasterVolume(volumePercent_ / 100.0f, nullptr);
    }
}
