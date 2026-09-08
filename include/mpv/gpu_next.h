/*
 * This file is part of mpv.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef MPV_GPU_NEXT_H_
#define MPV_GPU_NEXT_H_

#include <stdint.h>
#include <vulkan/vulkan.h>
#include "client.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MPV_GPU_NEXT_HOST_VERSION 1

/* Experimental Linux/Vulkan gpu-next host API, separate from render.h.
 * The host owns every Vulkan object. Register before mpv_initialize, select
 * vo=gpu-next, and retain this descriptor, all pointed-to data and callbacks
 * until mpv_terminate_destroy returns (all other client handles must be gone).
 * Registration never imports a device; VO initialization reports import errors
 * through the normal log/end-file events. No native-window fallback is used.
 *
 * Device: Vulkan >= 1.2, graphics queue index 0 in queue_family. Enable all
 * libplacebo pl_vulkan_required_features and describe the actual enabled feature
 * chain/extensions. lock_queue/unlock_queue must serialize every host queue
 * operation with mpv's operations. Never call mpv synchronously from callbacks,
 * never hold that queue lock while calling mpv or waiting for a callback.
 *
 * All acquire/release callbacks run on the VO thread. They must return promptly
 * and must not wait for the UI thread. Use a bounded host pool: acquire returns
 * 0 when exhausted/hidden, 1 on success, negative on fatal error. mpv holds at
 * most one acquired target. Fatal errors disable acquisition until VO recreation.
 * A successful acquire receives exactly one release, including on teardown.
 * release(status=0) is the presentation notification, issued ONLY in flip_page
 * after mpv's playback scheduler permits presentation, with GPU writes complete.
 * A negative status means discard, not present (including interrupted teardown).
 * Do not display the image before a successful release. Actual compositor
 * presentation remains the host's responsibility; no display-sync feedback.
 *
 * Acquire must supply an image whose previous GPU accesses have COMPLETED,
 * owned by queue_family, with the stated layout. No GPU or host accesses until
 * release. This prototype blocks for mpv GPU completion, never reads pixels to
 * CPU. On successful release the layout is SHADER_READ_ONLY_OPTIMAL, ownership
 * remains queue_family. Error releases leave layout/content unspecified but
 * all mpv GPU work is complete; destroy or transition from UNDEFINED before reuse.
 * Images are 2D, single mip/layer/sample, optimal tiling, same VkDevice, format
 * A2B10G10R10_UNORM_PACK32, usage COLOR_ATTACHMENT|SAMPLED|TRANSFER_DST (additional
 * flags permitted). Pixel dimensions may change on every acquire: retain old
 * images until released, then until the host's own sampling has completed.
 *
 * Fixed native SDR target: BT.709, gamma 2.2, full range, 203 nit white,
 * 0.203 nit black, premultiplied alpha, 10-bit RGB. Output-target color overrides
 * cannot change this contract. Existing scaling, tone mapping and OSD paths are
 * used. The host must preserve these nonlinear code values: an sRGB attachment
 * needs inverse-sRGB before its automatic encode, not another gamma 2.2 encode.
 */
typedef struct mpv_gpu_next_target {
    VkImage image;
    int width;
    int height;
    VkImageLayout layout;
    VkImageUsageFlags usage;
    uint64_t token;
} mpv_gpu_next_target;

typedef struct mpv_gpu_next_host {
    uint32_t version;
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    PFN_vkGetInstanceProcAddr get_proc_addr;
    uint32_t queue_family;
    const VkPhysicalDeviceFeatures2 *features;
    const char *const *extensions;
    int num_extensions;
    void *opaque;
    void (*lock_queue)(void *opaque, uint32_t family, uint32_t index);
    void (*unlock_queue)(void *opaque, uint32_t family, uint32_t index);
    int (*acquire)(void *opaque, mpv_gpu_next_target *target);
    void (*release)(void *opaque, const mpv_gpu_next_target *target, int status);
} mpv_gpu_next_host;

/* Returns 0, MPV_ERROR_INVALID_PARAMETER, or MPV_ERROR_NOT_IMPLEMENTED when
 * built without Vulkan. NULL unregisters before initialization. After initialize
 * returns MPV_ERROR_INVALID_PARAMETER. Descriptor is borrowed, not copied.
 */
MPV_EXPORT int mpv_gpu_next_set_host(mpv_handle *ctx,
                                   const mpv_gpu_next_host *host);

/* Request a scheduler-driven redraw after a host resize or after acquire
 * returned 0 and capacity becomes available. Call from a host application
 * thread, never from a VO callback or while holding the queue lock.
 * Do not call after every successful copy: this would cause a redraw loop.
 * Returns 0 for an initialized host (also when no video is loaded),
 * MPV_ERROR_UNINITIALIZED without an initialized host, or
 * MPV_ERROR_INVALID_PARAMETER for NULL. Presentation still uses flip_page.
 */
MPV_EXPORT int mpv_gpu_next_request_redraw(mpv_handle *ctx);

#ifdef __cplusplus
}
#endif
#endif
