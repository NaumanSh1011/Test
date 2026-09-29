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
 * Streams are socket-backed (ModuleVirtualSco / StreamVirtualSco): OUTPUT is tapped to the app
 * client over the va_server bridge (@virtual_audio) as downlink, INPUT is served from it as uplink.
 */

#include <cstdlib>

#define LOG_TAG "AHAL_VirtualSco"
#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

#include <core-impl/Module.h>

#include "ModuleVirtualSco.h"
#include "VirtualScoConfiguration.h"

using aidl::android::hardware::audio::core::Module;
using aidl::android::hardware::audio::core::ModuleVirtualSco;

int main() {
    android::base::SetMinimumLogSeverity(::android::base::DEBUG);
    ABinderProcess_setThreadPoolMaxThreadCount(16);
    LOG(INFO) << "AICaller virtual SCO AIDL module starting";

    // AOSP's complete Module implementation with socket-backed streams; starts the va_server bridge.
    auto module = ndk::SharedRefBase::make<ModuleVirtualSco>(
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
