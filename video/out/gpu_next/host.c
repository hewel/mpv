/*
 * This file is part of mpv.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <libplacebo/vulkan.h>

#include "common/common.h"
#include "mpv/gpu_next.h"
#include "video/out/gpu/context.h"
#include "video/out/placebo/ra_pl.h"
#include "video/out/placebo/utils.h"
#include "context.h"
#include "host.h"

#if HAVE_VAAPI_DRM && HAVE_DRM && defined(__linux__)
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <xf86drm.h>
#include "mpv/render_gl.h"
#define HOST_VAAPI_DRM 1
#else
#define HOST_VAAPI_DRM 0
#endif

struct host_priv {
    const mpv_gpu_next_host *host;
    pl_vulkan vk;
    pl_tex tex;
    VkSemaphore done;
    uint64_t serial;
    mpv_gpu_next_target target;
    bool acquired;
    bool failed;
#if HOST_VAAPI_DRM
    mpv_opengl_drm_params_v2 drm_params;
#endif
};

#if HOST_VAAPI_DRM
static void host_init_drm(struct gpu_ctx *ctx)
{
    struct host_priv *p = ctx->priv;
    const mpv_gpu_next_host *host = p->host;
    PFN_vkGetInstanceProcAddr get_proc = host->get_proc_addr
        ? host->get_proc_addr : vkGetInstanceProcAddr;
    PFN_vkEnumerateDeviceExtensionProperties enumerate = (void *)
        get_proc(host->instance, "vkEnumerateDeviceExtensionProperties");
    PFN_vkGetPhysicalDeviceProperties2 get_props = (void *)
        get_proc(host->instance, "vkGetPhysicalDeviceProperties2");
    uint32_t count = 0;
    if (!enumerate || !get_props ||
        enumerate(host->physical_device, NULL, &count, NULL) != VK_SUCCESS)
        return;

    VkExtensionProperties *exts = talloc_array(NULL, VkExtensionProperties, count);
    bool supported = false;
    if (enumerate(host->physical_device, NULL, &count, exts) == VK_SUCCESS) {
        for (uint32_t i = 0; i < count; i++) {
            if (strcmp(exts[i].extensionName,
                       VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME) == 0)
                supported = true;
        }
    }
    talloc_free(exts);
    if (!supported) {
        MP_VERBOSE(ctx, "Host VAAPI unavailable: no Vulkan DRM device identity\n");
        return;
    }

    // Query support, not the enabled-device list: this is physical-device data.
    VkPhysicalDeviceDrmPropertiesEXT drm_props = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT,
    };
    VkPhysicalDeviceProperties2 props = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &drm_props,
    };
    get_props(host->physical_device, &props);
    if (!drm_props.hasRender) {
        MP_VERBOSE(ctx, "Host VAAPI unavailable: Vulkan device has no render node\n");
        return;
    }

    dev_t id = makedev(drm_props.renderMajor, drm_props.renderMinor);
    drmDevice *device = NULL;
    int ret = drmGetDeviceFromDevId(id, 0, &device);
    if (ret < 0) {
        MP_WARN(ctx, "Cannot resolve host DRM render node: %s\n", mp_strerror(-ret));
        return;
    }
    if (device->available_nodes & (1 << DRM_NODE_RENDER)) {
        const char *path = device->nodes[DRM_NODE_RENDER];
        int fd = open(path, O_RDWR | O_CLOEXEC);
        struct stat st;
        if (fd < 0) {
            MP_WARN(ctx, "Cannot open host DRM render node %s: %s\n",
                    path, mp_strerror(errno));
        } else if (fstat(fd, &st) < 0 || !S_ISCHR(st.st_mode) || st.st_rdev != id) {
            MP_WARN(ctx, "Host DRM render node identity mismatch: %s\n", path);
            close(fd);
        } else {
            p->drm_params.render_fd = fd;
            ra_add_native_resource(ctx->ra_ctx->ra, "drm_params_v2", &p->drm_params);
            MP_VERBOSE(ctx, "Host VAAPI DRM render node: %s\n", path);
        }
    }
    drmFreeDevice(&device);
}
#endif

// The host ABI documents these libplacebo enum values; pin them so a
// libplacebo renumbering fails the build instead of silently changing it.
_Static_assert(PL_COLOR_PRIM_BT_709 == 3, "host ABI primaries");
_Static_assert(PL_COLOR_PRIM_BT_2020 == 6, "host ABI primaries");
_Static_assert(PL_COLOR_TRC_SRGB == 2, "host ABI transfer");
_Static_assert(PL_COLOR_TRC_GAMMA22 == 6, "host ABI transfer");
_Static_assert(PL_COLOR_TRC_PQ == 12, "host ABI transfer");
_Static_assert(PL_COLOR_TRC_HLG == 13, "host ABI transfer");

// Fixed SDR contract of host ABI version 1, and the per-field fallback for a
// version 2 target_color answer.
static const mpv_gpu_next_color host_sdr = {
    .primaries = PL_COLOR_PRIM_BT_709,
    .transfer = PL_COLOR_TRC_GAMMA22,
    .ref_luma = 203.0f,
    .min_luma = 0.203f,
    .max_luma = 203.0f,
    .depth = 10,
};

// Runs on the VO thread. Version 1 descriptors predate the target_color
// field, so it must never be read for them.
static mpv_gpu_next_color host_target_color(struct host_priv *p)
{
    mpv_gpu_next_color out = host_sdr;
    const mpv_gpu_next_host *host = p->host;
    if (host->version >= 2 && host->target_color) {
        mpv_gpu_next_color dynamic = {0};
        host->target_color(host->opaque, &dynamic);
        if (dynamic.primaries)
            out.primaries = dynamic.primaries;
        if (dynamic.transfer)
            out.transfer = dynamic.transfer;
        if (dynamic.ref_luma)
            out.ref_luma = dynamic.ref_luma;
        if (dynamic.min_luma)
            out.min_luma = dynamic.min_luma;
        if (dynamic.max_luma)
            out.max_luma = dynamic.max_luma;
        // depth is reserved: the host image is always full-range RGB10A2, so
        // it stays 10. Signaling any other depth would make libplacebo's
        // pl_color_repr_normalize rescale the code values in that image.
    }
    return out;
}

static struct pl_color_space host_pl_color(mpv_gpu_next_color color)
{
    return (struct pl_color_space) {
        .primaries = color.primaries,
        .transfer = color.transfer,
        .hdr = {.min_luma = color.min_luma, .max_luma = color.max_luma},
    };
}

static struct pl_color_space host_color(struct ra_swapchain *sw)
{
    return host_pl_color(host_target_color(sw->ctx->priv));
}

static int host_depth(struct ra_swapchain *sw) { return 10; }

static float host_luma(struct ra_swapchain *sw)
{
    return host_target_color(sw->ctx->priv).ref_luma;
}
static bool host_reconfig(struct ra_ctx *ra) { return true; }
static int host_control(struct ra_ctx *ra, int *events, int request, void *arg)
{
    return VO_NOTIMPL;
}
static bool host_start(struct ra_swapchain *sw, struct ra_fbo *fbo) { return true; }
static void host_swap(struct ra_swapchain *sw) {}

static const struct ra_ctx_fns host_fns = {
    .type = "vulkan", .name = "libmpv-host",
    .reconfig = host_reconfig, .control = host_control,
};
static const struct ra_swapchain_fns host_sw_fns = {
    .color_depth = host_depth, .target_csp = host_color,
    .target_ref_luma = host_luma, .start_frame = host_start,
    .swap_buffers = host_swap,
};

// Return ownership even when a frame was interrupted before flip_page.
static bool release_target(struct gpu_ctx *ctx, int status)
{
    struct host_priv *p = ctx->priv;
    if (!p->acquired)
        return true;
    if (p->tex) {
        if (!pl_vulkan_hold_ex(ctx->gpu, pl_vulkan_hold_params(
                .tex = p->tex,
                .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                .qf = VK_QUEUE_FAMILY_IGNORED,
                .semaphore = {p->done, ++p->serial})))
            status = MPV_ERROR_VO_INIT_FAILED;
        pl_gpu_finish(ctx->gpu);
        pl_tex_destroy(ctx->gpu, &p->tex);
    }
    if (pl_gpu_is_failed(ctx->gpu))
        status = MPV_ERROR_VO_INIT_FAILED;
    p->target.layout = status == 0 ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                   : VK_IMAGE_LAYOUT_UNDEFINED;
    p->acquired = false;
    p->host->release(p->host->opaque, &p->target, status);
    if (status < 0)
        p->failed = true;
    return status == 0;
}

bool gpu_host_start_frame(struct gpu_ctx *ctx, struct pl_swapchain_frame *frame)
{
    struct host_priv *p = ctx->priv;
    if (p->failed)
        return false;
    if (p->acquired) {
        release_target(ctx, MPV_ERROR_GENERIC);
        return false;
    }
    p->target = (mpv_gpu_next_target){0};
    int res = p->host->acquire(p->host->opaque, &p->target);
    if (res != 1) {
        if (res < 0) {
            MP_ERR(ctx, "Host target acquisition failed: %d\n", res);
            p->failed = true;
        }
        return false;
    }
    p->acquired = true;
    VkImageUsageFlags required = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!p->target.image || p->target.width <= 0 || p->target.height <= 0 ||
        (p->target.usage & required) != required)
        goto error;
    p->tex = pl_vulkan_wrap(ctx->gpu, pl_vulkan_wrap_params(
        .image = p->target.image,
        .width = p->target.width, .height = p->target.height,
        .format = VK_FORMAT_A2B10G10R10_UNORM_PACK32,
        .usage = p->target.usage));
    if (!p->tex)
        goto error;
    pl_vulkan_release_ex(ctx->gpu, pl_vulkan_release_params(
        .tex = p->tex, .layout = p->target.layout,
        .qf = VK_QUEUE_FAMILY_IGNORED));
    ctx->ra_ctx->vo->dwidth = p->target.width;
    ctx->ra_ctx->vo->dheight = p->target.height;
    // The host image is always RGB10A2; both depths stay 10 (see above).
    mpv_gpu_next_color color = host_target_color(p);
    *frame = (struct pl_swapchain_frame) {
        .fbo = p->tex,
        .color_space = host_pl_color(color),
        .color_repr = {
            .sys = PL_COLOR_SYSTEM_RGB, .levels = PL_COLOR_LEVELS_FULL,
            .alpha = PL_ALPHA_PREMULTIPLIED,
            .bits = {.sample_depth = 10, .color_depth = 10},
        },
    };
    return true;
error:
    MP_ERR(ctx, "Invalid or unsupported host Vulkan target\n");
    release_target(ctx, MPV_ERROR_INVALID_PARAMETER);
    return false;
}

bool gpu_host_submit_frame(struct gpu_ctx *ctx)
{
    return release_target(ctx, ctx->frame_error ? MPV_ERROR_GENERIC : 0);
}

void gpu_host_destroy(struct gpu_ctx *ctx)
{
    struct host_priv *p = ctx->priv;
    if (ctx->gpu) {
        release_target(ctx, MPV_ERROR_GENERIC);
        pl_gpu_finish(ctx->gpu);
        pl_vulkan_sem_destroy(ctx->gpu, &p->done);
    }
    if (ctx->ra_ctx && ctx->ra_ctx->ra)
        ctx->ra_ctx->ra->fns->destroy(ctx->ra_ctx->ra);
#if HOST_VAAPI_DRM
    // The VO destroys its hardware mappers and VA display before this context.
    if (p->drm_params.render_fd >= 0)
        close(p->drm_params.render_fd);
#endif
    pl_vulkan_destroy(&p->vk);
    pl_log_destroy(&ctx->pllog);
    talloc_free(ctx);
}

struct gpu_ctx *gpu_host_create(struct vo *vo, struct ra_ctx_opts *opts,
                                const mpv_gpu_next_host *host)
{
    struct gpu_ctx *ctx = talloc_zero(NULL, struct gpu_ctx);
    ctx->log = vo->log;
    ctx->host = true;
    struct host_priv *p = ctx->priv = talloc_zero(ctx, struct host_priv);
    p->host = host;
#if HOST_VAAPI_DRM
    p->drm_params = (mpv_opengl_drm_params_v2){.fd = -1, .render_fd = -1};
#endif
    ctx->pllog = mppl_log_create(ctx, vo->log);
    p->vk = pl_vulkan_import(ctx->pllog, pl_vulkan_import_params(
        .instance = host->instance, .phys_device = host->physical_device,
        .device = host->device, .get_proc_addr = host->get_proc_addr,
        .features = host->features, .extensions = host->extensions,
        .num_extensions = host->num_extensions,
        .queue_graphics = {host->queue_family, 1},
        .lock_queue = host->lock_queue, .unlock_queue = host->unlock_queue,
        .queue_ctx = host->opaque));
    if (!p->vk)
        goto error;
    ctx->gpu = p->vk->gpu;
    p->done = pl_vulkan_sem_create(ctx->gpu, pl_vulkan_sem_params(
        .type = VK_SEMAPHORE_TYPE_TIMELINE));
    if (!p->done)
        goto error;
    struct ra_ctx *ra = ctx->ra_ctx = talloc_zero(ctx, struct ra_ctx);
    *ra = (struct ra_ctx) {
        .vo = vo, .global = vo->global, .log = vo->log, .opts = *opts,
        .fns = &host_fns, .ra = ra_create_pl(ctx->gpu, vo->log), .priv = p,
    };
    if (!ra->ra)
        goto error;
#if HOST_VAAPI_DRM
    host_init_drm(ctx);
#endif
    ra->swapchain = talloc_zero(ra, struct ra_swapchain);
    *ra->swapchain = (struct ra_swapchain){.ctx = ra, .fns = &host_sw_fns};
    return ctx;
error:
    MP_ERR(ctx, "Failed to initialize host Vulkan renderer\n");
    gpu_host_destroy(ctx);
    return NULL;
}
