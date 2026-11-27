// core.decklink: the production module. Devices come from the DeckLink SDK
// adapter, built only where the SDK is installed (DECKLINK_SDK_DIR); without
// it the module loads, logs why, and reports no devices.
//
// SPDX-License-Identifier: MPL-2.0
#include <memory>

#include "module.h"

namespace decklink {
std::unique_ptr<DeviceProvider> make_sdk_provider(const em_host_api* host);  // sdk/ or the stub below
}

#ifndef DECKLINK_HAVE_SDK
namespace decklink {
namespace {
class NoDevices final : public DeviceProvider {
public:
    std::vector<SubDevice*> devices() override { return {}; }
};
}  // namespace
std::unique_ptr<DeviceProvider> make_sdk_provider(const em_host_api*) { return std::make_unique<NoDevices>(); }
}  // namespace decklink
#endif

namespace {

em_result init(const em_host_api* host, void** ctx) {
    auto* m = new decklink::Module(host, "core.decklink", decklink::make_sdk_provider(host));
#ifndef DECKLINK_HAVE_SDK
    m->log(EM_LOG_WARNING, "built without the DeckLink SDK; no devices");
#endif
    *ctx = m;
    return m->register_providers();
}

void shutdown(void* ctx) { delete static_cast<decklink::Module*>(ctx); }

}  // namespace

extern "C" EM_EXPORT const em_module_descriptor* em_module_entry(void) {
    static const em_module_descriptor d = {EM_ABI_VERSION, sizeof(em_module_descriptor), "core.decklink",
                                           "Blackmagic DeckLink", "0.1.0", "MPL-2.0", init, shutdown};
    return &d;
}
