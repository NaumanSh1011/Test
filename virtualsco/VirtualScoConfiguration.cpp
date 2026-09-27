/*
 * Virtual Bluetooth-SCO module configuration for the AIDL audio HAL.
 *
 * This declares the SAME logical device the legacy HAL exposes (native/hal): one virtual BT-SCO
 * output + one virtual BT-SCO input at MAC 02:56:41:00:00:01, PCM16 mono 16 kHz, plus the two mix
 * ports and their routes. It is modelled 1:1 on AOSP's getRSubmixConfiguration() (a software-only
 * "virtual" device), so it plugs straight into the default Module implementation.
 *
 * For the ENUMERATION SPIKE the module is created as Module::Type::STUB (see main_virtual.cpp),
 * i.e. the streams are AOSP's silent StreamStub. That is enough to answer the make-or-break
 * question: does the framework enumerate a vendor-injected IModule/virtual and let
 * StrategyRoutePinner pin voice-communication onto its BT-SCO device? Only once that passes do we
 * replace the stub streams with socket-backed streams (va_server/va_ring), the AIDL analogue of the
 * legacy HAL's out_write/in_read.
 *
 * The helper functions below are copied from AOSP's default Configuration.cpp because the originals
 * are file-static and not exported by libaudioserviceexampleimpl.
 */

#include <vector>

#include <aidl/android/hardware/audio/core/Module.h>
#include <aidl/android/media/audio/common/AudioChannelLayout.h>
#include <aidl/android/media/audio/common/AudioDeviceDescription.h>
#include <aidl/android/media/audio/common/AudioDeviceType.h>
#include <aidl/android/media/audio/common/AudioFormatType.h>
#include <aidl/android/media/audio/common/AudioIoFlags.h>
#include <aidl/android/media/audio/common/AudioPort.h>
#include <aidl/android/media/audio/common/AudioPortDeviceExt.h>
#include <aidl/android/media/audio/common/AudioPortMixExt.h>
#include <aidl/android/media/audio/common/AudioProfile.h>
#include <aidl/android/media/audio/common/PcmType.h>

#include "VirtualScoConfiguration.h"

using aidl::android::hardware::audio::core::AudioRoute;
using aidl::android::media::audio::common::AudioChannelLayout;
using aidl::android::media::audio::common::AudioDeviceDescription;
using aidl::android::media::audio::common::AudioDeviceType;
using aidl::android::media::audio::common::AudioFormatType;
using aidl::android::media::audio::common::AudioIoFlags;
using aidl::android::media::audio::common::AudioPort;
using aidl::android::media::audio::common::AudioPortDeviceExt;
using aidl::android::media::audio::common::AudioPortExt;
using aidl::android::media::audio::common::AudioPortMixExt;
using aidl::android::media::audio::common::AudioProfile;
using aidl::android::media::audio::common::PcmType;
using Configuration = aidl::android::hardware::audio::core::Module::Configuration;

namespace aicaller::virtualsco {

// The virtual BT-SCO MAC. StrategyRoutePinner pins by this address (see the legacy HAL / the app's
// sco/ package); the framework stringifies AudioDeviceAddress::mac as "02:56:41:00:00:01".
static const std::vector<uint8_t> kScoMac{0x02, 0x56, 0x41, 0x00, 0x00, 0x01};

static AudioProfile createProfile(PcmType pcmType, const std::vector<int32_t>& channelLayouts,
                                  const std::vector<int32_t>& sampleRates) {
    AudioProfile profile;
    profile.format.type = AudioFormatType::PCM;
    profile.format.pcm = pcmType;
    for (auto layout : channelLayouts) {
        profile.channelMasks.push_back(
                AudioChannelLayout::make<AudioChannelLayout::layoutMask>(layout));
    }
    profile.sampleRates.insert(profile.sampleRates.end(), sampleRates.begin(), sampleRates.end());
    return profile;
}

// BT-SCO device ext carrying the MAC. connection = CONNECTION_BT_SCO maps {OUT_HEADSET|IN_HEADSET}
// to the legacy AUDIO_DEVICE_*_BLUETOOTH_SCO_HEADSET the pinner expects.
static AudioPortExt createScoDeviceExt(AudioDeviceType devType) {
    AudioPortDeviceExt deviceExt;
    deviceExt.device.type.type = devType;
    deviceExt.device.type.connection = AudioDeviceDescription::CONNECTION_BT_SCO;
    deviceExt.device.address =
            aidl::android::media::audio::common::AudioDeviceAddress::make<
                    aidl::android::media::audio::common::AudioDeviceAddress::mac>(kScoMac);
    deviceExt.flags = 0;
    return AudioPortExt::make<AudioPortExt::Tag::device>(deviceExt);
}

static AudioPortExt createPortMixExt(int32_t maxOpen, int32_t maxActive) {
    AudioPortMixExt mixExt;
    mixExt.maxOpenStreamCount = maxOpen;
    mixExt.maxActiveStreamCount = maxActive;
    return AudioPortExt::make<AudioPortExt::Tag::mix>(mixExt);
}

static AudioPort createPort(int32_t id, const std::string& name, bool isInput,
                            const AudioPortExt& ext) {
    AudioPort port;
    port.id = id;
    port.name = name;
    port.flags = isInput ? AudioIoFlags::make<AudioIoFlags::Tag::input>(0)
                         : AudioIoFlags::make<AudioIoFlags::Tag::output>(0);
    port.ext = ext;
    return port;
}

static AudioRoute createRoute(const std::vector<AudioPort>& sources, const AudioPort& sink) {
    AudioRoute route;
    route.sinkPortId = sink.id;
    for (const auto& p : sources) route.sourcePortIds.push_back(p.id);
    return route;
}

std::unique_ptr<Configuration> getVirtualScoConfiguration() {
    Configuration c;
    const std::vector<AudioProfile> scoProfiles{
            createProfile(PcmType::INT_16_BIT, {AudioChannelLayout::LAYOUT_MONO}, {16000})};

    // Device ports (the tap points).
    AudioPort scoOutDevice =
            createPort(c.nextPortId++, "BT SCO Virtual", false,
                       createScoDeviceExt(AudioDeviceType::OUT_HEADSET));
    c.ports.push_back(scoOutDevice);
    c.connectedProfiles[scoOutDevice.id] = scoProfiles;

    AudioPort scoInDevice =
            createPort(c.nextPortId++, "BT SCO Virtual Mic", true,
                       createScoDeviceExt(AudioDeviceType::IN_HEADSET));
    c.ports.push_back(scoInDevice);
    c.connectedProfiles[scoInDevice.id] = scoProfiles;

    // Mix ports.
    AudioPort outMix = createPort(c.nextPortId++, "virtual output", false, createPortMixExt(1, 1));
    outMix.profiles = scoProfiles;
    c.ports.push_back(outMix);

    AudioPort inMix = createPort(c.nextPortId++, "virtual input", true, createPortMixExt(1, 1));
    inMix.profiles = scoProfiles;
    c.ports.push_back(inMix);

    // Routes: playback mix -> SCO out device; SCO in device -> capture mix.
    c.routes.push_back(createRoute({outMix}, scoOutDevice));
    c.routes.push_back(createRoute({scoInDevice}, inMix));

    return std::make_unique<Configuration>(c);
}

}  // namespace aicaller::virtualsco
