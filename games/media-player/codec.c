#include "codec.h"
#include "generated.h"
#include "media_player_config.h"
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#endif

/* Raw LZ4 block used by this ROM's ELF loader. All bounds are checked even
 * though recomp_game_rom has already authenticated the entire cartridge. */
static void unpack(Runtime *rt, const uint8_t *src, size_t size) {
    uint8_t *raw = malloc(BOOT_SEGMENT_SIZE);
    recomp_game_require(raw != NULL, "MP3 ELF allocation failed", 0);
    size_t in = 0, out = 0;
    while (in < size) {
        unsigned token = src[in++];
        size_t length = token >> 4;
        if (length == 15) {
            unsigned extra;
            do {
                recomp_game_require(in < size, "MP3 ELF literal length", 0);
                extra = src[in++]; length += extra;
            } while (extra == 255);
        }
        recomp_game_require(length <= size-in && length <= BOOT_SEGMENT_SIZE-out,
                            "MP3 ELF literal range", 0);
        memcpy(raw+out, src+in, length); in += length; out += length;
        if (in == size) break;
        recomp_game_require(size-in >= 2, "MP3 ELF match offset", 0);
        size_t offset = src[in] | ((size_t)src[in+1] << 8); in += 2;
        length = (token & 15) + 4;
        if ((token & 15) == 15) {
            unsigned extra;
            do {
                recomp_game_require(in < size, "MP3 ELF match length", 0);
                extra = src[in++]; length += extra;
            } while (extra == 255);
        }
        recomp_game_require(offset && offset <= out && length <= BOOT_SEGMENT_SIZE-out,
                            "MP3 ELF match range", 0);
        for (size_t i = 0; i < length; i++, out++) raw[out] = raw[out-offset];
    }
    recomp_game_require(out == BOOT_SEGMENT_SIZE, "MP3 ELF decoded size", 0);
    recomp_copy_to_ram(rt, BOOT_SEGMENT_VRAM, raw, out);
    free(raw);
}

static MipsContext context(void) {
    MipsContext ctx = {0};
    ctx.fpu_fr1 = true;
    ctx.gpr[28] = mips_s32(BOOT_GLOBAL_POINTER);
    ctx.gpr[29] = mips_s32(BOOT_STACK);
    ctx.gpr[4] = mips_s32(BOOT_STATE);
    return ctx;
}

static void reset(MediaCodec *c) {
    /* ROM mp3dec_t: 576 overlap floats, 960 QMF floats, 2 ints,
     * header[4], reservoir[511], padded to 6668 bytes. */
    for (unsigned i = 0; i < 6668; i += 4) recomp_store(&c->rt, BOOT_STATE+i, 0, 4);
    MipsContext ctx = context();
    recomp_lookup(&c->rt, BOOT_DECODER_INIT)(&c->rt, &ctx);
    c->available = 0; c->bytes = c->frames = 0;
    c->rate = c->channels = c->bitrate = c->samples = 0;
}

void media_codec_init(MediaCodec *c, const RecompGameOptions *options) {
    memset(c, 0, sizeof(*c));
    recomp_game_require(recomp_runtime_init(&c->rt, BOOT_RAM_SIZE, recomp_functions,
                        recomp_function_count, recomp_boot_sections, recomp_section_count),
                        "MP3 runtime allocation failed", 0);
    uint8_t *rom = recomp_game_rom(&c->rt, options, BOOT_ROM_SIZE, boot_sha1);
    unpack(&c->rt, rom+BOOT_COMPRESSED_OFFSET, BOOT_COMPRESSED_SIZE);
    free(rom);
    reset(c);
}

bool media_codec_open(MediaCodec *c, const char *path) {
    FILE *file;
#ifdef _WIN32
    wchar_t wide[32768];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, 32768)) return false;
    file = _wfopen(wide, L"rb");
#else
    file = fopen(path, "rb");
#endif
    if (!file) return false;
    if (c->file) fclose(c->file);
    c->file = file;
    reset(c);
    return true;
}

bool media_codec_restart(MediaCodec *c) {
    if (!c->file || fseek(c->file, 0, SEEK_SET)) return false;
    reset(c);
    return true;
}

int media_codec_step(MediaCodec *c) {
    if (!c->file) return -1;
    c->available += fread(c->input+c->available, 1, sizeof(c->input)-c->available, c->file);
    if (ferror(c->file)) return -2;
    if (!c->available) return -1;
    recomp_copy_to_ram(&c->rt, BOOT_INPUT, c->input, c->available);
    for (unsigned i = 0; i < 24; i += 4) recomp_store(&c->rt, BOOT_INFO+i, 0, 4);
    MipsContext ctx = context();
    ctx.gpr[5] = mips_s32(BOOT_INPUT);
    ctx.gpr[6] = c->available;
    ctx.gpr[7] = mips_s32(BOOT_PCM);
    /* libdragon O64: four 8-byte argument homes, then the fifth slot.
     * 800388b4 subtracts 0x4408; 800388f0 loads the pointer at sp+0x442c,
     * i.e. the low word at caller sp+36 in this big-endian 64-bit slot. */
    recomp_store(&c->rt, BOOT_STACK+32, mips_s32(BOOT_INFO), 8);
    recomp_lookup(&c->rt, BOOT_ENTRY)(&c->rt, &ctx);
    unsigned consumed = (unsigned)recomp_load(&c->rt, BOOT_INFO, 4);
    c->samples = (int32_t)ctx.gpr[2];
    recomp_game_require(consumed <= c->available && c->samples >= 0 && c->samples <= 1152,
                        "MP3 decoder output bounds", BOOT_ENTRY);
    if (!consumed) consumed = (unsigned)c->available;
    c->available -= consumed; c->bytes += consumed;
    memmove(c->input, c->input+consumed, c->available);
    if (!c->samples) return 0;
    c->channels = (int)recomp_load(&c->rt, BOOT_INFO+8, 4);
    c->rate = (int)recomp_load(&c->rt, BOOT_INFO+12, 4);
    c->bitrate = (int)recomp_load(&c->rt, BOOT_INFO+20, 4);
    recomp_game_require((c->channels == 1 || c->channels == 2) && c->rate >= 8000 && c->rate <= 48000,
                        "MP3 decoder format", BOOT_ENTRY);
    for (int i = 0; i < c->samples; i++) {
        c->pcm[i*2] = (int16_t)recomp_load(&c->rt, BOOT_PCM+(uint32_t)(i*c->channels*2), 2);
        c->pcm[i*2+1] = c->channels == 1 ? c->pcm[i*2] :
            (int16_t)recomp_load(&c->rt, BOOT_PCM+(uint32_t)(i*4+2), 2);
    }
    c->frames++;
    return 1;
}

void media_codec_close(MediaCodec *c) {
    if (c->file) fclose(c->file);
    recomp_runtime_destroy(&c->rt);
}
