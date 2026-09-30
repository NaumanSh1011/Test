/*
 * Socket-backed streams for the virtual BT-SCO module.
 *
 * Modelled 1:1 on AOSP's StreamStub / DriverStubImpl (hardware/interfaces/audio/aidl/default); only
 * transfer() differs: OUTPUT frames are pushed to the va_server bridge as downlink, INPUT frames are
 * pulled from it as uplink (silence when the ring is empty). StreamStub's wall-clock pacing is kept,
 * so the HAL runs at real time whether or not an app client is connected.
 */

#pragma once

#include <array>
#include <vector>

#include "core-impl/Stream.h"

namespace aidl::android::hardware::audio::core {

class DriverVirtualSco : virtual public DriverInterface {
  public:
    explicit DriverVirtualSco(const StreamContext& context);

    ::android::status_t init() override;
    ::android::status_t drain(StreamDescriptor::DrainMode) override;
    ::android::status_t flush() override;
    ::android::status_t pause() override;
    ::android::status_t standby() override;
    ::android::status_t start() override;
    ::android::status_t transfer(void* buffer, size_t frameCount, size_t* actualFrameCount,
                                 int32_t* latencyMs) override;
    void shutdown() override;

  private:
    const size_t mBufferSizeFrames;
    const size_t mFrameSizeBytes;
    const int mSampleRate;
    const bool mIsAsynchronous;
    const bool mIsInput;
    // PCM16 channels per frame. The socket is always mono; the playback side runs stereo (a MIXER
    // thread needs >= 2 channels), so transfer() down/up-mixes through mMonoBuffer.
    const size_t mChannelCount;
    std::vector<int16_t> mMonoBuffer;  // Sized in init(), reused per transfer (no audio-path alloc).
    // Output only: decimation from the playback rate (48 kHz) to the 16 kHz socket rate, through a
    // windowed-sinc low-pass FIR whose state carries across transfer() calls.
    static constexpr int kSocketRate = 16000;
    static constexpr size_t kFirTaps = 31;
    size_t mDecimFactor = 1;  // mSampleRate / kSocketRate when that's an integer > 1, else 1.
    std::array<float, kFirTaps> mFirCoeffs{};
    std::array<float, kFirTaps> mFirHistory{};
    size_t mFirPos = 0;
    size_t mDecimPhase = 0;
    std::vector<int16_t> mSocketBuffer;  // Decimated output, sized in init().
    bool mIsInitialized = false;  // Used for validating the state machine logic.
    bool mIsStandby = true;       // Used for validating the state machine logic.
    int64_t mStartTimeNs = 0;
    long mFramesSinceStart = 0;
};

class StreamVirtualSco : public StreamCommonImpl, public DriverVirtualSco {
  public:
    StreamVirtualSco(StreamContext* context, const Metadata& metadata);
    ~StreamVirtualSco();
};

class StreamInVirtualSco final : public StreamIn, public StreamVirtualSco {
  public:
    friend class ndk::SharedRefBase;
    StreamInVirtualSco(
            StreamContext&& context,
            const ::aidl::android::hardware::audio::common::SinkMetadata& sinkMetadata,
            const std::vector<::aidl::android::media::audio::common::MicrophoneInfo>& microphones);

  private:
    void onClose(StreamDescriptor::State) override { defaultOnClose(); }
};

class StreamOutVirtualSco final : public StreamOut, public StreamVirtualSco {
  public:
    friend class ndk::SharedRefBase;
    StreamOutVirtualSco(
            StreamContext&& context,
            const ::aidl::android::hardware::audio::common::SourceMetadata& sourceMetadata,
            const std::optional<::aidl::android::media::audio::common::AudioOffloadInfo>&
                    offloadInfo);

  private:
    void onClose(StreamDescriptor::State) override { defaultOnClose(); }
};

}  // namespace aidl::android::hardware::audio::core
