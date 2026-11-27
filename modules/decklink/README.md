# DeckLink module

SDI output and input through Blackmagic DeckLink cards, as a native module (`include/engine/module.h`). It's distributed separately from the MPL-2.0 core so the proprietary SDK never enters the core binary.

| File | What |
| --- | --- |
| `device.h` | The device seam: the slice of the SDK's scheduled-playback model the module uses |
| `module.cpp` | `em_output_provider` and `em_input_provider` ("sdi") on top of the seam. Portable, fully tested |
| `fake/` | FakeDeckLink: a simulated Duo 2 on an injected clock |
| `entry_sdk.cpp` | `core.decklink`: the production module |
| `entry_fake.cpp` | `core.decklink-fake`: the same module driving FakeDeckLink in real time, for engine runs and soaks with no hardware |

## How it maps to the card

- **Timing.** Frame *n* is scheduled at display time `n * den` in timescale `num` (e.g. 1001/60000), so slot times are exact forever. The card is the master clock: after each completion the module re-anchors engine time to the card's playback timeline, so `frame_needed` display times follow the card's crystal or, when genlocked, house sync.
- **Preroll.** The engine schedules frames `0..preroll-1`, then calls `start`. Each completion asks for the frame that keeps `preroll` frames queued.
- **Misses.** A slot with no frame repeats the previous frame on air (as the card does) and counts as dropped. A frame scheduled after its slot completes as `DROPPED`.
- **Audio.** Interleaved int32 at 48 kHz, placed on the playback timeline by its `presentation_time_ns`, so audio for frame *n* starts with frame *n*'s video.
- **Keying.** External key takes BGRA8; the card puts fill on the fill connector and alpha as key on its pair, so `key_row_bytes` is 0. The pair connector reports busy while keying.
- **Genlock.** `EM_CAP_GENLOCK` when the card has a reference input. Status goes `LOCKING` → `LOCKED`, and loss is pushed through `status_changed`.

## Still to do

- **SDK adapter** (`DECKLINK_HAVE_SDK`): wrap `IDeckLinkIterator`, `IDeckLinkOutput` and `IDeckLinkInput` behind `device.h`. Without it, `core.decklink` loads and reports no devices.
- **Duo 2 keying pairs:** the fake pairs connectors 1+2 and 3+4. Confirm the keyer support per connector pair in the SDK before promising alpha out.
- **Input format detection:** inputs open at 1080p59.94 for now.
- **Hardware loop:** the weekly real-signal test on a cabled Duo 2. The fake can't reproduce driver, firmware or signal timing.
