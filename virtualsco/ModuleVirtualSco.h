/*
 * The virtual BT-SCO audio module: AOSP's default Module with socket-backed streams
 * (StreamVirtualSco) in place of StreamStub. Owns the va_server bridge for its lifetime.
 */

#pragma once

#include "core-impl/Module.h"

namespace aidl::android::hardware::audio::core {

class ModuleVirtualSco final : public Module {
  public:
    explicit ModuleVirtualSco(std::unique_ptr<Configuration>&& config);
    ~ModuleVirtualSco();

  protected:
    ndk::ScopedAStatus createInputStream(
            StreamContext&& context,
            const ::aidl::android::hardware::audio::common::SinkMetadata& sinkMetadata,
            const std::vector<::aidl::android::media::audio::common::MicrophoneInfo>& microphones,
            std::shared_ptr<StreamIn>* result) override;
    ndk::ScopedAStatus createOutputStream(
            StreamContext&& context,
            const ::aidl::android::hardware::audio::common::SourceMetadata& sourceMetadata,
            const std::optional<::aidl::android::media::audio::common::AudioOffloadInfo>&
                    offloadInfo,
            std::shared_ptr<StreamOut>* result) override;
    // The SCO device ports use CONNECTION_BT_SCO, so the framework treats them as external
    // devices and connects them via connectExternalDevice; the base Module rejects that for any
    // port with a non-empty connection, so accept it here.
    ndk::ScopedAStatus populateConnectedDevicePort(
            ::aidl::android::media::audio::common::AudioPort* audioPort,
            int32_t nextPortId) override;
};

}  // namespace aidl::android::hardware::audio::core
