#include "LiveCodec.hpp"

#include "wmcodecdsp.h"
#include "codecapi.h"
#include "mferror.h"

#include <algorithm>
#include <atomic>
#include <cstring>

namespace wxl_live
{
    namespace
    {
        std::atomic<int> g_mfRefs{0};

        inline uint8_t ClampByte(int value)
        {
            return static_cast<uint8_t>(std::clamp(value, 0, 255));
        }

        inline bool ProvidesOutputSamples(DWORD flags)
        {
            return (flags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                             MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        }

        bool AppendSampleBytes(IMFSample* sample, std::vector<uint8_t>& out)
        {
            if (!sample) return false;
            IMFMediaBuffer* media = nullptr;
            if (FAILED(sample->ConvertToContiguousBuffer(&media)) || !media) return false;
            BYTE* data = nullptr;
            DWORD maximum = 0;
            DWORD current = 0;
            bool copied = false;
            if (SUCCEEDED(media->Lock(&data, &maximum, &current)) && current > 0)
            {
                out.insert(out.end(), data, data + current);
                copied = true;
                media->Unlock();
            }
            media->Release();
            return copied;
        }
    }

    bool StartupMediaFoundation()
    {
        if (g_mfRefs.fetch_add(1) == 0)
        {
            if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE)))
            {
                g_mfRefs.store(0);
                return false;
            }
        }
        return true;
    }

    void ShutdownMediaFoundation()
    {
        if (g_mfRefs.fetch_sub(1) == 1)
            MFShutdown();
    }

    void BgraToNv12(const uint8_t* bgra, uint32_t sourceStride,
                    uint8_t* y, uint32_t yStride, uint8_t* uv, uint32_t uvStride,
                    uint32_t width, uint32_t height)
    {
        if (!bgra || !y || !uv || width == 0 || height == 0) return;
        for (uint32_t row = 0; row < height; ++row)
        {
            const uint8_t* source = bgra + size_t(row) * sourceStride;
            uint8_t* target = y + size_t(row) * yStride;
            for (uint32_t column = 0; column < width; ++column)
            {
                const int b = source[column * 4 + 0];
                const int g = source[column * 4 + 1];
                const int r = source[column * 4 + 2];
                target[column] = ClampByte(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
            }
        }
        for (uint32_t row = 0; row + 1 < height; row += 2)
        {
            const uint8_t* source = bgra + size_t(row) * sourceStride;
            uint8_t* target = uv + size_t(row / 2) * uvStride;
            for (uint32_t column = 0; column + 1 < width; column += 2)
            {
                const uint8_t* pixel = source + size_t(column) * 4;
                const int b = (pixel[0] + pixel[4] + pixel[sourceStride] + pixel[sourceStride + 4]) / 4;
                const int g = (pixel[1] + pixel[5] + pixel[sourceStride + 1] + pixel[sourceStride + 5]) / 4;
                const int r = (pixel[2] + pixel[6] + pixel[sourceStride + 2] + pixel[sourceStride + 6]) / 4;
                target[column] = ClampByte(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
                target[column + 1] = ClampByte(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
            }
        }
    }

    void Nv12ToBgra(const uint8_t* y, uint32_t yStride, const uint8_t* uv, uint32_t uvStride,
                    uint8_t* bgra, uint32_t destinationStride,
                    uint32_t width, uint32_t height)
    {
        if (!y || !uv || !bgra || width == 0 || height == 0) return;
        for (uint32_t row = 0; row < height; ++row)
        {
            const uint8_t* yRow = y + size_t(row) * yStride;
            const uint8_t* uvRow = uv + size_t(row / 2) * uvStride;
            uint8_t* target = bgra + size_t(row) * destinationStride;
            for (uint32_t column = 0; column < width; ++column)
            {
                const int c = std::max(0, static_cast<int>(yRow[column]) - 16);
                const int d = static_cast<int>(uvRow[(column / 2) * 2]) - 128;
                const int e = static_cast<int>(uvRow[(column / 2) * 2 + 1]) - 128;
                target[column * 4 + 0] = ClampByte((298 * c + 516 * d + 128) >> 8);
                target[column * 4 + 1] = ClampByte((298 * c - 100 * d - 208 * e + 128) >> 8);
                target[column * 4 + 2] = ClampByte((298 * c + 409 * e + 128) >> 8);
                target[column * 4 + 3] = 0xFF;
            }
        }
    }

    LiveEncoder::LiveEncoder() = default;

    LiveEncoder::~LiveEncoder()
    {
        Shutdown();
    }

    void LiveEncoder::Shutdown()
    {
        if (encoder_) encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        if (outputType_) { outputType_->Release(); outputType_ = nullptr; }
        if (inputType_) { inputType_->Release(); inputType_ = nullptr; }
        if (encoder_) { encoder_->Release(); encoder_ = nullptr; }
        width_ = height_ = bitrate_ = 0;
        fps_ = 30;
        keyframeInterval_ = 60;
        frameIndex_ = 0;
        sequence_ = 0;
    }

    bool LiveEncoder::Initialize(uint32_t width, uint32_t height, uint32_t fps,
                                 uint32_t bitrate, uint32_t keyframeIntervalFrames)
    {
        Shutdown();
        if (width == 0 || height == 0 || fps == 0 || bitrate == 0) return false;
        if (FAILED(CoCreateInstance(CLSID_CMSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&encoder_))))
            return false;

        IMFAttributes* attributes = nullptr;
        if (SUCCEEDED(encoder_->GetAttributes(&attributes)))
        {
            attributes->SetUINT32(CODECAPI_AVLowLatencyMode, TRUE);
            attributes->SetUINT32(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
            attributes->SetUINT32(CODECAPI_AVEncCommonMeanBitRate, bitrate);
            attributes->SetUINT32(CODECAPI_AVEncCommonBufferSize, bitrate);
            attributes->SetUINT32(CODECAPI_AVEncMPVDefaultBPictureCount, 0);
            attributes->SetUINT32(CODECAPI_AVEncMPVGOPSize, std::max<uint32_t>(1, keyframeIntervalFrames));
            attributes->Release();
        }

        width_ = width;
        height_ = height;
        fps_ = fps;
        bitrate_ = bitrate;
        keyframeInterval_ = std::max<uint32_t>(1, keyframeIntervalFrames);
        frameDuration_ = 10'000'000LL / fps;
        frameIndex_ = 0;
        sequence_ = 0;

        if (FAILED(MFCreateMediaType(&outputType_))) { Shutdown(); return false; }
        outputType_->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outputType_->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        outputType_->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
        outputType_->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(outputType_, MF_MT_FRAME_SIZE, width, height);
        MFSetAttributeRatio(outputType_, MF_MT_FRAME_RATE, fps, 1);
        MFSetAttributeRatio(outputType_, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        if (FAILED(encoder_->SetOutputType(0, outputType_, 0))) { Shutdown(); return false; }

        if (FAILED(MFCreateMediaType(&inputType_))) { Shutdown(); return false; }
        inputType_->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        inputType_->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        inputType_->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(inputType_, MF_MT_FRAME_SIZE, width, height);
        MFSetAttributeRatio(inputType_, MF_MT_FRAME_RATE, fps, 1);
        MFSetAttributeRatio(inputType_, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        if (FAILED(encoder_->SetInputType(0, inputType_, 0))) { Shutdown(); return false; }

        encoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        if (FAILED(encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0)) ||
            FAILED(encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0)))
        {
            Shutdown();
            return false;
        }
        return true;
    }

    IMFSample* LiveEncoder::CreateInputSample(const uint8_t* bgra, uint32_t stride, int64_t sampleTime)
    {
        IMFMediaBuffer* buffer = nullptr;
        if (FAILED(MFCreate2DMediaBuffer(width_, height_, MFVideoFormat_NV12.Data1, FALSE, &buffer)))
            return nullptr;

        IMF2DBuffer* buffer2d = nullptr;
        bool written = false;
        BYTE* scanline = nullptr;
        LONG pitch = 0;
        if (SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(&buffer2d))) &&
            SUCCEEDED(buffer2d->Lock2D(&scanline, &pitch)) && scanline && pitch > 0)
        {
            const uint32_t yStride = static_cast<uint32_t>(pitch);
            BgraToNv12(bgra, stride, scanline, yStride,
                       scanline + size_t(pitch) * height_, yStride, width_, height_);
            buffer2d->Unlock2D();
            written = true;
        }
        if (buffer2d) buffer2d->Release();

        if (!written)
        {
            BYTE* data = nullptr;
            DWORD maximum = 0;
            DWORD current = 0;
            if (FAILED(buffer->Lock(&data, &maximum, &current)) ||
                maximum < width_ * height_ * 3 / 2)
            {
                buffer->Release();
                return nullptr;
            }
            BgraToNv12(bgra, stride, data, width_,
                       data + size_t(width_) * height_, width_, width_, height_);
            buffer->Unlock();
        }
        buffer->SetCurrentLength(width_ * height_ * 3 / 2);

        IMFSample* sample = nullptr;
        if (FAILED(MFCreateSample(&sample)) || FAILED(sample->AddBuffer(buffer)))
        {
            if (sample) sample->Release();
            buffer->Release();
            return nullptr;
        }
        buffer->Release();
        sample->SetSampleTime(sampleTime);
        sample->SetSampleDuration(frameDuration_);
        return sample;
    }

    bool LiveEncoder::NegotiateOutputType()
    {
        IMFMediaType* type = nullptr;
        if (FAILED(encoder_->GetOutputAvailableType(0, 0, &type))) return false;
        if (outputType_) outputType_->Release();
        outputType_ = type;
        return SUCCEEDED(encoder_->SetOutputType(0, outputType_, 0));
    }

    bool LiveEncoder::DrainOutput(std::vector<uint8_t>& accessUnit, bool& keyframe)
    {
        for (;;)
        {
            MFT_OUTPUT_STREAM_INFO info{};
            if (FAILED(encoder_->GetOutputStreamInfo(0, &info))) return false;

            const bool providesSamples = ProvidesOutputSamples(info.dwFlags);
            MFT_OUTPUT_DATA_BUFFER output{};
            IMFSample* sample = nullptr;
            if (providesSamples)
            {
                output.pSample = nullptr;
            }
            else
            {
                const DWORD size = std::max<DWORD>(info.cbSize, width_ * height_ * 2);
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
                if (!NegotiateOutputType()) return false;
                continue;
            }
            if (FAILED(result))
            {
                if (sample) sample->Release();
                return false;
            }

            AppendSampleBytes(output.pSample, accessUnit);
            UINT32 cleanPoint = 0;
            if (SUCCEEDED(output.pSample->GetUINT32(MFSampleExtension_CleanPoint, &cleanPoint)) &&
                cleanPoint)
                keyframe = true;
            if (output.pSample) output.pSample->Release();
            if (sample && sample != output.pSample) sample->Release();
        }
    }

    bool LiveEncoder::Encode(const uint8_t* bgra, uint32_t stride,
                             std::vector<uint8_t>& accessUnit, uint32_t& sequence, bool& keyframe)
    {
        accessUnit.clear();
        keyframe = false;
        sequence = 0;
        if (!encoder_ || !bgra || stride < width_ * 4) return false;

        IMFSample* sample = CreateInputSample(bgra, stride, frameIndex_ * frameDuration_);
        if (!sample) return false;

        HRESULT result = encoder_->ProcessInput(0, sample, 0);
        if (result == MF_E_NOTACCEPTING)
        {
            if (!DrainOutput(accessUnit, keyframe))
            {
                sample->Release();
                return false;
            }
            result = encoder_->ProcessInput(0, sample, 0);
        }
        sample->Release();
        if (FAILED(result)) return false;

        ++frameIndex_;
        if (!DrainOutput(accessUnit, keyframe)) return false;
        sequence = sequence_++;
        return true;
    }

    LiveDecoder::LiveDecoder() = default;

    LiveDecoder::~LiveDecoder()
    {
        Shutdown();
    }

    void LiveDecoder::Shutdown()
    {
        if (decoder_) decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        if (outputType_) { outputType_->Release(); outputType_ = nullptr; }
        if (inputType_) { inputType_->Release(); inputType_ = nullptr; }
        if (decoder_) { decoder_->Release(); decoder_ = nullptr; }
        width_ = 0;
        height_ = 0;
        displayWidth_ = 0;
        displayHeight_ = 0;
        apertureResolved_ = false;
        frameIndex_ = 0;
    }

    bool LiveDecoder::Initialize()
    {
        Shutdown();
        if (FAILED(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&decoder_))))
            return false;

        ICodecAPI* codec = nullptr;
        if (SUCCEEDED(decoder_->QueryInterface(IID_PPV_ARGS(&codec))) && codec)
        {
            VARIANT value;
            VariantInit(&value);
            value.vt = VT_UI4;
            value.ulVal = 1;
            codec->SetValue(&CODECAPI_AVLowLatencyMode, &value);
            VariantClear(&value);
            codec->Release();
        }

        if (FAILED(MFCreateMediaType(&inputType_))) { Shutdown(); return false; }
        inputType_->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        inputType_->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        if (FAILED(decoder_->SetInputType(0, inputType_, 0))) { Shutdown(); return false; }
        if (!SelectOutputType()) { Shutdown(); return false; }

        decoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        if (FAILED(decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0)) ||
            FAILED(decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0)))
        {
            Shutdown();
            return false;
        }
        frameIndex_ = 0;
        return true;
    }

    void LiveDecoder::ReadFrameSize()
    {
        if (apertureResolved_) return;
        UINT32 width = 0;
        UINT32 height = 0;
        if (outputType_ &&
            SUCCEEDED(MFGetAttributeSize(outputType_, MF_MT_FRAME_SIZE, &width, &height)))
        {
            width_ = width;
            height_ = height;
        }
        displayWidth_ = width_;
        displayHeight_ = height_;
        if (outputType_)
        {
            MFVideoArea area{};
            UINT32 blobSize = 0;
            if (SUCCEEDED(outputType_->GetBlob(
                    MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area),
                    sizeof(area), &blobSize)) && blobSize >= sizeof(MFVideoArea))
            {
                const UINT32 apertureWidth = static_cast<UINT32>(std::max<LONG>(0, area.Area.cx));
                const UINT32 apertureHeight = static_cast<UINT32>(std::max<LONG>(0, area.Area.cy));
                if (apertureWidth > 0 && apertureHeight > 0 &&
                    apertureWidth <= width_ && apertureHeight <= height_)
                {
                    displayWidth_ = apertureWidth;
                    displayHeight_ = apertureHeight;
                    apertureResolved_ = true;
                }
            }
        }
    }

    bool LiveDecoder::SelectOutputType()
    {
        IMFMediaType* type = nullptr;
        if (FAILED(MFCreateMediaType(&type))) return false;
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        HRESULT result = decoder_->SetOutputType(0, type, 0);
        if (FAILED(result))
        {
            type->Release();
            type = nullptr;
            for (DWORD index = 0;; ++index)
            {
                IMFMediaType* candidate = nullptr;
                if (FAILED(decoder_->GetOutputAvailableType(0, index, &candidate))) break;
                GUID subtype{};
                candidate->GetGUID(MF_MT_SUBTYPE, &subtype);
                if (subtype == MFVideoFormat_NV12)
                {
                    type = candidate;
                    break;
                }
                candidate->Release();
            }
            if (!type) return false;
            result = decoder_->SetOutputType(0, type, 0);
        }
        if (FAILED(result))
        {
            type->Release();
            return false;
        }
        if (outputType_) outputType_->Release();
        outputType_ = type;
        ReadFrameSize();
        return true;
    }

    IMFSample* LiveDecoder::CreateInputSample(std::span<const uint8_t> accessUnit, int64_t sampleTime)
    {
        IMFMediaBuffer* buffer = nullptr;
        if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(accessUnit.size()), &buffer)))
            return nullptr;
        BYTE* data = nullptr;
        DWORD maximum = 0;
        DWORD current = 0;
        if (FAILED(buffer->Lock(&data, &maximum, &current)) || maximum < accessUnit.size())
        {
            buffer->Release();
            return nullptr;
        }
        std::memcpy(data, accessUnit.data(), accessUnit.size());
        buffer->Unlock();
        buffer->SetCurrentLength(static_cast<DWORD>(accessUnit.size()));

        IMFSample* sample = nullptr;
        if (FAILED(MFCreateSample(&sample)) || FAILED(sample->AddBuffer(buffer)))
        {
            if (sample) sample->Release();
            buffer->Release();
            return nullptr;
        }
        buffer->Release();
        sample->SetSampleTime(sampleTime);
        sample->SetSampleDuration(frameDuration_);
        return sample;
    }

    bool LiveDecoder::DrainOutput(std::vector<uint8_t>& bgra, uint32_t& width,
                                  uint32_t& height, bool& produced)
    {
        for (;;)
        {
            MFT_OUTPUT_STREAM_INFO info{};
            if (FAILED(decoder_->GetOutputStreamInfo(0, &info))) return false;

            MFT_OUTPUT_DATA_BUFFER output{};
            IMFSample* sample = nullptr;
            if (!ProvidesOutputSamples(info.dwFlags))
            {
                const DWORD fallback = 640 * 360 * 3 / 2;
                const DWORD size = std::max<DWORD>(info.cbSize, fallback);
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

            if (displayWidth_ == 0 || displayHeight_ == 0 || !apertureResolved_)
                ReadFrameSize();

            IMFMediaBuffer* media = nullptr;
            if (displayWidth_ > 0 && displayHeight_ > 0 &&
                SUCCEEDED(output.pSample->GetBufferByIndex(0, &media)) && media)
            {
                bgra.resize(size_t(displayWidth_) * displayHeight_ * 4);
                IMF2DBuffer* buffer2d = nullptr;
                if (SUCCEEDED(media->QueryInterface(IID_PPV_ARGS(&buffer2d))))
                {
                    BYTE* scanline = nullptr;
                    LONG pitch = 0;
                    if (SUCCEEDED(buffer2d->Lock2D(&scanline, &pitch)) && scanline && pitch > 0)
                    {
                        const uint32_t yStride = static_cast<uint32_t>(pitch);
                        Nv12ToBgra(scanline, yStride,
                                   scanline + size_t(pitch) * height_, yStride,
                                   bgra.data(), displayWidth_ * 4, displayWidth_, displayHeight_);
                        buffer2d->Unlock2D();
                        produced = true;
                    }
                    buffer2d->Release();
                }
                else
                {
                    BYTE* data = nullptr;
                    DWORD maximum = 0;
                    DWORD current = 0;
                    if (SUCCEEDED(media->Lock(&data, &maximum, &current)) &&
                        maximum >= size_t(width_) * height_ * 3 / 2)
                    {
                        Nv12ToBgra(data, width_,
                                   data + size_t(width_) * height_, width_,
                                   bgra.data(), displayWidth_ * 4, displayWidth_, displayHeight_);
                        media->Unlock();
                        produced = true;
                    }
                }
                media->Release();
            }
            if (produced)
            {
                width = displayWidth_;
                height = displayHeight_;
            }

            if (output.pSample) output.pSample->Release();
            if (sample && sample != output.pSample) sample->Release();
        }
    }

    bool LiveDecoder::Decode(std::span<const uint8_t> accessUnit, uint32_t sequence,
                             std::vector<uint8_t>& bgra, uint32_t& width, uint32_t& height)
    {
        bgra.clear();
        width = 0;
        height = 0;
        if (!decoder_ || accessUnit.empty()) return false;

        const int64_t sampleTime = static_cast<int64_t>(sequence) * frameDuration_;
        IMFSample* sample = CreateInputSample(accessUnit, sampleTime);
        if (!sample) return false;

        HRESULT result = decoder_->ProcessInput(0, sample, 0);
        if (result == MF_E_NOTACCEPTING)
        {
            bool produced = false;
            if (!DrainOutput(bgra, width, height, produced))
            {
                sample->Release();
                return false;
            }
            result = decoder_->ProcessInput(0, sample, 0);
        }
        sample->Release();
        if (FAILED(result)) return false;

        bool produced = false;
        if (!DrainOutput(bgra, width, height, produced)) return false;
        ++frameIndex_;
        return produced;
    }
}
