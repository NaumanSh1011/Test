/*
 * Virtual Bluetooth-SCO module configuration for the AIDL audio HAL.
 *
 * This declares the SAME logical device the legacy HAL exposes (native/hal): one virtual BT-SCO
 * output + one virtual BT-SCO input at MAC 02:56:41:00:00:01, PCM16 mono 16 kHz, plus the two mix
 * ports and their routes. It is modelled 1:1 on AOSP's getRSubmixConfiguration() (a software-only
 * "virtual" device), so it plugs straight into the default Module implementation.
 *
 * On this AIDL build the device ports are presented as BUS devices (TYPE_BUS, empty connection) so
 * the framework auto-attaches them; the app's pinner matches the device by address across
 * TYPE_BLUETOOTH_SCO (legacy HIDL fleet) or TYPE_BUS (this build). See OPTION2_BUS_DEVICE.md.
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

#include <string>
#include <vector>

#include <core-impl/Module.h>
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

// The virtual device's address. StrategyRoutePinner matches the device by this address (see the
// legacy HAL / the app's sco/ package). Declared as an `id` string, not `mac`: for an
// empty-connection device the framework converts legacy addresses back to AIDL as `id`
// (suggestDeviceAddressTag), and the HAL mapper compares devices exactly, so a `mac`-tagged address
// would stop matching on the way back (patches / stream opens).
static const std::string kVirtualDeviceAddress = "02:56:41:00:00:01";

// Virtual device ext: a BUS device (AudioDeviceInfo.TYPE_BUS) with an EMPTY connection type and a
// fixed address. The framework only auto-attaches AIDL device ports whose connection is empty
// (AudioPolicyConfig::loadFromAidl), so this device is available at boot and appears in
// AudioManager.getDevices() without any setDeviceConnectionState call, attached to this module.
// (A CONNECTION_BT_SCO port can only become available via an external connect, which on the target
// stack binds to the vendor's own BT-SCO port — see OPTION2_BUS_DEVICE.md.)
static AudioPortExt createVirtualDeviceExt(AudioDeviceType devType) {
    AudioPortDeviceExt deviceExt;
    deviceExt.device.type.type = devType;
    deviceExt.device.address =
            aidl::android::media::audio::common::AudioDeviceAddress::make<
                    aidl::android::media::audio::common::AudioDeviceAddress::id>(
                    kVirtualDeviceAddress);
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
    // Capture side: PCM16 mono 16 kHz, 1:1 with the va_server uplink frames. RecordThreads accept a
    // mono HAL stream and resample/remix each client, so this opens as a normal capture.
    const std::vector<AudioProfile> scoProfiles{
            createProfile(PcmType::INT_16_BIT, {AudioChannelLayout::LAYOUT_MONO}, {16000})};
    // Playback side: PCM16 STEREO 48 kHz. Stereo because AudioFlinger opens a MIXER thread only for
    // a sink channel mask of >= 2 channels (isValidPcmSinkChannelMask: "mono is not supported at
    // this time"; mono fell back to a non-resampling DIRECT thread). 48 kHz because on the target
    // device a 16 kHz OUT_BUS mixer never consumed its tracks, while 48 kHz mixers mix VoIP (48 kHz)
    // playback fine (OUT_BUS_GO_48K.md). StreamVirtualSco downmixes and decimates 48 -> 16 kHz
    // mono for the socket.
    const std::vector<AudioProfile> outProfiles{
            createProfile(PcmType::INT_16_BIT, {AudioChannelLayout::LAYOUT_STEREO}, {48000})};

    // Device ports (the tap points): attached BUS devices at the fixed address, with static profiles
    // (no connectedProfiles — there is no external connect).
    AudioPort scoOutDevice =
            createPort(c.nextPortId++, "BT SCO Virtual", false,
                       createVirtualDeviceExt(AudioDeviceType::OUT_BUS));
    scoOutDevice.profiles = outProfiles;
    c.ports.push_back(scoOutDevice);

    AudioPort scoInDevice =
            createPort(c.nextPortId++, "BT SCO Virtual Mic", true,
                       createVirtualDeviceExt(AudioDeviceType::IN_BUS));
    scoInDevice.profiles = scoProfiles;
    c.ports.push_back(scoInDevice);

    // Mix ports.
    AudioPort outMix = createPort(c.nextPortId++, "virtual output", false, createPortMixExt(1, 1));
    outMix.profiles = outProfiles;
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
