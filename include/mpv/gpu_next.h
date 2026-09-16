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

#define MPV_GPU_NEXT_HOST_VERSION 2

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
 * Optional Linux VAAPI: when built with VAAPI/DRM, mpv uses the physical device's
 * VK_EXT_physical_device_drm identity to open and own its matching render node.
 * Direct VAAPI also requires actual DMA-BUF texture import support in libplacebo;
 * enable VK_KHR_external_memory_fd, VK_EXT_external_memory_dma_buf and
 * VK_EXT_image_drm_format_modifier (including dependencies) on the host device
 * when supported. Individual decoded formats/modifiers must still be importable.
 * Missing support leaves normal hwdec fallback policy in effect. No host fd is
 * borrowed and the descriptor ABI is unchanged.
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
 * Render-target color space. Version 1 descriptors, and version 2 descriptors
 * with target_color == NULL, use a fixed native SDR target: BT.709, gamma 2.2,
 * full range, 203 nit white, 0.203 nit black, premultiplied alpha, 10-bit RGB.
 * Output-target color overrides cannot change that contract. Otherwise the VO
 * thread calls target_color whenever the swapchain target description is
 * queried; the host fills *out, and fields left 0/unknown fall back to the
 * fixed SDR constants, so a partial description is valid. A changed answer
 * takes effect on the next rendered frame, without VO or device recreation;
 * while paused, schedule that frame with mpv_gpu_next_request_redraw. The
 * host synchronizes its own answer state against the VO thread.
 *
 * The host guarantees its swapchain/surface actually signals the described
 * target. PQ means full-range BT.2020 in the same 10-bit A2B10G10R10 target.
 * HDR10 static metadata (MaxCLL/mastering display) is NOT passed in this ABI
 * version. The host must preserve code values exactly as rendered: an sRGB
 * attachment needs inverse-sRGB before its automatic encode, not another
 * gamma 2.2 encode. Existing scaling, tone mapping and OSD paths are used;
 * color processing remains in gpu-next/libplacebo. target_color follows the
 * acquire/release callback rules: it runs on the VO thread, must return
 * promptly, and must never call mpv synchronously.
 */
typedef struct mpv_gpu_next_target {
    VkImage image;
    int width;
    int height;
    VkImageLayout layout;
    VkImageUsageFlags usage;
    uint64_t token;
} mpv_gpu_next_target;

/* Runtime-described render-target color space (host ABI version 2). The int
 * fields MUST numerically match libplacebo's enum pl_color_primaries and
 * enum pl_color_transfer values (e.g. BT.709 = 3, BT.2020 = 6, sRGB = 2,
 * gamma 2.2 = 6, PQ = 12, HLG = 13); this header deliberately does not
 * include libplacebo headers. A zero field is unknown and falls back to the
 * fixed SDR constant for that field.
 *
 * depth is reserved: the host image is always full-range 10-bit
 * A2B10G10R10, so hosts MUST pass 0 (unknown) or 10; other values are
 * ignored. ref_luma is honored only when mpv is built against libplacebo
 * API >= 371; older builds keep the 203 nit default. Treat ref_luma as
 * static per target configuration even on API >= 371: changing it alone
 * does not refresh an already-mapped paused frame's cached source luminance.
 */
typedef struct mpv_gpu_next_color {
    int primaries;   // matches enum pl_color_primaries (libplacebo)
    int transfer;    // matches enum pl_color_transfer (libplacebo)
    float ref_luma;  // SDR reference white in nits (e.g. 203); 0 = unknown
    float min_luma;  // target black in nits (e.g. 0.203); 0 = unknown
    float max_luma;  // target peak in nits; 0 = unknown
    int depth;       // reserved: MUST be 0 (unknown) or 10 (see above)
} mpv_gpu_next_color;

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
    /* Version 2 and later. Optional; NULL keeps the fixed SDR target.
     * Otherwise called on the VO thread whenever the swapchain target
     * description is queried; see the color contract above. Never read for
     * version 1 descriptors, which are smaller than this struct.
     */
    void (*target_color)(void *opaque, mpv_gpu_next_color *out);
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
