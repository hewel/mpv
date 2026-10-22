/*
 * This file is part of mpv.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

// Opt-in, real-device regression. A queued wait deliberately holds the GPU
// while the VO hands off a frame, proving that v3 does not CPU-wait for it.
#undef NDEBUG
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <libplacebo/log.h>
#include <libplacebo/vulkan.h>
#include "mpv/gpu_next.h"

#define VK_OK(call) assert((call) == VK_SUCCESS)

enum scenario { NORMAL, BAD_SEMAPHORE, BAD_SIGNAL, BAD_SIZE, INFLIGHT_SHUTDOWN };

struct host {
    pl_vulkan vk;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    mpv_gpu_next_target target;
    VkDeviceMemory memory;
    VkSemaphore timeline, gate;
    uint64_t value;
    bool available, busy, gated;
    enum scenario scenario;
    int exhausted, acquired, released;
    int terminating;
    mpv_handle *ctx;
    int status;
    mpv_gpu_next_frame last;
};

static void lock_queue(void *opaque, uint32_t family, uint32_t index)
{
    struct host *h = opaque;
    h->vk->lock_queue(h->vk, family, index);
}

static void unlock_queue(void *opaque, uint32_t family, uint32_t index)
{
    struct host *h = opaque;
    h->vk->unlock_queue(h->vk, family, index);
}

static void signal_value(struct host *h, VkSemaphore sem, uint64_t value)
{
    VK_OK(vkSignalSemaphore(h->vk->device, &(VkSemaphoreSignalInfo){
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
        .semaphore = sem, .value = value,
    }));
}

static void wait_value(struct host *h, VkSemaphore sem, uint64_t value)
{
    VK_OK(vkWaitSemaphores(h->vk->device, &(VkSemaphoreWaitInfo){
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1, .pSemaphores = &sem, .pValues = &value,
    }, 10000000000ULL));
}

static bool wait_count(struct host *h, const int *counter, int count)
{
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += 10;
    pthread_mutex_lock(&h->mutex);
    int res = 0;
    while (*counter < count && !res)
        res = pthread_cond_timedwait(&h->changed, &h->mutex, &deadline);
    bool reached = *counter >= count;
    pthread_mutex_unlock(&h->mutex);
    assert(res == 0 || res == ETIMEDOUT);
    return reached;
}

static void hold_gpu(struct host *h)
{
    VkQueue queue;
    vkGetDeviceQueue(h->vk->device, h->vk->queue_graphics.index, 0, &queue);
    uint64_t value = 1;
    VkTimelineSemaphoreSubmitInfo timeline = {
        .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .waitSemaphoreValueCount = 1, .pWaitSemaphoreValues = &value,
        .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &h->value,
    };
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &timeline,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &h->gate,
        .pWaitDstStageMask = &stage,
        .signalSemaphoreCount = 1, .pSignalSemaphores = &h->timeline,
    };
    lock_queue(h, h->vk->queue_graphics.index, 0);
    VK_OK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
    unlock_queue(h, h->vk->queue_graphics.index, 0);
}

static int acquire_frame(void *opaque, mpv_gpu_next_frame *frame)
{
    struct host *h = opaque;
    pthread_mutex_lock(&h->mutex);
    if (!h->available || h->busy) {
        h->exhausted++;
        pthread_cond_signal(&h->changed);
        pthread_mutex_unlock(&h->mutex);
        return 0;
    }
    h->busy = true;
    h->acquired++;
    *frame = (mpv_gpu_next_frame){
        .target = h->target, .semaphore = h->timeline,
        .wait_value = h->value, .signal_value = h->value + 1,
    };
    if (h->scenario == BAD_SEMAPHORE)
        frame->semaphore = VK_NULL_HANDLE;
    if (h->scenario == BAD_SIGNAL)
        frame->signal_value = frame->wait_value;
    if (h->scenario == BAD_SIZE)
        frame->target.width = 0;
    bool gate = h->gated && h->acquired == 1;
    pthread_mutex_unlock(&h->mutex);
    if (gate)
        hold_gpu(h);
    return 1;
}

static void release_frame(void *opaque, const mpv_gpu_next_frame *frame, int status)
{
    struct host *h = opaque;
    pthread_mutex_lock(&h->mutex);
    assert(h->busy && h->acquired == h->released + 1);
    h->last = *frame;
    h->status = status;
    h->released++;
    pthread_cond_signal(&h->changed);
    pthread_mutex_unlock(&h->mutex);
}

static int acquire(void *opaque, mpv_gpu_next_target *target)
{
    mpv_gpu_next_frame frame;
    int res = acquire_frame(opaque, &frame);
    if (res == 1)
        *target = frame.target;
    return res;
}

static void release(void *opaque, const mpv_gpu_next_target *target, int status)
{
    release_frame(opaque, &(mpv_gpu_next_frame){.target = *target}, status);
}

static void create_image(struct host *h, int width, int height)
{
    h->target = (mpv_gpu_next_target){
        .width = width, .height = height, .layout = VK_IMAGE_LAYOUT_UNDEFINED,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .token = 42,
    };
    VK_OK(vkCreateImage(h->vk->device, &(VkImageCreateInfo){
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_A2B10G10R10_UNORM_PACK32,
        .extent = {width, height, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = h->target.usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    }, NULL, &h->target.image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(h->vk->device, h->target.image, &req);
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(h->vk->phys_device, &props);
    uint32_t index = 0;
    while (index < props.memoryTypeCount && !(req.memoryTypeBits & (1u << index)))
        index++;
    assert(index < props.memoryTypeCount);
    VK_OK(vkAllocateMemory(h->vk->device, &(VkMemoryAllocateInfo){
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size, .memoryTypeIndex = index,
    }, NULL, &h->memory));
    VK_OK(vkBindImageMemory(h->vk->device, h->target.image, h->memory, 0));
}

static void destroy_image(struct host *h)
{
    vkDestroyImage(h->vk->device, h->target.image, NULL);
    vkFreeMemory(h->vk->device, h->memory, NULL);
}

static void *terminate(void *opaque)
{
    struct host *h = opaque;
    pthread_mutex_lock(&h->mutex);
    h->terminating = 1;
    pthread_cond_signal(&h->changed);
    pthread_mutex_unlock(&h->mutex);
    mpv_terminate_destroy(h->ctx);
    uint64_t value;
    VK_OK(vkGetSemaphoreCounterValue(h->vk->device, h->timeline, &value));
    assert(value >= h->last.signal_value);
    return NULL;
}

static void run_case(pl_vulkan vk, const char *media, int version,
                     enum scenario scenario)
{
    struct host h = {
        .vk = vk, .mutex = PTHREAD_MUTEX_INITIALIZER,
        .changed = PTHREAD_COND_INITIALIZER,
        .gated = version == 3 && (scenario == NORMAL || scenario == INFLIGHT_SHUTDOWN),
        .scenario = scenario,
    };
    // The host's incoming signal is submitted by acquire but cannot complete
    // until the main thread opens gate. The initial producer wait is pending.
    h.value = h.gated ? 2 : 0;
    h.timeline = pl_vulkan_sem_create(vk->gpu,
        pl_vulkan_sem_params(.type = VK_SEMAPHORE_TYPE_TIMELINE));
    h.gate = pl_vulkan_sem_create(vk->gpu,
        pl_vulkan_sem_params(.type = VK_SEMAPHORE_TYPE_TIMELINE));
    assert(h.timeline && h.gate);
    create_image(&h, 320, 180);
    mpv_gpu_next_host host = {
        .version = version, .instance = vk->instance,
        .physical_device = vk->phys_device, .device = vk->device,
        .get_proc_addr = vk->get_proc_addr,
        .queue_family = vk->queue_graphics.index, .features = vk->features,
        .extensions = vk->extensions, .num_extensions = vk->num_extensions,
        .opaque = &h, .lock_queue = lock_queue, .unlock_queue = unlock_queue,
        .acquire = acquire, .release = release,
        .acquire_frame = version == 3 ? acquire_frame : NULL,
        .release_frame = version == 3 ? release_frame : NULL,
    };
    mpv_handle *ctx = h.ctx = mpv_create();
    assert(ctx);
    assert(mpv_set_option_string(ctx, "config", "no") == 0);
    assert(mpv_set_option_string(ctx, "vo", "gpu-next") == 0);
    assert(mpv_set_option_string(ctx, "ao", "null") == 0);
    assert(mpv_set_option_string(ctx, "hwdec", "no") == 0);
    assert(mpv_set_option_string(ctx, "pause", "yes") == 0);
    assert(mpv_set_option_string(ctx, "keep-open", "yes") == 0);
    assert(mpv_set_option_string(ctx, "terminal", "yes") == 0);
    assert(mpv_gpu_next_set_host(ctx, &host) == 0);
    assert(mpv_initialize(ctx) == 0);
    const char *load[] = {"loadfile", media, NULL};
    assert(mpv_command(ctx, load) == 0);
    assert(wait_count(&h, &h.exhausted, 1));
    pthread_mutex_lock(&h.mutex);
    h.available = true;
    pthread_mutex_unlock(&h.mutex);
    assert(mpv_gpu_next_request_redraw(ctx) == 0);
    bool released = wait_count(&h, &h.released, 1);
    pthread_t terminating;
    if (released && scenario == INFLIGHT_SHUTDOWN) {
        assert(pthread_create(&terminating, NULL, terminate, &h) == 0);
        assert(wait_count(&h, &h.terminating, 1));
    }
    if (h.gated) {
        uint64_t value;
        VK_OK(vkGetSemaphoreCounterValue(vk->device, h.timeline, &value));
        // Always open the gate, including on a regression, before teardown.
        signal_value(&h, h.gate, 1);
        assert(value == 0);
    }
    assert(released);
    if (scenario == INFLIGHT_SHUTDOWN) {
        assert(pthread_join(terminating, NULL) == 0);
    } else if (scenario != NORMAL) {
        assert(h.status == MPV_ERROR_INVALID_PARAMETER);
        assert(h.last.target.layout == VK_IMAGE_LAYOUT_UNDEFINED);
        uint64_t value;
        VK_OK(vkGetSemaphoreCounterValue(vk->device, h.timeline, &value));
        assert(value == 0); // A discard must not imply a signal to wait for.
        assert(mpv_gpu_next_request_redraw(ctx) == 0);
    } else {
        assert(h.status == 0 && h.last.target.token == 42);
        assert(h.last.target.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (version == 3) {
            wait_value(&h, h.timeline, h.last.signal_value);
            // No consumer GPU work: acknowledge completion on the host.
            signal_value(&h, h.timeline, h.last.signal_value + 1);
        }
        // Resize only after the producer completed. Keep the slot semaphore
        // and advance its value so replacement images cannot reuse old signals.
        destroy_image(&h);
        create_image(&h, 160, 90);
        pthread_mutex_lock(&h.mutex);
        h.value = version == 3 ? h.last.signal_value + 1 : 2;
        h.busy = false;
        pthread_mutex_unlock(&h.mutex);
        assert(mpv_gpu_next_request_redraw(ctx) == 0);
        assert(wait_count(&h, &h.released, 2));
        assert(h.status == 0 && h.last.target.width == 160);
        if (version == 3)
            wait_value(&h, h.timeline, h.last.signal_value);
    }
    // Shutdown drains outstanding producer operations and retains the callback
    // storage until the final release; no queue/pool lock spans termination.
    if (scenario != INFLIGHT_SHUTDOWN)
        mpv_terminate_destroy(ctx);
    assert(h.acquired == h.released && h.released == (scenario == NORMAL ? 2 : 1));
    destroy_image(&h);
    pl_vulkan_sem_destroy(vk->gpu, &h.timeline);
    pl_vulkan_sem_destroy(vk->gpu, &h.gate);
    pthread_cond_destroy(&h.changed);
    pthread_mutex_destroy(&h.mutex);
    printf("PASS host v%d scenario=%d acquired=%d released=%d\n",
           version, scenario, h.acquired, h.released);
}

int main(int argc, char **argv)
{
    if (!getenv("MPV_TEST_GPU_NEXT")) {
        puts("Set MPV_TEST_GPU_NEXT=1 to run real Vulkan host regressions");
        return 77;
    }
    assert(argc == 2);
    pl_log log = pl_log_create(PL_API_VER, pl_log_params(
        .log_cb = pl_log_color, .log_priv = stderr, .log_level = PL_LOG_WARN));
    pl_vulkan vk = pl_vulkan_create(log, pl_vulkan_params(
        .async_compute = false, .async_transfer = false, .queue_count = 1,
        .allow_software = true));
    assert(vk);
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(vk->phys_device, &props);
    printf("Vulkan device: %s\n", props.deviceName);
    run_case(vk, argv[1], 1, NORMAL);
    run_case(vk, argv[1], 2, NORMAL);
    run_case(vk, argv[1], 3, NORMAL);
    for (enum scenario s = BAD_SEMAPHORE; s <= INFLIGHT_SHUTDOWN; s++)
        run_case(vk, argv[1], 3, s);
    pl_vulkan_destroy(&vk);
    pl_log_destroy(&log);
    return 0;
}
