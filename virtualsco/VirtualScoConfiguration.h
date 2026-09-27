#pragma once

#include <memory>

#include <aidl/android/hardware/audio/core/Module.h>

namespace aicaller::virtualsco {

// Builds the virtual BT-SCO module configuration (one SCO out + one SCO in device at
// 02:56:41:00:00:01, PCM16 mono 16 kHz, two mix ports, two routes). See VirtualScoConfiguration.cpp.
std::unique_ptr<aidl::android::hardware::audio::core::Module::Configuration>
getVirtualScoConfiguration();

}  // namespace aicaller::virtualsco
