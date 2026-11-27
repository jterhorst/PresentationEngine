// SPDX-License-Identifier: MPL-2.0
#include "core/live_state.h"

#include <algorithm>

namespace engine {

TransitionChoice to_transition(const v1::TransitionSpec& spec) {
    TransitionChoice t;
    t.duration = ms_to_ns(spec.duration_ms());
    t.easing = spec.easing();
    const bool cut = spec.transition_id().empty() || spec.transition_id() == "cut" || t.duration == 0;
    t.kind = cut ? TransitionKind::Cut : TransitionKind::Dissolve;
    return t;
}

bool LiveState::apply(const LiveCommand& c, Nanos edge) {
    if (c.kind != LiveCommand::Kind::LoadDocument && !document) return false;
    const TransitionChoice* override_choice = c.has_transition_override ? &c.transition : nullptr;
    switch (c.kind) {
    case LiveCommand::Kind::LoadDocument:
        // Prototype: a new document clears the live layers. Phase 1 keeps the
        // live slide on its old revision and flags it stale until Retake.
        document = c.document;
        slide = previous_slide = kNone;
        background = nullptr;
        fixture_count = 0;
        break;
    case LiveCommand::Kind::TakeSlide:
        if (c.index >= document->slides.size()) return false;
        take(c.index, override_choice, edge);
        break;
    case LiveCommand::Kind::ClearLayer:
        clear(c.layer, edge, override_choice);
        break;
    case LiveCommand::Kind::ShowFixture:
        if (c.index >= document->fixtures.size()) return false;
        show_fixture(c.index);
        break;
    case LiveCommand::Kind::DismissFixture:
        dismiss_fixture(c.index);
        break;
    }
    ++state_revision;
    return true;
}

uint32_t LiveState::apply_pending(CommandQueue& queue, Nanos edge) {
    uint32_t n = 0;
    LiveCommand c;
    while (queue.pop(c)) n += apply(c, edge) ? 1 : 0;
    return n;
}

void LiveState::take(uint32_t slide_index, const TransitionChoice* override_choice, Nanos edge) {
    const SlideRef& ref = document->slides[slide_index];
    const v1::Slide& s = *ref.slide;
    TransitionChoice choice;
    if (override_choice) choice = *override_choice;
    else if (s.has_transition()) choice = to_transition(s.transition());
    else choice = to_transition(document->presentations[ref.presentation].presentation->default_transition());

    if (choice.kind == TransitionKind::Dissolve) {
        previous_slide = slide;
        transition_start = edge;
        transition = choice;
    } else {
        previous_slide = kNone;
        transition = {};
    }
    slide = slide_index;
    if (s.has_background()) background = &s.background();

    for (const auto& action : s.actions()) {
        if (action.has_show_fixture_id()) {
            if (auto f = document->find_fixture(action.show_fixture_id())) show_fixture(*f);
        } else if (action.has_dismiss_fixture_id()) {
            if (auto f = document->find_fixture(action.dismiss_fixture_id())) dismiss_fixture(*f);
        } else if (action.has_set_background()) {
            background = &action.set_background();
        } else if (action.has_clear_layer() && action.clear_layer() != v1::LAYER_TARGET_SLIDE) {
            clear(action.clear_layer(), edge, nullptr);
        }
    }
}

void LiveState::clear(v1::LayerTarget layer, Nanos edge, const TransitionChoice* override_choice) {
    const bool all = layer == v1::LAYER_TARGET_ALL;
    if (all || layer == v1::LAYER_TARGET_BACKGROUND) background = nullptr;
    if (all || layer == v1::LAYER_TARGET_FIXTURES) fixture_count = 0;
    if ((all || layer == v1::LAYER_TARGET_SLIDE) && slide != kNone) {
        if (override_choice && override_choice->kind == TransitionKind::Dissolve) {
            previous_slide = slide;
            transition_start = edge;
            transition = *override_choice;
        } else {
            previous_slide = kNone;
        }
        slide = kNone;
    }
}

void LiveState::show_fixture(uint32_t fixture) {
    for (uint32_t i = 0; i < fixture_count; ++i)
        if (fixtures[i] == fixture) return;
    if (fixture_count < kMaxFixtures) fixtures[fixture_count++] = fixture;
}

void LiveState::dismiss_fixture(uint32_t fixture) {
    for (uint32_t i = 0; i < fixture_count; ++i)
        if (fixtures[i] == fixture) {
            std::copy(fixtures.begin() + i + 1, fixtures.begin() + fixture_count, fixtures.begin() + i);
            --fixture_count;
            return;
        }
}

bool LiveState::transition_running(Nanos t) const {
    return transition.kind == TransitionKind::Dissolve && t < transition_start + transition.duration;
}

float LiveState::transition_progress(Nanos t) const {
    if (!transition_running(t)) return 1.0f;
    float x = static_cast<float>(t - transition_start) / static_cast<float>(transition.duration);
    x = std::clamp(x, 0.0f, 1.0f);
    switch (transition.easing) {
    case v1::EASING_EASE_IN: return x * x;
    case v1::EASING_EASE_OUT: return 1 - (1 - x) * (1 - x);
    case v1::EASING_EASE_IN_OUT: return x < 0.5f ? 2 * x * x : 1 - 2 * (1 - x) * (1 - x);
    default: return x;
    }
}

}  // namespace engine
