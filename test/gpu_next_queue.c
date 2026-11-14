/* Exercise the production VO entry points with a CPU-only libplacebo queue.
 * Including the renderer lets us inspect queued image identities without a
 * test API in the player. Rename its public definitions because the remaining
 * player objects also contain the normal driver and option registration.
 */
#define video_out_gpu_next queue_test_driver
#define gl_next_conf queue_test_conf
#define lut_types queue_test_lut_types
#include "video/out/vo_gpu_next.c"
#undef lut_types
#undef gl_next_conf
#undef video_out_gpu_next

#include <libplacebo/dummy.h>
#include "common/global.h"
#include "common/msg_control.h"
#include "options/m_config_frontend.h"

extern const struct m_sub_options gl_next_conf;

static bool no_target(struct ra_swapchain *sw, struct ra_fbo *fbo)
{
    return false;
}

static void draw(struct vo *vo, struct mp_image **images, uint64_t id, bool redraw)
{
    struct vo_frame frame = {
        .current = images[0],
        .frames = {images[0], images[1]},
        .num_frames = 2,
        .frame_id = id,
        .redraw = redraw,
        .still = redraw,
        .display_synced = !redraw,
        .approx_duration = 1.0 / 24,
        .ideal_frame_vsync_duration = 1.0 / 60,
    };
    mp_require(!queue_test_driver.draw_frame(vo, &frame));
}

static void check_transition(struct vo *vo, double old_pts, double next_pts,
                             int redraws, bool reset)
{
    struct priv *p = vo->priv;
    struct mp_image *old[2], *next[2];
    for (int i = 0; i < 2; i++) {
        old[i] = mp_image_alloc(IMGFMT_420P, 16, 16);
        next[i] = mp_image_alloc(IMGFMT_420P, 16, 16);
        mp_require(old[i] && next[i]);
        old[i]->pts = old_pts + i / 24.0;
        next[i]->pts = next_pts + i / 24.0;
    }

    mp_require(queue_test_driver.control(vo, VOCTRL_RESET, NULL) == VO_TRUE);
    uint64_t id = p->last_id + VO_MAX_REQ_FRAMES + 1;
    draw(vo, old, id, false);
    if (reset)
        mp_require(queue_test_driver.control(vo, VOCTRL_RESET, NULL) == VO_TRUE);
    // vo.c retains current_frame across reset and can redraw it while waiting
    // for the new file/seek result. The first new frame has a distinct ID even
    // when both files (or a repeated seek) start at the same timestamp.
    struct vo_frame empty = {0};
    mp_require(!queue_test_driver.draw_frame(vo, &empty));
    for (int i = 0; i < redraws; i++)
        draw(vo, old, id, true);
    draw(vo, next, id + VO_MAX_REQ_FRAMES + 1, false);

    int count = pl_queue_num_frames(p->queue);
    mp_require(count > 0);
    for (int i = 0; i < count; i++) {
        struct pl_source_frame source;
        mp_require(pl_queue_peek(p->queue, i, &source));
        struct mp_image *image = source.frame_data;
        if (image->planes[0] != next[0]->planes[0] &&
            image->planes[0] != next[1]->planes[0]) {
            fprintf(stderr, "pre-reset image remains in queue (PTS %g -> %g, "
                    "%d redraws, entry %d/%d)\n",
                    old_pts, next_pts, redraws, i, count);
            abort();
        }
    }
    for (int i = 0; i < 2; i++) {
        talloc_free(old[i]);
        talloc_free(next[i]);
    }
}

int main(void)
{
    struct mpv_global *global = talloc_zero(NULL, struct mpv_global);
    mp_msg_init(global);
    struct m_config *config = m_config_new(global, global->log, &mp_opt_root);
    global->config = config->shadow;
    struct priv *p = talloc_zero(NULL, struct priv);
    struct ra_swapchain sw = {
        .fns = &(const struct ra_swapchain_fns){.start_frame = no_target},
    };
    struct ra_ctx ra = {.swapchain = &sw};
    struct gpu_ctx context = {.host = true, .ra_ctx = &ra};
    struct vo vo = {.priv = p, .log = global->log, .global = global};
    p->context = &context;
    p->ra_ctx = &ra;
    p->opts_cache = m_config_cache_alloc(p, global, &gl_video_conf);
    p->next_opts_cache = m_config_cache_alloc(p, global, &gl_next_conf);
    p->next_opts = p->next_opts_cache->opts;
    p->next_opts->target_hint = 0;
    p->video_eq = mp_csp_equalizer_create(p, global);
    p->pars = pl_options_alloc(NULL);
    ((struct gl_video_opts *)p->opts_cache->opts)->interpolation = true;
    p->pars->params.frame_mixer = &pl_filter_oversample;
    p->gpu = pl_gpu_dummy_create(NULL, NULL);
    p->rr = pl_renderer_create(NULL, p->gpu);
    p->queue = pl_queue_create(p->gpu);

    check_transition(&vo, 0, 0, 1, true); // overlapping file timestamps
    check_transition(&vo, 0, 0, 3, true); // repeated paused redraws
    check_transition(&vo, 10, 0, 1, true); // backward seek
    check_transition(&vo, 0, 10, 1, true); // forward seek
    check_transition(&vo, 0, 0, 0, true); // no intervening redraw
    check_transition(&vo, 10, 0, 0, false); // PTS discontinuity without seek

    pl_queue_destroy(&p->queue);
    pl_renderer_destroy(&p->rr);
    pl_options_free(&p->pars);
    pl_gpu_dummy_destroy(&p->gpu);
    talloc_free(p);
    mp_msg_uninit(global);
    talloc_free(global);
    return 0;
}
