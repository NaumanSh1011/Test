/*
 * Standalone vendor service that registers a single AIDL audio module instance,
 * android.hardware.audio.core.IModule/virtual, exposing the virtual BT-SCO device.
 *
 * It does NOT touch the device's real audio HAL (secaudiohalaidl / the primary module). The
 * framework's AIDL audio factory enumerates all DECLARED IModule instances (getDeclaredInstances),
 * so declaring IModule/virtual in a VINTF fragment (virtualsco.xml) makes AudioPolicyManager pick
 * this up alongside the vendor's own modules — the AIDL analogue of adding an <xi:include> module
 * to the legacy policy XML.
 *
 * ENUMERATION SPIKE: created as Module::Type::STUB, so streams are AOSP's silent StreamStub. Goal is
 * to confirm the module + its BT-SCO device appear in `dumpsys media.audio_policy` and that
 * StrategyRoutePinner can pin voice-communication to it. The socket-backed streams come next.
 */

#include <cstdlib>

#define LOG_TAG "AHAL_VirtualSco"
#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

#include <aidl/android/hardware/audio/core/Module.h>

#include "VirtualScoConfiguration.h"

using aidl::android::hardware::audio::core::Module;

int main() {
    android::base::SetMinimumLogSeverity(::android::base::DEBUG);
    ABinderProcess_setThreadPoolMaxThreadCount(16);
    LOG(INFO) << "AICaller virtual SCO AIDL module starting";

    // Reuse AOSP's complete Module implementation; STUB gives silent no-op streams for the spike.
    auto module = Module::createInstance(Module::Type::STUB,
                                         aicaller::virtualsco::getVirtualScoConfiguration());
    if (module == nullptr) {
        LOG(ERROR) << "failed to create virtual SCO module instance";
        return EXIT_FAILURE;
    }

    const std::string fqn = std::string(Module::descriptor).append("/virtual");
    binder_status_t status = AServiceManager_addService(module->asBinder().get(), fqn.c_str());
    if (status != STATUS_OK) {
        LOG(ERROR) << "failed to register " << fqn << " (status=" << status
                   << "); check the VINTF fragment declares IModule/virtual and SELinux allows the "
                      "add_service";
        return EXIT_FAILURE;
    }
    LOG(INFO) << "registered " << fqn;

    ABinderProcess_joinThreadPool();
    return EXIT_FAILURE;  // not reached
}
