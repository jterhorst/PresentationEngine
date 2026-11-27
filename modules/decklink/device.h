// The DeckLink module's device seam (architecture.md, "Simulated DeckLink").
//
// The module talks to cards only through these interfaces. Production wraps
// the DeckLink SDK (IDeckLinkIterator -> IDeckLinkOutput / IDeckLinkInput) in
// an adapter built where the SDK is installed; tests and the nightly loopback
// use FakeDeckLink (fake/fake_decklink.h), which runs on an injected clock.
//
// The calls mirror the SDK's scheduled-playback model on purpose (enable
// output, schedule frames at display times in a timescale, start scheduled
// playback, completion callbacks, hardware reference clock, reference
// status) so the adapter stays a thin translation.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace decklink {

enum class Result { Ok, Unsupported, Busy, DeviceLost, InvalidArgument };

enum class PixelFormat { UYVY8, V210, BGRA8 };

struct DisplayMode {
    uint32_t width = 1920, height = 1080;
    int32_t rate_num = 60000, rate_den = 1001;  // frames per second = num / den
    bool interlaced = false;
    bool operator==(const DisplayMode&) const = default;
};

enum class KeyMode { None, External, Internal };

// IDeckLinkVideoOutputCallback::ScheduledFrameCompleted results.
enum class FrameResult { Completed, DisplayedLate, Dropped, Flushed };

// BMDReferenceStatus, plus "no reference input on this card".
enum class ReferenceStatus { NotSupported, NoReference, Locking, Locked };

// Uppercase IDs in the SDK; here a handle the device echoes back on completion.
struct OutputFrame {
    const void* data = nullptr;
    uint32_t row_bytes = 0;
    PixelFormat format = PixelFormat::V210;
    uint32_t width = 0, height = 0;
    uint64_t cookie = 0;             // the engine's frame number
};
// Keying: as with the SDK, an External key output takes BGRA8 frames and the
// card puts fill on this connector and the alpha as key on its pair.

class OutputCallback {
public:
    virtual ~OutputCallback() = default;
    virtual void frame_completed(uint64_t cookie, FrameResult result) = 0;
    virtual void device_removed() = 0;
};

struct InputFrame {
    const void* data = nullptr;
    uint32_t row_bytes = 0;
    PixelFormat format = PixelFormat::V210;
    uint32_t width = 0, height = 0;
    int64_t stream_time = 0;         // in the input's timescale
    int64_t timescale = 0;
};

class InputCallback {
public:
    virtual ~InputCallback() = default;
    virtual void frame_arrived(const InputFrame& frame) = 0;  // data valid only during the call
    virtual void signal_lost() = 0;
    virtual void device_removed() = 0;
};

struct SubDeviceInfo {
    std::string persistent_id;       // stable across reboots
    std::string display_name;        // e.g. "DeckLink Duo (1)"
    std::string model;               // e.g. "DeckLink Duo 2"
    bool can_output = false;
    bool can_input = false;
    bool external_key = false;       // can be the fill side of a fill + key pair
    bool internal_key = false;
    bool reference_input = false;    // genlock
};

// One sub-device (an SDI connector, in the Duo 2's default profile).
class SubDevice {
public:
    virtual ~SubDevice() = default;
    virtual const SubDeviceInfo& info() const = 0;

    // Output (IDeckLinkOutput)
    virtual bool supports(const DisplayMode& mode, PixelFormat format) const = 0;
    virtual Result enable_video_output(const DisplayMode& mode, KeyMode key) = 0;
    virtual Result enable_audio_output(uint32_t sample_rate, uint32_t channels) = 0;
    virtual void set_output_callback(OutputCallback* cb) = 0;
    // Shows `frame` at display_time / timescale seconds on the device's
    // playback timeline, for `duration` units.
    virtual Result schedule_video_frame(const OutputFrame& frame, int64_t display_time, int64_t duration,
                                        int64_t timescale) = 0;
    // Interleaved int32 samples starting at stream_time / timescale.
    virtual Result schedule_audio_samples(const int32_t* samples, uint32_t frame_count, int64_t stream_time,
                                          int64_t timescale) = 0;
    virtual Result start_scheduled_playback(int64_t start_time, int64_t timescale) = 0;
    virtual Result stop_scheduled_playback() = 0;  // flushes; completions report Flushed
    virtual Result disable_output() = 0;
    virtual uint32_t buffered_video_frames() const = 0;
    // Playback timeline position (GetScheduledStreamTime) in `timescale` units.
    virtual int64_t scheduled_stream_time(int64_t timescale) const = 0;
    virtual ReferenceStatus reference_status() const = 0;

    // Input (IDeckLinkInput)
    virtual Result enable_video_input(const DisplayMode& mode, PixelFormat format) = 0;
    virtual void set_input_callback(InputCallback* cb) = 0;
    virtual Result start_streams() = 0;
    virtual Result stop_streams() = 0;
    virtual Result disable_input() = 0;
};

class DeviceProvider {
public:
    virtual ~DeviceProvider() = default;
    virtual std::vector<SubDevice*> devices() = 0;  // non-owning; valid until the provider is destroyed
};

}  // namespace decklink
