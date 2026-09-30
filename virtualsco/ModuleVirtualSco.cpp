/*
 * The virtual BT-SCO audio module. See ModuleVirtualSco.h.
 */

#define LOG_TAG "AHAL_VirtualScoModule"
#include <android-base/logging.h>

#include "ModuleVirtualSco.h"
#include "StreamVirtualSco.h"
#include "va_protocol.h"
#include "va_server.h"

using aidl::android::hardware::audio::common::SinkMetadata;
using aidl::android::hardware::audio::common::SourceMetadata;
using aidl::android::media::audio::common::AudioChannelLayout;
using aidl::android::media::audio::common::AudioFormatType;
using aidl::android::media::audio::common::AudioOffloadInfo;
using aidl::android::media::audio::common::AudioPort;
using aidl::android::media::audio::common::AudioPortConfig;
using aidl::android::media::audio::common::AudioPortExt;
using aidl::android::media::audio::common::AudioProfile;
using aidl::android::media::audio::common::MicrophoneInfo;
using aidl::android::media::audio::common::PcmType;

namespace aidl::android::hardware::audio::core {

// Must match the virtual mix ports in VirtualScoConfiguration.cpp (PCM_16_BIT mono 16 kHz):
// 20 ms frames of int16 mono.
static constexpr uint32_t kSampleRate = 16000;
static constexpr uint32_t kFrameSamples = 320;

ModuleVirtualSco::ModuleVirtualSco(std::unique_ptr<Configuration>&& config)
    : Module(Type::STUB, std::move(config)) {
    if (va_server_start(VA_SOCKET_PATH, kSampleRate, kFrameSamples)) {
        LOG(INFO) << "va_server listening on " << VA_SOCKET_PATH;
    } else {
        LOG(ERROR) << "va_server failed to start on " << VA_SOCKET_PATH
                   << "; streams will run silent (check SELinux socket rules)";
    }
}

ModuleVirtualSco::~ModuleVirtualSco() {
    va_server_stop();
}

ndk::ScopedAStatus ModuleVirtualSco::createInputStream(
        StreamContext&& context, const SinkMetadata& sinkMetadata,
        const std::vector<MicrophoneInfo>& microphones, std::shared_ptr<StreamIn>* result) {
    return createStreamInstance<StreamInVirtualSco>(result, std::move(context), sinkMetadata,
                                                    microphones);
}

ndk::ScopedAStatus ModuleVirtualSco::createOutputStream(
        StreamContext&& context, const SourceMetadata& sourceMetadata,
        const std::optional<AudioOffloadInfo>& offloadInfo, std::shared_ptr<StreamOut>* result) {
    return createStreamInstance<StreamOutVirtualSco>(result, std::move(context), sourceMetadata,
                                                     offloadInfo);
}

ndk::ScopedAStatus ModuleVirtualSco::populateConnectedDevicePort(AudioPort* audioPort, int32_t) {
    if (audioPort->ext.getTag() != AudioPortExt::device) {
        LOG(ERROR) << __func__ << ": not a device port: " << audioPort->toString();
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    // The template SCO ports carry static profiles (VirtualScoConfiguration.cpp); keep them. If a
    // port arrives without any, give it the only format the virtual SCO mix ports support.
    if (audioPort->profiles.empty()) {
        AudioProfile profile;
        profile.format.type = AudioFormatType::PCM;
        profile.format.pcm = PcmType::INT_16_BIT;
        profile.channelMasks.push_back(AudioChannelLayout::make<AudioChannelLayout::layoutMask>(
                AudioChannelLayout::LAYOUT_MONO));
        profile.sampleRates.push_back(kSampleRate);
        audioPort->profiles.push_back(profile);
    }
    LOG(INFO) << __func__ << ": connecting " << audioPort->toString();
    return ndk::ScopedAStatus::ok();
}

int32_t ModuleVirtualSco::getNominalLatencyMs(const AudioPortConfig&) {
    static constexpr int32_t kLatencyMs = 20;
    return kLatencyMs;
}

}  // namespace aidl::android::hardware::audio::core
