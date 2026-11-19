#include "graphics.h"
#include "game.h"
#include "lighting.h"
#include <openssl/sha.h>
#include <string.h>

static Turok2AcclaimLight lights[TUROK2_ACCLAIM_LIGHTS];
static uint32_t verified_code, verified_data;

static void reset(RecompMicrocodeContext *context, bool from_task, void *userdata) {
    (void)from_task;
    (void)userdata;
    uint32_t data = recomp_microcode_data_address(context);
    for (unsigned i = 0; i < TUROK2_ACCLAIM_LIGHTS; i++) {
        uint8_t bytes[16];
        recomp_microcode_read(context, data + 0x250 + i * 16, false, bytes, sizeof(bytes));
        turok2_acclaim_decode(&lights[i], bytes);
    }
}

static void move_mem(RecompMicrocodeContext *context, uint32_t w0, uint32_t w1, void *userdata) {
    (void)userdata;
    Turok2AcclaimMove move = turok2_acclaim_move(w0);
    if (move.kind == TUROK2_ACCLAIM_COMPACT) {
        uint8_t bytes[16];
        recomp_microcode_read(context, w1, true, bytes, sizeof(bytes));
        turok2_acclaim_decode(&lights[move.index], bytes);
    } else if (move.kind != TUROK2_ACCLAIM_INVALID) {
        /* This ROM's MOVEMEM continuation returns directly to dispatch:
         * even a forced matrix occupies one command, without a skipped word. */
        recomp_microcode_default(context);
    } else {
        recomp_fail("unsupported Turok 2 Acclaim MOVEMEM layout", w0);
    }
}

static void vertex(RecompMicrocodeContext *context, uint32_t w0, uint32_t w1, void *userdata) {
    (void)userdata;
    recomp_microcode_default(context);
    uint32_t mode = recomp_microcode_geometry_mode(context);
    if (!(mode & TUROK2_ACCLAIM_MODE))
        return;
    unsigned count = (w0 >> 12) & 255, end = (w0 >> 1) & 127;
    recomp_game_require(count <= end, "invalid Turok 2 Acclaim vertex range", w0);
    unsigned light_count = recomp_microcode_light_count(context);
    Turok2AcclaimDirectional directional[RECOMP_MICROCODE_MAX_LIGHTS];
    for (unsigned i = 0; i < light_count; i++)
        recomp_microcode_light(context, i, directional[i].direction, directional[i].color);
    float ambient[3], unused[3];
    recomp_microcode_light(context, light_count, unused, ambient);
    bool lighting = (mode & 0x20000) != 0;
    for (unsigned i = end - count; i < end; i++) {
        RecompMicrocodeVertex v;
        recomp_microcode_vertex(context, i, &v);
        float color[3];
        recomp_game_require(turok2_acclaim_color(lights, v.clip_position, v.normal, v.world_matrix, directional,
                                                 light_count, lighting, lighting ? ambient : v.color, color),
                            "non-finite Turok 2 Acclaim vertex lighting input", w1);
        recomp_microcode_vertex_color(context, i, color);
    }
}

static bool bind_microcode(void *interpreter, size_t ram_size, uint32_t code, uint32_t data) {
    (void)ram_size;
    static const RecompMicrocodeBinding bindings[] = {{0xdc, move_mem}, {0x01, vertex}};
    static const RecompMicrocodeConfig config = {.base = RECOMP_GBI_F3DEX2,
                                                 .no_near_clip = true,
                                                 .bindings = bindings,
                                                 .binding_count = sizeof(bindings) / sizeof(bindings[0]),
                                                 .reset = reset};
    recomp_game_require(code == verified_code && data == verified_data,
                        "Turok 2 graphics microcode was not verified before binding", code);
    recomp_renderer_bind_microcode(interpreter, code, data, &config);
    return true;
}

static void fingerprint(Runtime *rt, uint32_t address, size_t size, const char *expected) {
    uint8_t bytes[0x1390], digest[SHA256_DIGEST_LENGTH];
    recomp_game_require(size <= sizeof(bytes), "Turok 2 microcode fingerprint span too large", address);
    for (size_t i = 0; i < size; i++)
        bytes[i] = (uint8_t)recomp_load(rt, address + (uint32_t)i, 1);
    SHA256(bytes, size, digest);
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < sizeof(digest); i++)
        recomp_game_require(hex[digest[i] >> 4] == expected[i * 2] && hex[digest[i] & 15] == expected[i * 2 + 1],
                            "unsupported Turok 2 graphics microcode image", address);
}

void turok2_graphics_verify(Runtime *rt, const RecompTask *task) {
    /* The original RSP program's two overlay images extend beyond the OSTask's
     * nominal 0x1000-byte text descriptor. Verify the complete 0x1390 program. */
    fingerprint(rt, task->words[4], 0x1390, "6734f26b67b25c6a521dc2ee56d343d33c8005ea4001c2db83fbabf539e62e57");
    fingerprint(rt, task->words[6], 0x420, "462006091cd750bfb2f191d8da8f6221d64fd4257852cacab4aacd94e27b3ff1");
    verified_code = recomp_ram_address(rt, task->words[4], 0x1390);
    verified_data = recomp_ram_address(rt, task->words[6], 0x420);
}

void turok2_graphics_init(RecompRenderer *renderer) {
    verified_code = verified_data = UINT32_MAX;
    memset(lights, 0, sizeof(lights));
    recomp_renderer_set_microcode_hook(renderer, bind_microcode);
}
