/*
 * Socket-backed streams for the virtual BT-SCO module. See StreamVirtualSco.h.
 *
 * Driver state machine and pacing are copied from AOSP's DriverStubImpl.cpp; transfer() hands the
 * PCM to/from the va_server bridge. Both sides run at 16 kHz PCM16 (VirtualScoConfiguration.cpp), so
 * no resampling: capture is mono (1:1 with va_server frames); playback is stereo, because AudioFlinger
 * only opens a MIXER thread (which resamples VoIP clients) for >= 2 channels, and is downmixed here.
 */

#include <algorithm>
#include <cmath>
#include <cstring>

#define LOG_TAG "AHAL_VirtualScoStream"
#include <android-base/logging.h>
#include <audio_utils/clock.h>

#include "StreamVirtualSco.h"
#include "va_server.h"

using aidl::android::hardware::audio::common::SinkMetadata;
using aidl::android::hardware::audio::common::SourceMetadata;
using aidl::android::media::audio::common::AudioOffloadInfo;
using aidl::android::media::audio::common::MicrophoneInfo;

namespace aidl::android::hardware::audio::core {

DriverVirtualSco::DriverVirtualSco(const StreamContext& context)
    : mBufferSizeFrames(context.getBufferSizeInFrames()),
      mFrameSizeBytes(context.getFrameSize()),
      mSampleRate(context.getSampleRate()),
      mIsAsynchronous(!!context.getAsyncCallback()),
      mIsInput(context.isInput()),
      mChannelCount(std::max<size_t>(1, context.getFrameSize() / sizeof(int16_t))) {}

::android::status_t DriverVirtualSco::init() {
    if (mChannelCount > 1) {
        mMonoBuffer.resize(mBufferSizeFrames);
    }
    LOG(INFO) << __func__ << ": " << (mIsInput ? "input" : "output") << " " << mSampleRate << " Hz, "
              << mChannelCount << " ch, buffer " << mBufferSizeFrames << " frames";
    mIsInitialized = true;
    return ::android::OK;
}

::android::status_t DriverVirtualSco::drain(StreamDescriptor::DrainMode) {
    if (!mIsInitialized) {
        LOG(FATAL) << __func__ << ": must not happen for an uninitialized driver";
    }
    if (!mIsInput) {
        if (!mIsAsynchronous) {
            static constexpr float kMicrosPerSecond = MICROS_PER_SECOND;
            const size_t delayUs = static_cast<size_t>(
                    std::roundf(mBufferSizeFrames * kMicrosPerSecond / mSampleRate));
            usleep(delayUs);
        } else {
            usleep(500);
        }
    }
    return ::android::OK;
}

::android::status_t DriverVirtualSco::flush() {
    if (!mIsInitialized) {
        LOG(FATAL) << __func__ << ": must not happen for an uninitialized driver";
    }
    return ::android::OK;
}

::android::status_t DriverVirtualSco::pause() {
    if (!mIsInitialized) {
        LOG(FATAL) << __func__ << ": must not happen for an uninitialized driver";
    }
    return ::android::OK;
}

::android::status_t DriverVirtualSco::standby() {
    if (!mIsInitialized) {
        LOG(FATAL) << __func__ << ": must not happen for an uninitialized driver";
    }
    mIsStandby = true;
    return ::android::OK;
}

::android::status_t DriverVirtualSco::start() {
    if (!mIsInitialized) {
        LOG(FATAL) << __func__ << ": must not happen for an uninitialized driver";
    }
    mIsStandby = false;
    mStartTimeNs = ::android::uptimeNanos();
    mFramesSinceStart = 0;
    return ::android::OK;
}

::android::status_t DriverVirtualSco::transfer(void* buffer, size_t frameCount,
                                               size_t* actualFrameCount, int32_t*) {
    if (!mIsInitialized) {
        LOG(FATAL) << __func__ << ": must not happen for an uninitialized driver";
    }
    if (mIsStandby) {
        LOG(FATAL) << __func__ << ": must not happen while in standby";
    }
    // Bridge I/O first (non-blocking ring ops), then StreamStub's wall-clock pacing. The socket is
    // mono; multi-channel buffers go through mMonoBuffer (sized for a full HAL buffer in init()).
    int16_t* samples = static_cast<int16_t*>(buffer);
    const size_t monoFrames =
            mChannelCount > 1 ? std::min(frameCount, mMonoBuffer.size()) : frameCount;
    int16_t* mono = mChannelCount > 1 ? mMonoBuffer.data() : samples;
    if (mIsInput) {
        // Uplink: agent voice becomes the virtual mic; silence when nothing is buffered.
        if (!va_server_pull_uplink(mono, static_cast<int>(monoFrames))) {
            memset(buffer, 0, frameCount * mFrameSizeBytes);
        } else if (mChannelCount > 1) {
            for (size_t i = 0; i < monoFrames; ++i) {
                for (size_t ch = 0; ch < mChannelCount; ++ch) {
                    samples[i * mChannelCount + ch] = mono[i];
                }
            }
        }
    } else {
        // Downlink: the mixed call playback routed to the virtual out is tapped to the app client.
        if (mChannelCount > 1) {
            for (size_t i = 0; i < monoFrames; ++i) {
                int32_t sum = 0;
                for (size_t ch = 0; ch < mChannelCount; ++ch) {
                    sum += samples[i * mChannelCount + ch];
                }
                mono[i] = static_cast<int16_t>(sum / static_cast<int32_t>(mChannelCount));
            }
        }
        va_server_push_downlink(mono, static_cast<int>(monoFrames));
    }
    *actualFrameCount = frameCount;
    if (mIsAsynchronous) {
        usleep(500);
    } else {
        mFramesSinceStart += *actualFrameCount;
        const long bufferDurationUs = (*actualFrameCount) * MICROS_PER_SECOND / mSampleRate;
        const auto totalDurationUs =
                (::android::uptimeNanos() - mStartTimeNs) / NANOS_PER_MICROSECOND;
        const long totalOffsetUs =
                mFramesSinceStart * MICROS_PER_SECOND / mSampleRate - totalDurationUs;
        LOG(VERBOSE) << __func__ << ": totalOffsetUs " << totalOffsetUs;
        if (totalOffsetUs > 0) {
            const long sleepTimeUs = std::min(totalOffsetUs, bufferDurationUs);
            LOG(VERBOSE) << __func__ << ": sleeping for " << sleepTimeUs << " us";
            usleep(sleepTimeUs);
        }
    }
    return ::android::OK;
}

void DriverVirtualSco::shutdown() {
    mIsInitialized = false;
}

StreamVirtualSco::StreamVirtualSco(StreamContext* context, const Metadata& metadata)
    : StreamCommonImpl(context, metadata), DriverVirtualSco(getContext()) {}

StreamVirtualSco::~StreamVirtualSco() {
    cleanupWorker();
}

StreamInVirtualSco::StreamInVirtualSco(StreamContext&& context, const SinkMetadata& sinkMetadata,
                                       const std::vector<MicrophoneInfo>& microphones)
    : StreamIn(std::move(context), microphones),
      StreamVirtualSco(&mContextInstance, sinkMetadata) {}

StreamOutVirtualSco::StreamOutVirtualSco(StreamContext&& context,
                                         const SourceMetadata& sourceMetadata,
                                         const std::optional<AudioOffloadInfo>& offloadInfo)
    : StreamOut(std::move(context), offloadInfo),
      StreamVirtualSco(&mContextInstance, sourceMetadata) {}

}  // namespace aidl::android::hardware::audio::core
