// What is on air: background, live slide, fixtures and any running transition.
// Mutated only by apply(), which runs at a frame edge on the render thread.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <array>
#include <cstdint>

#include "core/clock.h"
#include "core/command_queue.h"
#include "core/document_index.h"

namespace engine {

enum class TransitionKind : uint8_t { Cut, Dissolve };

struct TransitionChoice {
    TransitionKind kind = TransitionKind::Cut;
    Nanos duration = 0;
    v1::Easing easing = v1::EASING_UNSPECIFIED;
};

// "core" provides cut and dissolve; anything else falls back to dissolve for now.
TransitionChoice to_transition(const v1::TransitionSpec& spec);

// A command resolved against a loaded document, small and trivially copyable
// so it crosses the SPSC queue without allocating.
struct LiveCommand {
    enum class Kind : uint8_t { LoadDocument, TakeSlide, ClearLayer, ShowFixture, DismissFixture };
    Kind kind = Kind::TakeSlide;
    const DocumentIndex* document = nullptr;  // LoadDocument; must outlive its use (owned by the control side)
    uint32_t index = 0;                       // slide index (TakeSlide) or fixture index
    v1::LayerTarget layer = v1::LAYER_TARGET_UNSPECIFIED;
    bool has_transition_override = false;
    TransitionChoice transition;
};

using CommandQueue = SpscQueue<LiveCommand, 256>;

constexpr uint32_t kMaxFixtures = 32;
constexpr uint32_t kNone = UINT32_MAX;

struct LiveState {
    const DocumentIndex* document = nullptr;
    uint64_t state_revision = 0;

    const v1::Background* background = nullptr;  // points into the document; nullptr = none
    uint32_t slide = kNone;                      // index into DocumentIndex::slides
    uint32_t previous_slide = kNone;             // outgoing side of a running transition
    Nanos transition_start = 0;
    TransitionChoice transition;

    std::array<uint32_t, kMaxFixtures> fixtures{};
    uint32_t fixture_count = 0;

    // Applies one command at frame edge `edge`. Returns false if it was rejected.
    bool apply(const LiveCommand& c, Nanos edge);

    // Drains the queue at a frame edge. No allocation, no locks.
    uint32_t apply_pending(CommandQueue& queue, Nanos edge);

    bool transition_running(Nanos t) const;
    float transition_progress(Nanos t) const;  // eased, 0..1

private:
    void take(uint32_t slide_index, const TransitionChoice* override_choice, Nanos edge);
    void clear(v1::LayerTarget layer, Nanos edge, const TransitionChoice* override_choice);
    void show_fixture(uint32_t fixture);
    void dismiss_fixture(uint32_t fixture);
};

}  // namespace engine
