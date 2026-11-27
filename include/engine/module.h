/*
 * Native module ABI (tier 3: trusted I/O code loaded into the engine).
 *
 * A module is a dynamic library (.dylib on macOS) exporting one C symbol,
 * em_module_entry, that returns a descriptor. Through the host API it registers
 * output providers (SDI, NDI, ...) and input providers.
 *
 * Why a C ABI: modules may be built with a different compiler or C++ standard
 * library than the engine, and may wrap proprietary SDKs. C structs and function
 * pointers are the only stable boundary across that.
 *
 * Stability rules
 * - EM_ABI_VERSION bumps only on incompatible changes. The engine refuses a
 *   module whose abi_version differs.
 * - Structs that may grow carry struct_size as their first or second field.
 *   New fields are only ever appended; readers check struct_size before reading
 *   a field added later.
 * - Enum values are never renumbered; new values are appended.
 *
 * Threading rules
 * - Host API functions are thread-safe unless noted.
 * - Provider functions may be called from any engine thread, but never
 *   concurrently on the same handle.
 * - Callbacks into the engine (em_output_callbacks, em_input_callbacks) may be
 *   called from the provider's own threads. They never block.
 *
 * Memory rules
 * - Output video buffers are allocated by the engine (GPU-shared, page aligned,
 *   sized by query_buffer_requirements). A scheduled buffer stays owned by the
 *   engine and must not be written by the module; the module may read it until
 *   it reports frame_completed for that frame_number.
 * - Input buffers are allocated by the module and returned with a release
 *   function the engine calls when it is done (possibly on another thread).
 */
#ifndef ENGINE_MODULE_H
#define ENGINE_MODULE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EM_ABI_VERSION 1u
#define EM_MODULE_ENTRY_SYMBOL "em_module_entry"

#if defined(_WIN32)
#define EM_EXPORT __declspec(dllexport)
#else
#define EM_EXPORT __attribute__((visibility("default")))
#endif

/* ------------------------------------------------------------------------ */
/* Basic types                                                              */
/* ------------------------------------------------------------------------ */

typedef enum em_result {
    EM_OK = 0,
    EM_ERR_INVALID_ARGUMENT = -1,
    EM_ERR_UNSUPPORTED = -2,
    EM_ERR_DEVICE_NOT_FOUND = -3,
    EM_ERR_DEVICE_LOST = -4,
    EM_ERR_BUSY = -5,
    EM_ERR_NO_MEMORY = -6,
    EM_ERR_TIMEOUT = -7,
    EM_ERR_BUFFER_TOO_SMALL = -8,
    EM_ERR_INTERNAL = -100
} em_result;

typedef enum em_log_level {
    EM_LOG_DEBUG = 1,
    EM_LOG_INFO = 2,
    EM_LOG_WARNING = 3,
    EM_LOG_ERROR = 4
} em_log_level;

typedef struct em_rational {
    int32_t num;
    int32_t den;
} em_rational;

typedef enum em_pixel_format {
    EM_PIXEL_FORMAT_BGRA8 = 1,
    EM_PIXEL_FORMAT_UYVY8 = 2,
    EM_PIXEL_FORMAT_V210 = 3,
    EM_PIXEL_FORMAT_RGBA16F = 4
} em_pixel_format;

typedef struct em_video_mode {
    uint32_t width;
    uint32_t height;
    em_rational frame_rate; /* e.g. {60000, 1001} */
    bool interlaced;
} em_video_mode;

typedef enum em_key_mode {
    EM_KEY_MODE_NONE = 0,
    EM_KEY_MODE_EXTERNAL = 1, /* fill + key on a connector pair */
    EM_KEY_MODE_INTERNAL = 2  /* card keys over its own input */
} em_key_mode;

typedef enum em_genlock_state {
    EM_GENLOCK_NOT_USED = 0,
    EM_GENLOCK_NO_REFERENCE = 1,
    EM_GENLOCK_LOCKING = 2,
    EM_GENLOCK_LOCKED = 3
} em_genlock_state;

