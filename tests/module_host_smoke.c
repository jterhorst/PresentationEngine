/*
 * Minimal host that loads a module the way the engine will: dlopen, resolve
 * em_module_entry, check the ABI, init, open an output, preroll, run, stop.
 *
 * Usage: module_host_smoke <path-to-module>
 */
#define _POSIX_C_SOURCE 200809L
#include "engine/module.h"

#include <dlfcn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define POOL 8

static em_output_provider g_provider;
static bool g_have_provider;
static em_output_handle *g_handle;
static em_buffer_requirements g_req;
static void *g_pool[POOL];
static atomic_uint_fast64_t g_displayed, g_needed, g_flushed;

static int64_t host_now(void *ctx) {
    (void)ctx;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void host_log(void *ctx, em_log_level level, const char *id, const char *msg) {
    (void)ctx;
    printf("[%d] %s: %s\n", (int)level, id, msg);
}

static em_result host_register_output(void *ctx, const em_output_provider *p) {
    (void)ctx;
    if (p->struct_size < sizeof(em_output_provider)) return EM_ERR_INVALID_ARGUMENT;
    g_provider = *p;
    g_have_provider = true;
    return EM_OK;
}

static em_result host_register_input(void *ctx, const em_input_provider *p) {
    (void)ctx; (void)p;
    return EM_OK;
}

static void host_devices_changed(void *ctx, const char *id) { (void)ctx; (void)id; }

static void schedule(uint64_t frame) {
    em_video_buffer b = {
        .struct_size = sizeof b,
        .format = EM_PIXEL_FORMAT_V210,
        .width = 1920, .height = 1080,
        .row_bytes = g_req.row_bytes,
        .data = g_pool[frame % POOL],
        .size_bytes = g_req.size_bytes,
        .frame_number = frame,
    };
    g_provider.schedule_video(g_handle, &b);
}

static void on_frame_needed(void *ctx, uint64_t frame, int64_t display, int64_t deadline) {
    (void)ctx; (void)display; (void)deadline;
    atomic_fetch_add(&g_needed, 1);
    schedule(frame); /* a real engine renders here */
}

static void on_frame_completed(void *ctx, uint64_t frame, em_frame_result r) {
    (void)ctx; (void)frame;
    if (r == EM_FRAME_DISPLAYED) atomic_fetch_add(&g_displayed, 1);
    if (r == EM_FRAME_FLUSHED) atomic_fetch_add(&g_flushed, 1);
}

static void on_status(void *ctx, const em_output_status *s) { (void)ctx; (void)s; }

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s module\n", argv[0]); return 2; }
    void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    em_module_entry_fn entry = (em_module_entry_fn)dlsym(lib, EM_MODULE_ENTRY_SYMBOL);
    if (!entry) { fprintf(stderr, "missing %s\n", EM_MODULE_ENTRY_SYMBOL); return 1; }
    const em_module_descriptor *d = entry();
    if (d->abi_version != EM_ABI_VERSION) { fprintf(stderr, "ABI mismatch\n"); return 1; }
    printf("module %s %s (%s)\n", d->id, d->version, d->license);

    em_host_api host = {
        .abi_version = EM_ABI_VERSION, .struct_size = sizeof host, .host_ctx = NULL,
        .now_ns = host_now, .log = host_log,
        .register_output_provider = host_register_output,
        .register_input_provider = host_register_input,
        .devices_changed = host_devices_changed,
    };
    void *mctx = NULL;
    if (d->init(&host, &mctx) != EM_OK || !g_have_provider) { fprintf(stderr, "init failed\n"); return 1; }

    em_device_info devs[4];
    uint32_t n = 0;
    if (g_provider.enumerate(g_provider.provider_ctx, devs, 4, &n) != EM_OK || n == 0) return 1;
    printf("device: %s (%s)\n", devs[0].name, devs[0].id);

    em_output_open_params p = {
        .struct_size = sizeof p, .device_id = devs[0].id, .connector_index = 0,
        .mode = {1920, 1080, {60000, 1001}, false}, .format = EM_PIXEL_FORMAT_V210,
        .key_mode = EM_KEY_MODE_NONE, .genlock = false,
        .audio_channels = 2, .audio_sample_rate = 48000, .preroll_frames = 3,
    };
    if (g_provider.query_buffer_requirements(g_provider.provider_ctx, &p, &g_req) != EM_OK) return 1;
    printf("v210 1080p row_bytes=%u size=%zu\n", g_req.row_bytes, g_req.size_bytes);
    for (int i = 0; i < POOL; i++) {
        if (posix_memalign(&g_pool[i], g_req.alignment, g_req.size_bytes) != 0) return 1;
        memset(g_pool[i], 0, g_req.size_bytes);
    }

    em_output_callbacks cb = {
        .struct_size = sizeof cb, .engine_ctx = NULL,
        .frame_needed = on_frame_needed, .frame_completed = on_frame_completed,
        .status_changed = on_status,
    };
    if (g_provider.open(g_provider.provider_ctx, &p, &cb, &g_handle) != EM_OK) return 1;
    for (uint64_t f = 0; f < p.preroll_frames; f++) schedule(f); /* preroll */
    if (g_provider.start(g_handle, host_now(NULL)) != EM_OK) return 1;

    struct timespec run = {0, 500000000L}; /* 0.5 s */
    nanosleep(&run, NULL);

    em_output_status st;
    g_provider.get_status(g_handle, &st);
    g_provider.stop(g_handle);
    g_provider.close(g_handle);
    d->shutdown(mctx);
    for (int i = 0; i < POOL; i++) free(g_pool[i]);

    uint64_t shown = atomic_load(&g_displayed);
    printf("displayed=%llu needed=%llu dropped=%llu flushed=%llu\n",
           (unsigned long long)shown, (unsigned long long)atomic_load(&g_needed),
           (unsigned long long)st.frames_dropped, (unsigned long long)atomic_load(&g_flushed));
    /* ~30 frames expected at 59.94 in 0.5 s; allow scheduler slack. */
    if (shown < 20 || st.frames_dropped != 0) { fprintf(stderr, "FAIL\n"); return 1; }
    printf("PASS\n");
    return 0;
}
