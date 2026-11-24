#include "internal.h"
#include "timing.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

enum { PAGE_BYTES = 4096 };
typedef struct ColorWrite {
    uint32_t address;
    size_t bytes;
} ColorWrite;

typedef struct RenderQueue {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t wake;
    bool threaded, submitted, ready, stop, timing;
    bool in_flight; /* Owner only, from submission through collection. */
    const uint32_t *words;
    size_t count;
    uint64_t elapsed, cpu;
    uint8_t *ram, *pages, *view;
    ColorWrite *writes;
    size_t write_count, capacity;
    char failure[256];
    uint32_t failure_address;
} RenderQueue;

static void set_context_current(RecompRDPGles2 *s, bool attach) {
#ifdef __EMSCRIPTEN__
    if (emscripten_webgl_make_context_current(attach ? s->context : 0) != EMSCRIPTEN_RESULT_SUCCESS)
#else
    if (SDL_GL_MakeCurrent(s->window, attach ? s->context : NULL))
#endif
        recomp_fail("GLES render context handoff failed", 0);
    if (attach)
        s->owner = SDL_ThreadID();
}

/* Word-swizzled RAM preserves page boundaries. A source outside installed RAM
 * is handled by the TMEM invalid-byte path. A partly valid tile can still read
 * its in-range texels. Capture each touched page at most once per submission. */
static void capture_pages(RecompRDPGles2 *s, uint64_t address, size_t bytes) {
    RenderQueue *q = s->queue;
    size_t size = s->rt->rdram_size;
    if (address >= size || !bytes)
        return;
    if (bytes > size - address)
        bytes = size - address;
    size_t last = (address + bytes - 1) / PAGE_BYTES;
    for (size_t page = address / PAGE_BYTES; page <= last; page++) {
        if (q->pages[page])
            continue;
        size_t offset = page * PAGE_BYTES;
        size_t span = size - offset;
        memcpy(q->ram + offset, s->rt->rdram + offset, span < PAGE_BYTES ? span : PAGE_BYTES);
        q->pages[page] = 1;
    }
}

static void capture_inputs(RecompRDPGles2 *s, const uint32_t *words, size_t count) {
    memset(s->queue->pages, 0, (s->rt->rdram_size + PAGE_BYTES - 1) / PAGE_BYTES);
    unsigned width = s->image_width, siz = s->image_siz, tiles[8];
    uint32_t image = s->image_address & 0x03ffffff;
    for (unsigned i = 0; i < 8; i++)
        tiles[i] = s->tiles[i].siz;
    for (size_t i = 0; i < count;) {
        uint32_t w0 = words[i] | 0xc0000000u;
        i += recomp_gles2_command_words(w0, count - i);
        /* Only two-word state and DMA commands carry memory dependencies. */
        unsigned op = w0 >> 24;
        if (op != 0xfd && op != 0xf5 && op != 0xff && op != 0xf0 && op != 0xf3 && op != 0xf4)
            continue;
        uint32_t w1 = words[i - 1];
        unsigned tile = (w1 >> 24) & 7;
        if (op == 0xfd) {
            image = w1 & 0x03ffffff;
            width = (w0 & 4095) + 1;
            siz = (w0 >> 19) & 3;
        } else if (op == 0xf5)
            tiles[tile] = (w0 >> 19) & 3;
        else if (op == 0xff) {
            if (((w0 >> 19) & 3) == 1) {
                unsigned w = (w0 & 4095) + 1;
                for (size_t k = 0; k < s->framebuffer_size_count; k++)
                    if (s->framebuffer_sizes[k].width == w)
                        capture_pages(s, w1 & 0x03ffffff, (size_t)w * s->framebuffer_sizes[k].height);
            }
        } else {
            TextureTransfer dma = recomp_gles2_texture_transfer(width, siz, tiles[tile], w0, w1);
#ifdef RECOMP_GLES2_TEXTURE_PACKS
            /* GLideN64 hashes TLUT data starting at SETTIMG, independently
             * of the S/T offset used by the actual palette DMA below. */
            if (s->rice_active && op == 0xf0) {
                unsigned colors = dma.width * dma.rows;
                capture_pages(s, image, (colors < 256 ? colors : 256) * 2);
            }
#endif
            for (unsigned row = 0; row < dma.rows; row++) {
                uint64_t bit = dma.first_bit + row * dma.stride_bits;
                capture_pages(s, image + bit / 8, ((bit & 7) + dma.row_bits + 7) / 8);
            }
        }
    }
}

