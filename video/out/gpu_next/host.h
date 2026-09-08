/*
 * This file is part of mpv.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "context.h"

struct mpv_gpu_next_host;
struct gpu_ctx *gpu_host_create(struct vo *vo, struct ra_ctx_opts *opts,
                                const struct mpv_gpu_next_host *host);
void gpu_host_destroy(struct gpu_ctx *ctx);
bool gpu_host_start_frame(struct gpu_ctx *ctx, struct pl_swapchain_frame *frame);
bool gpu_host_submit_frame(struct gpu_ctx *ctx);
