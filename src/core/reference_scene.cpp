// SPDX-License-Identifier: MPL-2.0
#include "core/reference_scene.h"

#include <filesystem>
#include <fstream>
#include <sstream>

#include <google/protobuf/util/json_util.h>

namespace engine {

std::optional<testing_v1::ReferenceScene> load_reference_scene(const std::string& path, std::string& error) {
    std::ifstream in(path);
    if (!in) {
        error = "cannot open " + path;
        return std::nullopt;
    }
    std::stringstream buf;
    buf << in.rdbuf();
    testing_v1::ReferenceScene scene;
    auto status = google::protobuf::util::JsonStringToMessage(buf.str(), &scene);
    if (!status.ok()) {
        error = path + ": " + std::string(status.message());
        return std::nullopt;
    }
    const auto dir = std::filesystem::path(path).parent_path();
    for (auto& m : *scene.mutable_document()->mutable_media())
        if (!m.hint_path().empty() && std::filesystem::path(m.hint_path()).is_relative())
            m.set_hint_path((dir / m.hint_path()).lexically_normal().string());
    return scene;
}

std::optional<LiveCommand> to_live_command(const v1::Command& command, const DocumentIndex& doc, std::string& error) {
    LiveCommand c;
    switch (command.kind_case()) {
    case v1::Command::kTakeSlide: {
        const auto& t = command.take_slide();
        auto idx = doc.find_slide(t.presentation_id(), t.slide_id());
        if (!idx) {
            error = "unknown slide " + t.presentation_id() + "/" + t.slide_id();
            return std::nullopt;
        }
        c.kind = LiveCommand::Kind::TakeSlide;
        c.index = *idx;
        if (t.has_transition_override()) {
            c.has_transition_override = true;
            c.transition = to_transition(t.transition_override());
        }
        return c;
    }
    case v1::Command::kClearLayer:
        c.kind = LiveCommand::Kind::ClearLayer;
        c.layer = command.clear_layer().layer();
        if (command.clear_layer().has_transition()) {
            c.has_transition_override = true;
            c.transition = to_transition(command.clear_layer().transition());
        }
        return c;
    case v1::Command::kShowFixture:
    case v1::Command::kDismissFixture: {
        const bool show = command.has_show_fixture();
        const auto& id = show ? command.show_fixture().fixture_id() : command.dismiss_fixture().fixture_id();
        auto idx = doc.find_fixture(id);
        if (!idx) {
            error = "unknown fixture " + id;
            return std::nullopt;
        }
        c.kind = show ? LiveCommand::Kind::ShowFixture : LiveCommand::Kind::DismissFixture;
        c.index = *idx;
        return c;
    }
    default:
        error = "command not supported by the prototype yet";
        return std::nullopt;
    }
}

}  // namespace engine