/* Color images may start or end within a native word. Publish only the lanes
 * actually written, including intermediate images sampled later in the list. */
static void publish_color(Runtime *rt, const uint8_t *src, ColorWrite w) {
    uint32_t a = w.address, end = a + (uint32_t)w.bytes;
#if RECOMP_LITTLE_ENDIAN_HOST
    while (a < end && (a & 3)) {
        rt->rdram[a ^ 3] = src[a ^ 3];
        a++;
    }
    while (a < end && (end & 3)) {
        end--;
        rt->rdram[end ^ 3] = src[end ^ 3];
    }
#endif
    memcpy(rt->rdram + a, src + a, end - a);
}

void recomp_gles2_record_color(RecompRDPGles2 *s, uint32_t address, size_t bytes) {
    RenderQueue *q = s->queue;
    if (s->draw_ram == s->rt->rdram)
        return;
    if (!q || q->ram != s->draw_ram) {
        publish_color(s->rt, s->draw_ram, (ColorWrite){address, bytes});
        return;
    }
    if (q->write_count == q->capacity) {
        size_t cap = q->capacity ? q->capacity * 2 : 8;
        ColorWrite *writes = realloc(q->writes, cap * sizeof(*writes));
        if (!writes)
            recomp_fail("GLES readback journal allocation failed", address);
        q->writes = writes;
        q->capacity = cap;
    }
    q->writes[q->write_count++] = (ColorWrite){address, bytes};
}

static void draw(RecompRDPGles2 *s, uint8_t *ram) {
    RenderQueue *q = s->queue;
    uint64_t start = recomp_timing_clock();
    uint64_t cpu = q->timing ? recomp_timing_thread_clock() : 0;
    recomp_rdp_gles2_draw_rdp(s, ram, s->rt->rdram_size, q->words, q->count);
    q->cpu = cpu ? recomp_timing_thread_clock() - cpu : 0;
    q->elapsed = recomp_timing_clock() - start;
}

/* Fatal draw errors are reported with the guest parked on its owner thread. */
static void render_failed(const RecompFailureInfo *info, void *opaque) {
    RenderQueue *q = opaque;
    if (info->warning)
        return;
    pthread_mutex_lock(&q->mutex);
    snprintf(q->failure, sizeof(q->failure), "%s", info->message);
    q->failure_address = info->address;
    q->ready = true;
    pthread_cond_signal(&q->wake);
    pthread_mutex_unlock(&q->mutex);
    pthread_exit(NULL);
}

static void *render_main(void *opaque) {
    RecompRDPGles2 *s = opaque;
    RenderQueue *q = s->queue;
    recomp_set_failure_handler(render_failed, q);
    pthread_mutex_lock(&q->mutex);
    for (;;) {
        while (!q->submitted && !q->stop)
            pthread_cond_wait(&q->wake, &q->mutex);
        if (q->stop)
            break;
        q->submitted = false;
        pthread_mutex_unlock(&q->mutex);
        set_context_current(s, true);
        draw(s, q->view);
        set_context_current(s, false);
        pthread_mutex_lock(&q->mutex);
        q->ready = true;
        pthread_cond_signal(&q->wake);
    }
    pthread_mutex_unlock(&q->mutex);
    return NULL;
}

