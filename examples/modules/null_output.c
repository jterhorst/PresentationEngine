/*
 * Example native module: a "null" output that behaves like real playout hardware
 * (its own clock, preroll, scheduled frames, completion callbacks) but discards
 * pixels. Useful as a template for real modules and for CI.
 *
 * Build (macOS): clang -std=c11 -O2 -shared -fvisibility=hidden -Iinclude \
 *                  examples/modules/null_output.c -o null_output.dylib
 */
#define _POSIX_C_SOURCE 200809L
#include "engine/module.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RING 32

struct em_output_handle {
    em_output_callbacks cb;
    em_output_open_params params;
    char device_id[128];
    pthread_t thread;
    pthread_mutex_t lock;
    bool running;
    int64_t start_ns;
    uint64_t scheduled[RING]; /* frame_number + 1 in slot frame % RING; 0 = empty */
    em_output_status status;
};

static const em_host_api *g_host;

static int64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void sleep_until(int64_t t_ns) {
    int64_t now = mono_ns();
    if (t_ns <= now) return;
    struct timespec ts = {(time_t)((t_ns - now) / 1000000000LL), (long)((t_ns - now) % 1000000000LL)};
    nanosleep(&ts, NULL);
}

static int64_t frame_duration_ns(const em_video_mode *m) {
    return (int64_t)1000000000LL * m->frame_rate.den / m->frame_rate.num;
}

static void *tick_thread(void *arg) {
    em_output_handle *h = arg;
    const int64_t dur = frame_duration_ns(&h->params.mode);
    const uint64_t preroll = h->params.preroll_frames;
    for (uint64_t k = 0;; k++) {
        int64_t slot_time = h->start_ns + (int64_t)k * dur;
        sleep_until(slot_time);

        pthread_mutex_lock(&h->lock);
        if (!h->running) {
            pthread_mutex_unlock(&h->lock);
            break; /* stop() flushes whatever is still scheduled */
        }
        bool have = h->scheduled[k % RING] == k + 1;
        if (have) h->scheduled[k % RING] = 0;
        else h->status.frames_dropped++;
        pthread_mutex_unlock(&h->lock);

        if (have) h->cb.frame_completed(h->cb.engine_ctx, k, EM_FRAME_DISPLAYED);
        /* Ask for the frame that keeps the preroll depth constant. */
        uint64_t want = k + preroll;
        h->cb.frame_needed(h->cb.engine_ctx, want, h->start_ns + (int64_t)want * dur,
                           h->start_ns + (int64_t)(want - 1) * dur);
    }
    return NULL;
}

static em_result null_enumerate(void *ctx, em_device_info *out, uint32_t capacity, uint32_t *count) {
    (void)ctx;
    *count = 1;
    if (capacity < 1) return EM_ERR_BUFFER_TOO_SMALL;
    memset(&out[0], 0, sizeof out[0]);
    strcpy(out[0].id, "null:0");
    strcpy(out[0].name, "Null output");
    out[0].connector_count = 1;
    out[0].caps = EM_CAP_OUTPUT | EM_CAP_AUDIO | EM_CAP_ALPHA;
    return EM_OK;
}

static em_result null_query(void *ctx, const em_output_open_params *p, em_buffer_requirements *out) {
    (void)ctx;
    uint32_t bpp = p->format == EM_PIXEL_FORMAT_RGBA16F ? 8 : p->format == EM_PIXEL_FORMAT_BGRA8 ? 4 : 2;
    uint32_t row = p->format == EM_PIXEL_FORMAT_V210 ? ((p->mode.width + 47) / 48) * 128
                                                     : p->mode.width * bpp;
    out->struct_size = sizeof *out;
    out->row_bytes = row;
    out->size_bytes = (size_t)row * p->mode.height;
    out->alignment = 4096;
    out->key_row_bytes = p->key_mode == EM_KEY_MODE_EXTERNAL ? row : 0;
    return EM_OK;
}

