// Loads reference scenes (tests/scenes/reference_scene.proto) and turns
// protocol Commands into LiveCommands against a loaded document.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <optional>
#include <string>

#include "core/live_state.h"
#include "reference_scene.pb.h"

namespace engine {

namespace testing_v1 = ::engine::testing::v1;

// Parses a *.scene.json file. Media hint paths are resolved relative to the
// scene file's folder. Returns nullopt and fills `error` on failure.
std::optional<testing_v1::ReferenceScene> load_reference_scene(const std::string& path, std::string& error);

// Validates a protocol command against `doc` (as the engine does when a
// command arrives) and converts it for the render thread.
std::optional<LiveCommand> to_live_command(const v1::Command& command, const DocumentIndex& doc, std::string& error);

}  // namespace engine
