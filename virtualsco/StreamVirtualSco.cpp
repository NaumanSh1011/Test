/*
 * Socket-backed streams for the virtual BT-SCO module. See StreamVirtualSco.h.
 *
 * Driver state machine and pacing are copied from AOSP's DriverStubImpl.cpp; transfer() hands the
 * PCM to/from the va_server bridge. The mix ports are PCM_16_BIT mono (VirtualScoConfiguration.cpp),
 * so one frame == one int16 sample and buffers map 1:1 onto va_server frames — no conversion.
 */

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
      mIsInput(context.isInput()) {}

::android::status_t DriverVirtualSco::init() {
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
    // Bridge I/O first (non-blocking ring ops), then StreamStub's wall-clock pacing.
    int16_t* samples = static_cast<int16_t*>(buffer);
    if (mIsInput) {
        // Uplink: agent voice becomes the virtual SCO mic; silence when nothing is buffered.
        if (!va_server_pull_uplink(samples, static_cast<int>(frameCount))) {
            memset(buffer, 0, frameCount * mFrameSizeBytes);
        }
    } else {
        // Downlink: call playback routed to the virtual SCO out is tapped to the app client.
        va_server_push_downlink(samples, static_cast<int>(frameCount));
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
