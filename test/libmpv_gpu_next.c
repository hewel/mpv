/*
 * This file is part of mpv.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

// Keep API calls in assertions active in release-configured test builds too.
#undef NDEBUG
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "mpv/gpu_next.h"

static void lock_queue(void *ctx, uint32_t family, uint32_t index) {}
static int acquire(void *ctx, mpv_gpu_next_target *target) { return 0; }
static void release(void *ctx, const mpv_gpu_next_target *target, int status) {}
static int acquire_frame(void *ctx, mpv_gpu_next_frame *frame) { return 0; }
static void release_frame(void *ctx, const mpv_gpu_next_frame *frame, int status) {}

static const VkPhysicalDeviceFeatures2 features = {
    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
};

// Registration borrows the descriptor without importing Vulkan objects.
static const mpv_gpu_next_host valid = {
    .version = MPV_GPU_NEXT_HOST_VERSION,
    .instance = (VkInstance)(uintptr_t)1,
    .physical_device = (VkPhysicalDevice)(uintptr_t)1,
    .device = (VkDevice)(uintptr_t)1,
    .features = &features,
    .lock_queue = lock_queue,
    .unlock_queue = lock_queue,
    .acquire = acquire,
    .release = release,
};

static mpv_handle *create(void)
{
    mpv_handle *ctx = mpv_create();
    assert(ctx);
    assert(mpv_set_option_string(ctx, "config", "no") == 0);
    assert(mpv_set_option_string(ctx, "vo", "null") == 0);
    return ctx;
}

static void check_legacy_prefix(uint32_t version, size_t size)
{
    // Place the historical descriptor against an inaccessible page: even a
    // read of an appended callback is an observable memory-safety regression.
    long page = sysconf(_SC_PAGESIZE);
    assert(page > 0 && size < page);
    char *memory = mmap(NULL, page * 2, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(memory != MAP_FAILED);
    assert(mprotect(memory + page, page, PROT_NONE) == 0);
    mpv_gpu_next_host source = valid;
    source.version = version;
    mpv_gpu_next_host *host = (void *)(memory + page - size);
    memcpy(host, &source, size);
    assert(mprotect(memory, page, PROT_READ) == 0);

    mpv_handle *ctx = create();
    assert(mpv_gpu_next_set_host(ctx, host) == 0);
    assert(mpv_initialize(ctx) == 0);
    assert(mpv_gpu_next_request_redraw(ctx) == 0);
    assert(mpv_gpu_next_set_host(ctx, NULL) == MPV_ERROR_INVALID_PARAMETER);
    mpv_terminate_destroy(ctx);
    assert(munmap(memory, page * 2) == 0);
}

int main(void)
{
    assert(mpv_gpu_next_set_host(NULL, NULL) == MPV_ERROR_INVALID_PARAMETER);
    assert(mpv_gpu_next_request_redraw(NULL) == MPV_ERROR_INVALID_PARAMETER);
    mpv_handle *ctx = create();
    assert(mpv_gpu_next_request_redraw(ctx) == MPV_ERROR_UNINITIALIZED);
    mpv_gpu_next_host host = valid;
    assert(mpv_gpu_next_set_host(ctx, &host) == 0);

    host.acquire_frame = acquire_frame;
    assert(mpv_gpu_next_set_host(ctx, &host) == MPV_ERROR_INVALID_PARAMETER);
    host.acquire_frame = NULL;
    host.release_frame = release_frame;
    assert(mpv_gpu_next_set_host(ctx, &host) == MPV_ERROR_INVALID_PARAMETER);
    host.acquire_frame = acquire_frame;
    host.acquire = NULL;
    host.release = NULL;
    assert(mpv_gpu_next_set_host(ctx, &host) == 0);
    host.version = MPV_GPU_NEXT_HOST_VERSION + 1;
    assert(mpv_gpu_next_set_host(ctx, &host) == MPV_ERROR_INVALID_PARAMETER);
    host.version = 0;
    assert(mpv_gpu_next_set_host(ctx, &host) == MPV_ERROR_INVALID_PARAMETER);
    host = valid;
    host.num_extensions = -1;
    assert(mpv_gpu_next_set_host(ctx, &host) == MPV_ERROR_INVALID_PARAMETER);
    host.num_extensions = 1;
    assert(mpv_gpu_next_set_host(ctx, &host) == MPV_ERROR_INVALID_PARAMETER);

    assert(mpv_gpu_next_set_host(ctx, NULL) == 0);
    assert(mpv_initialize(ctx) == 0);
    assert(mpv_gpu_next_request_redraw(ctx) == MPV_ERROR_UNINITIALIZED);
    mpv_terminate_destroy(ctx);
    check_legacy_prefix(1, offsetof(mpv_gpu_next_host, target_color));
    check_legacy_prefix(2, offsetof(mpv_gpu_next_host, acquire_frame));
    return 0;
}
