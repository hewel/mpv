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

struct host_priv {
    const mpv_gpu_next_host *host;
    pl_vulkan vk;
    pl_tex tex;
    VkSemaphore done;
    uint64_t serial;
    mpv_gpu_next_target target;
    bool acquired;
    bool failed;
};

static struct pl_color_space host_color(struct ra_swapchain *sw)
{
    return (struct pl_color_space) {
        .primaries = PL_COLOR_PRIM_BT_709,
        .transfer = PL_COLOR_TRC_GAMMA22,
        .hdr = {.min_luma = 0.203f, .max_luma = 203.0f},
    };
}

static int host_depth(struct ra_swapchain *sw) { return 10; }
static float host_luma(struct ra_swapchain *sw) { return 203.0f; }
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
    *frame = (struct pl_swapchain_frame) {
        .fbo = p->tex,
        .color_space = host_color(NULL),
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
        .fns = &host_fns, .ra = ra_create_pl(ctx->gpu, vo->log),
    };
    if (!ra->ra)
        goto error;
    ra->swapchain = talloc_zero(ra, struct ra_swapchain);
    *ra->swapchain = (struct ra_swapchain){.ctx = ra, .fns = &host_sw_fns};
    return ctx;
error:
    MP_ERR(ctx, "Failed to initialize host Vulkan renderer\n");
    gpu_host_destroy(ctx);
    return NULL;
}
