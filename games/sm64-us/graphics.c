#include "graphics.h"
#include "game_config.h"
#include <stdlib.h>
#include <string.h>

#ifdef RECOMP_WITH_GLES2
#define HEAD_VERTEX_SLOTS 8192
typedef struct HeadVertex {
    uint32_t address, identity;
    int16_t position[3];
} HeadVertex;
static struct {
    HeadVertex vertices[HEAD_VERTEX_SLOTS];
    bool enabled, pending;
} head;

static HeadVertex *head_vertex(uint32_t physical) {
    unsigned slot = ((physical >> 3) * UINT32_C(2654435761)) & (HEAD_VERTEX_SLOTS - 1);
    for (unsigned i = 0; i < HEAD_VERTEX_SLOTS; i++) {
        HeadVertex *v = &head.vertices[(slot + i) & (HEAD_VERTEX_SLOTS - 1)];
        if (!v->address || v->address == physical + 1)
            return v;
    }
    return NULL;
}
static bool head_begin(void *unused) {
    (void)unused;
    bool pending = head.pending;
    head.pending = false;
    return pending;
}
static uint64_t head_identify(void *unused, uint32_t physical, const float position[3]) {
    (void)unused;
    HeadVertex *v = head_vertex(physical);
    if (!v || !v->address)
        return 0;
    for (unsigned i = 0; i < 3; i++)
        if (position[i] != v->position[i])
            return 0;
    return v->identity;
}
static void head_capture(Runtime *rt, uint32_t group) {
    /* Offsets and traversal match the two fingerprinted US converters. Keep
     * their original position/normal stores; annotate only their Vtx links. */
    recomp_ram_address(rt, group, 0x20);
    if (!head.pending) {
        memset(head.vertices, 0, sizeof(head.vertices));
        head.pending = true;
    }
    unsigned budget = 32768;
    for (uint32_t node = recomp_load32(rt, group + 0x1c); node; node = recomp_load32(rt, node + 4)) {
        if (!budget--)
            recomp_fail("SM64 head vertex list exhausted", node);
        recomp_ram_address(rt, node, 12);
        uint32_t vertex = recomp_load32(rt, node + 8);
        recomp_ram_address(rt, vertex, 0x48);
        for (uint32_t link = recomp_load32(rt, vertex + 0x44); link; link = recomp_load32(rt, link)) {
            if (!budget--)
                recomp_fail("SM64 head packed vertex list exhausted", link);
            recomp_ram_address(rt, link, 12);
            uint32_t packed = recomp_load32(rt, link + 8);
            uint32_t physical = recomp_ram_address(rt, packed, 16);
            HeadVertex *entry = head_vertex(physical);
            if (!entry)
                recomp_fail("SM64 head identity table exhausted", packed);
            entry->address = physical + 1;
            entry->identity = vertex;
            for (unsigned i = 0; i < 3; i++)
                entry->position[i] = (int16_t)recomp_load(rt, packed + i * 2, 2);
        }
    }
}
#endif

void sm64_head_vertices_hook(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    uint32_t group = (uint32_t)ctx->gpr[4];
    original(rt, ctx);
#ifdef RECOMP_WITH_GLES2
    if (head.enabled)
        head_capture(rt, group);
#else
    (void)group;
#endif
}

void sm64_graphics_bind(RecompRenderer *renderer) {
#ifdef RECOMP_WITH_GLES2
    recomp_renderer_graphics_identity(renderer, SM64_GFX_CODE_SHA256, SM64_GFX_CODE_SIZE, SM64_GFX_DATA_SHA256,
                                      SM64_GFX_DATA_SIZE);
    memset(&head, 0, sizeof(head));
    const char *fps = getenv("RECOMP_GLES2_FPS");
    head.enabled = fps && !strcmp(fps, "60");
    if (head.enabled) {
        const RecompVertexIdentity identity = {.begin = head_begin, .identify = head_identify};
        recomp_renderer_set_vertex_identity(renderer, &identity);
    }
#else
    (void)renderer;
#endif
}