static RenderQueue *create_queue(RecompRDPGles2 *s) {
    RenderQueue *q = calloc(1, sizeof(*q));
    if (!q)
        recomp_fail("GLES render queue allocation failed", 0);
    s->queue = q;
    const char *mode = getenv("RECOMP_GLES2_RENDER_THREAD");
    if (mode && !strcmp(mode, "0"))
        return q;
    if (mode && *mode && strcmp(mode, "1"))
        recomp_fail("RECOMP_GLES2_RENDER_THREAD must be 0 or 1", 0);
    if (pthread_mutex_init(&q->mutex, NULL) || pthread_cond_init(&q->wake, NULL) ||
        pthread_create(&q->thread, NULL, render_main, s))
        recomp_fail("GLES render thread initialization failed", 0);
    q->threaded = true;
    return q;
}

void recomp_rdp_gles2_submit(RecompRDPGles2 *s, const uint32_t *words, size_t count, bool guest_parked) {
    if (s->queue && s->queue->in_flight)
        recomp_fail("GLES render submission before collection", 0);
    recomp_gles2_require_owner(s);
    if ((!words && count) || count > 4 * 1024 * 1024)
        recomp_fail("invalid GLES render submission", 0);
    if (s->torch_mode) {
        s->task_lights.count = 0;
        s->task_lights.capsule_count = 0;
        s->task_lights.player_min_depth = 0;
        ++s->light_serial;
        if (s->torch_source && !guest_parked)
            s->torch_source(s->rt, &s->task_lights);
    }
    RenderQueue *q = s->queue ? s->queue : create_queue(s);
    q->in_flight = true;
    q->words = words;
    q->count = count;
    q->timing = s->rt->timing != NULL;
    if (!q->threaded) {
        draw(s, s->rt->rdram);
        return;
    }
    q->view = s->rt->rdram;
    if (!guest_parked) {
        if (!q->ram) {
            q->ram = malloc(s->rt->rdram_size);
            q->pages = malloc((s->rt->rdram_size + PAGE_BYTES - 1) / PAGE_BYTES);
            if (!q->ram || !q->pages)
                recomp_fail("GLES render memory allocation failed", 0);
        }
        capture_inputs(s, words, count);
        q->view = q->ram;
    }
    set_context_current(s, false);
    pthread_mutex_lock(&q->mutex);
    q->ready = false;
    q->submitted = true;
    pthread_cond_signal(&q->wake);
    pthread_mutex_unlock(&q->mutex);
}

bool recomp_rdp_gles2_ready(RecompRDPGles2 *s, bool wait) {
    RenderQueue *q = s->queue;
    if (!q || !q->in_flight)
        return false;
    if (!q->threaded)
        return true;
    pthread_mutex_lock(&q->mutex);
    while (wait && !q->ready)
        pthread_cond_wait(&q->wake, &q->mutex);
    bool ready = q->ready;
    pthread_mutex_unlock(&q->mutex);
    return ready;
}

void recomp_rdp_gles2_collect(RecompRDPGles2 *s, uint64_t *elapsed_ns, uint64_t *cpu_ns) {
    RenderQueue *q = s->queue;
    if (!recomp_rdp_gles2_ready(s, true))
        recomp_fail("GLES render collection without submission", 0);
    if (q->failure[0])
        recomp_fail(q->failure, q->failure_address);
    if (q->threaded) {
        set_context_current(s, true);
        for (size_t i = 0; i < q->write_count; i++)
            publish_color(s->rt, q->ram, q->writes[i]);
        q->write_count = 0;
    }
    q->in_flight = false;
    *elapsed_ns = q->elapsed;
    *cpu_ns = q->cpu;
}

void recomp_gles2_queue_destroy(RecompRDPGles2 *s) {
    RenderQueue *q = s->queue;
    if (!q)
        return;
    if (q->in_flight) {
        uint64_t elapsed, cpu;
        recomp_rdp_gles2_collect(s, &elapsed, &cpu);
    }
    if (q->threaded) {
        pthread_mutex_lock(&q->mutex);
        q->stop = true;
        pthread_cond_signal(&q->wake);
        pthread_mutex_unlock(&q->mutex);
        pthread_join(q->thread, NULL);
        pthread_cond_destroy(&q->wake);
        pthread_mutex_destroy(&q->mutex);
    }
    free(q->ram);
    free(q->pages);
    free(q->writes);
    free(q);
    s->queue = NULL;
}