static em_result null_open(void *ctx, const em_output_open_params *p, const em_output_callbacks *cb,
                           em_output_handle **out) {
    (void)ctx;
    if (!p || !cb || !out || p->mode.frame_rate.num <= 0 || p->mode.frame_rate.den <= 0 ||
        p->preroll_frames < 2 || p->preroll_frames >= RING)
        return EM_ERR_INVALID_ARGUMENT;
    if (!p->device_id || strcmp(p->device_id, "null:0") != 0) return EM_ERR_DEVICE_NOT_FOUND;
    em_output_handle *h = calloc(1, sizeof *h);
    if (!h) return EM_ERR_NO_MEMORY;
    h->cb = *cb;
    h->params = *p;
    strncpy(h->device_id, p->device_id, sizeof h->device_id - 1);
    h->params.device_id = h->device_id;
    pthread_mutex_init(&h->lock, NULL);
    h->status.struct_size = sizeof h->status;
    h->status.genlock = p->genlock ? EM_GENLOCK_NO_REFERENCE : EM_GENLOCK_NOT_USED;
    *out = h;
    return EM_OK;
}

static em_result null_schedule_video(em_output_handle *h, const em_video_buffer *b) {
    if (!h || !b) return EM_ERR_INVALID_ARGUMENT;
    pthread_mutex_lock(&h->lock);
    h->scheduled[b->frame_number % RING] = b->frame_number + 1;
    pthread_mutex_unlock(&h->lock);
    return EM_OK;
}

static em_result null_schedule_audio(em_output_handle *h, const em_audio_buffer *a) {
    return (h && a) ? EM_OK : EM_ERR_INVALID_ARGUMENT;
}

static em_result null_start(em_output_handle *h, int64_t start_time_ns) {
    (void)start_time_ns; /* a real device maps engine time to its clock */
    pthread_mutex_lock(&h->lock);
    h->running = true;
    h->start_ns = mono_ns() + frame_duration_ns(&h->params.mode);
    pthread_mutex_unlock(&h->lock);
    return pthread_create(&h->thread, NULL, tick_thread, h) == 0 ? EM_OK : EM_ERR_INTERNAL;
}

static em_result null_get_status(em_output_handle *h, em_output_status *out) {
    pthread_mutex_lock(&h->lock);
    *out = h->status;
    uint32_t n = 0;
    for (int i = 0; i < RING; i++) n += h->scheduled[i] != 0;
    out->buffered_frames = n;
    pthread_mutex_unlock(&h->lock);
    return EM_OK;
}

static em_result null_stop(em_output_handle *h) {
    pthread_mutex_lock(&h->lock);
    bool was = h->running;
    h->running = false;
    pthread_mutex_unlock(&h->lock);
    if (was) pthread_join(h->thread, NULL);
    for (int i = 0; i < RING; i++) {
        if (h->scheduled[i]) {
            h->cb.frame_completed(h->cb.engine_ctx, h->scheduled[i] - 1, EM_FRAME_FLUSHED);
            h->scheduled[i] = 0;
        }
    }
    return EM_OK;
}

static void null_close(em_output_handle *h) {
    pthread_mutex_destroy(&h->lock);
    free(h);
}

static em_result module_init(const em_host_api *host, void **module_ctx) {
    g_host = host;
    *module_ctx = NULL;
    static em_output_provider provider = {
        .struct_size = sizeof(em_output_provider),
        .kind = "null",
        .provider_ctx = NULL,
        .enumerate = null_enumerate,
        .query_buffer_requirements = null_query,
        .open = null_open,
        .start = null_start,
        .schedule_video = null_schedule_video,
        .schedule_audio = null_schedule_audio,
        .get_status = null_get_status,
        .stop = null_stop,
        .close = null_close,
    };
    host->log(host->host_ctx, EM_LOG_INFO, "org.example.null-output", "registering null output");
    return host->register_output_provider(host->host_ctx, &provider);
}

static void module_shutdown(void *module_ctx) {
    (void)module_ctx;
    g_host = NULL;
}

EM_EXPORT const em_module_descriptor *em_module_entry(void) {
    static const em_module_descriptor d = {
        .abi_version = EM_ABI_VERSION,
        .struct_size = sizeof(em_module_descriptor),
        .id = "org.example.null-output",
        .name = "Null output",
        .version = "1.0.0",
        .license = "MPL-2.0",
        .init = module_init,
        .shutdown = module_shutdown,
    };
    return &d;
}