/* Device capability bits (em_device_info.caps). */
#define EM_CAP_OUTPUT        (1u << 0)
#define EM_CAP_INPUT         (1u << 1)
#define EM_CAP_GENLOCK       (1u << 2)
#define EM_CAP_KEY_EXTERNAL  (1u << 3)
#define EM_CAP_KEY_INTERNAL  (1u << 4)
#define EM_CAP_AUDIO         (1u << 5)
#define EM_CAP_ALPHA         (1u << 6)

typedef struct em_device_info {
    char id[128];   /* stable across reboots, e.g. persistent id + connector */
    char name[128]; /* operator-facing */
    uint32_t connector_count;
    uint32_t caps;  /* EM_CAP_* */
} em_device_info;

/* A video frame in memory. For outputs the engine owns it; for inputs the module does. */
typedef struct em_video_buffer {
    uint32_t struct_size;
    em_pixel_format format;
    uint32_t width;
    uint32_t height;
    uint32_t row_bytes;
    void *data;
    size_t size_bytes;
    void *key_data;             /* EM_KEY_MODE_EXTERNAL: key plane, else NULL */
    uint32_t key_row_bytes;
    void *native_handle;        /* macOS: unretained id<MTLBuffer> or IOSurfaceRef, may be NULL */
    uint64_t frame_number;
    int64_t presentation_time_ns; /* engine clock */
} em_video_buffer;

/* Interleaved signed 32-bit PCM (SDI-native). */
typedef struct em_audio_buffer {
    uint32_t struct_size;
    const int32_t *samples;
    uint32_t channel_count;
    uint32_t frame_count;       /* samples per channel */
    uint32_t sample_rate;       /* 48000 for SDI */
    int64_t presentation_time_ns;
} em_audio_buffer;

/* ------------------------------------------------------------------------ */
/* Output providers                                                         */
/* ------------------------------------------------------------------------ */

typedef struct em_output_handle em_output_handle; /* opaque, module-defined */

typedef struct em_output_open_params {
    uint32_t struct_size;
    const char *device_id;
    uint32_t connector_index;
    em_video_mode mode;
    em_pixel_format format;
    em_key_mode key_mode;
    bool genlock;
    uint32_t audio_channels;    /* 0 = no audio */
    uint32_t audio_sample_rate;
    uint32_t preroll_frames;    /* frames to schedule before start, >= 2 */
} em_output_open_params;

typedef struct em_buffer_requirements {
    uint32_t struct_size;
    uint32_t row_bytes;
    size_t size_bytes;
    size_t alignment;           /* power of two */
    uint32_t key_row_bytes;     /* 0 unless EM_KEY_MODE_EXTERNAL */
} em_buffer_requirements;

typedef enum em_frame_result {
    EM_FRAME_DISPLAYED = 0,
    EM_FRAME_LATE = 1,          /* displayed after its slot */
    EM_FRAME_DROPPED = 2,       /* never displayed */
    EM_FRAME_FLUSHED = 3        /* discarded by stop() */
} em_frame_result;

typedef struct em_output_status {
    uint32_t struct_size;
    em_genlock_state genlock;
    uint32_t buffered_frames;
    uint64_t frames_late;
    uint64_t frames_dropped;
    em_result last_error;
} em_output_status;

/* Engine callbacks the provider calls. The provider's clock drives the engine:
 * frame_needed is the output's tick. */
typedef struct em_output_callbacks {
    uint32_t struct_size;
    void *engine_ctx;
    /* The output needs frame `frame_number`, shown at display_time_ns. The engine
     * calls schedule_video before deadline_ns or the provider applies its miss policy. */
    void (*frame_needed)(void *engine_ctx, uint64_t frame_number,
                         int64_t display_time_ns, int64_t deadline_ns);
    /* The module is done reading the buffer for frame_number. */
    void (*frame_completed)(void *engine_ctx, uint64_t frame_number, em_frame_result result);
    void (*status_changed)(void *engine_ctx, const em_output_status *status);
} em_output_callbacks;

