// The DeckLink module: em_output_provider ("sdi") and em_input_provider
// ("sdi") implemented on the device seam. Everything here is portable; the
// SDK adapter and FakeDeckLink sit behind decklink::DeviceProvider.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>
#include <string>

#include "device.h"
#include "engine/module.h"

namespace decklink {

class Module {
public:
    Module(const em_host_api* host, std::string module_id, std::unique_ptr<DeviceProvider> devices);
    ~Module();

    // Registers the output and input providers with the host.
    em_result register_providers();

    const em_host_api* host() const { return host_; }
    const std::string& id() const { return id_; }
    DeviceProvider& devices() { return *devices_; }
    SubDevice* find(const char* em_device_id);
    void log(em_log_level level, const std::string& message) const;

    // Device ids handed to the engine: "decklink:<persistent id>".
    static std::string device_id(const SubDevice& d);

private:
    const em_host_api* host_;
    std::string id_;
    std::unique_ptr<DeviceProvider> devices_;
    em_output_provider output_{};
    em_input_provider input_{};
};

}  // namespace decklink
