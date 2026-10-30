#include "recomp.h"

/* amDmaCallback (70002424), pinned in profile.toml. The 64 nodes and their
 * 0x200-byte sample buffers are allocated by amCreateAudioManager. */
enum {
    DMA_STATE = 0x8005e7b0u,
    DMA_NODES = 0x8005e7c0u,
    AUDIO_FRAME = 0x800230f4u,
    DMA_COUNT = 64,
    DMA_NODE_BYTES = 20,
    DMA_BUFFER_BYTES = 0x200
};

static void check_node(uint32_t node) {
    if (node < DMA_NODES || node >= DMA_NODES + DMA_COUNT * DMA_NODE_BYTES ||
        (node - DMA_NODES) % DMA_NODE_BYTES)
        recomp_fail("GoldenEye invalid audio DMA cache node", node);
}

void goldeneye_audio_dma_hook(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    if (!recomp_load32(rt, DMA_STATE + 8)) {
        uint32_t frame = recomp_load32(rt, AUDIO_FRAME);
        uint32_t request = (uint32_t)ctx->gpr[4], bytes = (uint32_t)ctx->gpr[5];
        uint32_t node = recomp_load32(rt, DMA_STATE + 4), previous = 0;
        uint32_t oldest = 0, oldest_previous = 0, oldest_age = 0;
        bool hit = false;
        unsigned count = 0;
        while (node) {
            check_node(node);
            if (++count > DMA_COUNT || recomp_load32(rt, node + 4) != previous)
                recomp_fail("GoldenEye corrupt audio DMA cache links", node);
            uint32_t start = recomp_load32(rt, node + 8);
            if (request >= start && (uint64_t)request + bytes <= (uint64_t)start + DMA_BUFFER_BYTES) {
                hit = true;
                break;
            }
            uint32_t age = frame - recomp_load32(rt, node + 12);
            if (age >= 2 && age < UINT32_C(0x80000000) && age > oldest_age) {
                oldest = node;
                oldest_previous = previous;
                oldest_age = age;
            }
            previous = node;
            node = recomp_load32(rt, node);
        }
        if (!hit && oldest) {
            /* The normal sweep runs BEFORE incrementing g_AudioFrameCount,
             * retaining two older generations. On a miss with no free nodes,
             * the original callback returns unrelated samples. Reclaim only
             * an older generation; current and previous frame buffers remain
             * protected. amMain waits for task completion each iteration. */
            uint32_t next = recomp_load32(rt, oldest);
            if (next) {
                check_node(next);
                recomp_store32(rt, next + 4, oldest_previous);
            }
            if (oldest_previous)
                recomp_store32(rt, oldest_previous, next);
            else
                recomp_store32(rt, DMA_STATE + 4, next);
            recomp_store32(rt, oldest, 0);
            recomp_store32(rt, oldest + 4, 0);
            recomp_store32(rt, DMA_STATE + 8, oldest);
        }
    }
    /* Preserve the original hit lookup, DMA submission and address result. */
    original(rt, ctx);
}