typedef struct em_output_provider {
    uint32_t struct_size;
    const char *kind;           /* "sdi", "ndi", "screen", or a module-specific kind */
    void *provider_ctx;

    em_result (*enumerate)(void *provider_ctx, em_device_info *out,
                           uint32_t capacity, uint32_t *count);
    em_result (*query_buffer_requirements)(void *provider_ctx,
                                           const em_output_open_params *params,
                                           em_buffer_requirements *out);
    em_result (*open)(void *provider_ctx, const em_output_open_params *params,
                      const em_output_callbacks *callbacks, em_output_handle **out);
    /* Begin playback at start_time_ns (engine clock). Preroll frames must already be scheduled. */
    em_result (*start)(em_output_handle *handle, int64_t start_time_ns);
    em_result (*schedule_video)(em_output_handle *handle, const em_video_buffer *buffer);
    em_result (*schedule_audio)(em_output_handle *handle, const em_audio_buffer *buffer);
    em_result (*get_status)(em_output_handle *handle, em_output_status *out);
    em_result (*stop)(em_output_handle *handle);  /* flushes scheduled frames */
    void (*close)(em_output_handle *handle);       /* after stop; handle is invalid afterwards */
} em_output_provider;

/* ------------------------------------------------------------------------ */
/* Input providers                                                          */
/* ------------------------------------------------------------------------ */

typedef struct em_input_handle em_input_handle; /* opaque, module-defined */

typedef struct em_input_open_params {
    uint32_t struct_size;
    const char *device_id;      /* SDI device id or NDI source name */
    uint32_t connector_index;
    em_pixel_format preferred_format;
    bool capture_audio;
    bool low_bandwidth;         /* NDI proxy stream */
} em_input_open_params;

/* A captured frame; the engine calls release(release_ctx) exactly once. */
typedef struct em_input_frame {
    uint32_t struct_size;
    em_video_buffer video;
    void (*release)(void *release_ctx);
    void *release_ctx;
} em_input_frame;

typedef struct em_input_callbacks {
    uint32_t struct_size;
    void *engine_ctx;
    void (*video_arrived)(void *engine_ctx, const em_input_frame *frame);
    void (*audio_arrived)(void *engine_ctx, const em_audio_buffer *audio); /* copy before returning */
    void (*format_changed)(void *engine_ctx, const em_video_mode *mode, em_pixel_format format);
    void (*signal_lost)(void *engine_ctx);
} em_input_callbacks;

typedef struct em_input_provider {
    uint32_t struct_size;
    const char *kind;           /* "sdi", "ndi", ... */
    void *provider_ctx;

    em_result (*enumerate)(void *provider_ctx, em_device_info *out,
                           uint32_t capacity, uint32_t *count);
    em_result (*open)(void *provider_ctx, const em_input_open_params *params,
                      const em_input_callbacks *callbacks, em_input_handle **out);
    em_result (*start)(em_input_handle *handle);
    em_result (*stop)(em_input_handle *handle);
    void (*close)(em_input_handle *handle);
} em_input_provider;

/* ------------------------------------------------------------------------ */
/* Host API and module entry                                                */
/* ------------------------------------------------------------------------ */

typedef struct em_host_api {
    uint32_t abi_version;
    uint32_t struct_size;
    void *host_ctx;

    int64_t (*now_ns)(void *host_ctx); /* engine monotonic clock */
    void (*log)(void *host_ctx, em_log_level level, const char *module_id, const char *message);

    /* Providers are copied by the host; the module keeps provider_ctx alive until shutdown. */
    em_result (*register_output_provider)(void *host_ctx, const em_output_provider *provider);
    em_result (*register_input_provider)(void *host_ctx, const em_input_provider *provider);

    /* Hot-plug: tell the engine to re-enumerate this module's devices. */
    void (*devices_changed)(void *host_ctx, const char *module_id);
} em_host_api;

typedef struct em_module_descriptor {
    uint32_t abi_version;       /* EM_ABI_VERSION the module was built against */
    uint32_t struct_size;
    const char *id;             /* reverse-DNS, e.g. "org.example.decklink" */
    const char *name;
    const char *version;        /* semver */
    const char *license;        /* SPDX identifier, e.g. "MPL-2.0" */

    /* Called once after loading. Register providers here. */
    em_result (*init)(const em_host_api *host, void **module_ctx);
    /* Called once before unloading, after every handle is closed. */
    void (*shutdown)(void *module_ctx);
} em_module_descriptor;

typedef const em_module_descriptor *(*em_module_entry_fn)(void);

/* Every module defines exactly this symbol. */
EM_EXPORT const em_module_descriptor *em_module_entry(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ENGINE_MODULE_H */
