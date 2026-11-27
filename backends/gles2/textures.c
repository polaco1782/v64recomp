#include "internal.h"
#include "hq2x.h"
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static unsigned byte(RecompRDPGles2 *s, unsigned offset) {
    if (s->tmem_invalid[offset & 4095])
        recomp_fail("GLES2 drawing consumes an invalid texture DMA", offset & 4095);
    return s->tmem[offset & 4095];
}
static unsigned tile_extent(const RecompRDPGles2 *s, unsigned axis, unsigned low, unsigned high, unsigned mask,
                            unsigned mode, unsigned shift) {
    unsigned extent = high >= low ? (high - low) / 4 + 1 : 1;
    if (!mask || (mode & 2))
        return extent;
    if (mask <= 10)
        return 1u << mask;
    /* S2DEX backgrounds set a very large wrap mask and zero tile bounds.
     * Each rectangle addresses a slice of TMEM. Decode the texels covered
     * by its coordinates, including reversed and mirrored axes. */
    if (s->drawing_rectangle) {
        float scale = shift > 10 ? (float)(1u << (16 - shift)) : 1.0f / (float)(1u << shift);
        float last = 0;
        for (unsigned i = 0; i < s->batch_count; i++) {
            float uv = s->batch[i].uv[axis] * scale - (float)low / 4;
            if (uv < 0 && (mode & 1))
                uv = -1.0f - uv;
            last = fmaxf(last, uv);
        }
        unsigned sampled = (unsigned)fminf(1024, floorf(last) + 1);
        if (sampled > extent)
            extent = sampled;
    }
    return extent;
}

static void discard_texture(RecompRDPGles2 *s, TextureCache *cache, Texture *entry) {
    if (entry->id)
        s->gl.DeleteTextures(1, &entry->id);
#ifdef RECOMP_GLES2_TEXTURE_PACKS
    free(entry->preview);
#endif
    cache->bytes -= entry->bytes;
    *entry = (Texture){0};
}

void recomp_gles2_clear_texture_caches(RecompRDPGles2 *s) {
    for (unsigned i = 0; i < GLES2_TEXTURE_SLOTS; i++) {
        discard_texture(s, &s->textures, &s->textures.slots[i]);
#ifdef RECOMP_GLES2_TEXTURE_PACKS
        discard_texture(s, &s->replacements, &s->replacements.slots[i]);
#endif
    }
}

static size_t texture_size(unsigned width, unsigned height, bool mipmaps) {
    size_t bytes = (size_t)width * height * 4;
    while (mipmaps && (width > 1 || height > 1)) {
        width = width > 1 ? width / 2 : 1;
        height = height > 1 ? height / 2 : 1;
        bytes += (size_t)width * height * 4;
    }
    return bytes;
}
/* Keep small, frequently reused tiles resident without allowing large repeated
 * tiles or their mip chains to grow the native cache without bound. */
static void reserve_texture(RecompRDPGles2 *s, TextureCache *cache, Texture *slot, size_t bytes, size_t budget) {
    discard_texture(s, cache, slot);
    while (cache->bytes + bytes > budget) {
        Texture *oldest = NULL;
        for (unsigned i = 0; i < GLES2_TEXTURE_SLOTS; i++) {
            Texture *entry = &cache->slots[i];
            if (entry->id && (!oldest || entry->used < oldest->used))
                oldest = entry;
        }
        if (!oldest)
            recomp_fail("GLES texture exceeds cache budget", 0);
        discard_texture(s, cache, oldest);
    }
    slot->bytes = bytes;
    cache->bytes += bytes;
}

static Texture *texture_slot(TextureCache *cache, unsigned ways, uint64_t hash, unsigned wrap) {
    size_t bucket = (hash ^ (hash >> 32)) % (GLES2_TEXTURE_SLOTS / ways);
    Texture *slots = &cache->slots[bucket * ways], *oldest = slots;
    for (unsigned i = 0; i < ways; i++) {
        Texture *entry = &slots[i];
        if (entry->id && entry->hash == hash && entry->hq2x_wrap == wrap)
            return entry;
        if (entry->used < oldest->used)
            oldest = entry;
    }
    return oldest;
}

static unsigned indexed_texel(RecompRDPGles2 *s, const Tile *t, unsigned sample_size, unsigned x, unsigned y) {
    unsigned offset = (t->tmem * 8 + y * t->line * 8 + ((x << sample_size) >> 1)) ^ ((y & 1) ? 4 : 0);
    offset &= 2047;
    if (t->siz)
        return byte(s, offset);
    unsigned shift = sample_size == 0 && (x & 1) ? 0 : 4;
    if ((s->tmem_invalid[offset] >> shift) & 15)
        recomp_fail("GLES2 drawing consumes an invalid texture DMA", offset);
    return (s->tmem[offset] >> shift) & 15;
}
static unsigned palette_color(RecompRDPGles2 *s, const Tile *t, unsigned index) {
    unsigned k = 2048 + (index + (t->siz == 0 ? t->palette * 16 : 0)) * 8;
    return byte(s, k) * 256 + byte(s, k + 1);
}

static void decode_tile(RecompRDPGles2 *s, const Tile *t, unsigned width, unsigned height, bool indexed, unsigned mask,
                        unsigned sample_size, unsigned tlut, uint8_t *pixels) {
    for (unsigned y = 0; y < height; y++)
        for (unsigned x = 0; x < width; x++) {
            uint8_t *p = pixels + ((size_t)y * width + x) * 4;
            if (indexed) {
                unsigned index = indexed_texel(s, t, sample_size, x, y);
                pixel16(palette_color(s, t, index), p, tlut == 3);
                continue;
            }
            unsigned o = t->tmem * 8 + y * t->line * 8 + ((x << sample_size) >> 1);
            o ^= (y & 1) ? 4 : 0;
            unsigned b0 = byte(s, o & mask), b1 = t->siz >= 2 ? byte(s, (o + 1) & mask) : 0,
                     nibble = (b0 >> ((sample_size == 0 && (x & 1)) ? 0 : 4)) & 15;
            if (t->siz == 0) {
                unsigned i = nibble * 17, a = i;
                if (t->fmt == 3) {
                    i = ((nibble >> 1) * 255) / 7;
                    a = (nibble & 1) ? 255 : 0;
                }
                p[0] = p[1] = p[2] = (uint8_t)i;
                p[3] = (uint8_t)a;
            } else if (t->siz == 1) {
                unsigned i = b0, a = b0;
                if (t->fmt == 3) {
                    i = (b0 >> 4) * 17;
                    a = (b0 & 15) * 17;
                }
                p[0] = p[1] = p[2] = (uint8_t)i;
                p[3] = (uint8_t)a;
            } else if (t->siz == 2)
                pixel16(b0 * 256 + b1, p, t->fmt == 3);
            else {
                p[0] = (uint8_t)b0;
                p[1] = (uint8_t)b1;
                p[2] = (uint8_t)byte(s, (o & 2047) | 2048);
                p[3] = (uint8_t)byte(s, ((o + 1) & 2047) | 2048);
            }
        }
}

#ifdef RECOMP_GLES2_TEXTURE_PACKS
static GLuint replacement_texture(RecompRDPGles2 *s, Gles2PackTile key, size_t source, bool known, uint8_t **original) {
    Texture *entry = texture_slot(&s->replacements, GLES2_REPLACEMENT_WAYS, source, 0);
    bool resident = entry->id && entry->hash == source;
    if (resident) {
        if ((uint64_t)entry->image_width * key.height != (uint64_t)entry->image_height * key.width)
            return 0;
    } else {
        Gles2PackImage image;
        if (!gles2_texture_pack_decode(s->texture_pack, source, key, s->max_texture_size, &image))
            return 0;
        size_t size = texture_size(image.width, image.height, s->mipmaps);
        reserve_texture(s, &s->replacements, entry, size, 256u * 1024 * 1024);
        s->gl.GenTextures(1, &entry->id);
        s->gl.BindTexture(GL_TEXTURE_2D, entry->id);
        s->gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image.width, image.height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                         image.pixels);
        if (s->mipmaps)
            s->gl.GenerateMipmap(GL_TEXTURE_2D);
        entry->image_width = image.width;
        entry->image_height = image.height;
        entry->hash = source;
        if (s->editor)
            entry->preview = gles2_editor_make_preview(image.width, image.height, image.pixels);
        gles2_texture_pack_free_image(&image);
    }
    if (!known)
        gles2_texture_pack_remember(s->texture_pack, key.hash, source);
    if (entry->preview && (!known || !resident || !gles2_editor_touch(s->editor, key.hash, true)))
        *original = gles2_editor_observe_replacement(s->editor, key.hash, key.width, key.height, entry->preview);
    entry->used = ++s->texture_clock;
    return entry->id;
}

static GLuint find_replacement(RecompRDPGles2 *s, Gles2PackTile key, const Tile *tile, unsigned tlut, bool *incomplete,
                               uint8_t **original) {
    size_t source, known = SIZE_MAX;
    if (gles2_texture_pack_known(s->texture_pack, key.hash, &known)) {
        GLuint texture = replacement_texture(s, key, known, true, original);
        if (texture)
            return texture;
    }
    if (gles2_texture_pack_ai(s->texture_pack, key.hash, &source) && source != known) {
        GLuint ai = replacement_texture(s, key, source, false, original);
        if (ai)
            return ai;
    }
    if (gles2_texture_pack_local(s->texture_pack, key.hash, &source) && source != known) {
        GLuint local = replacement_texture(s, key, source, false, original);
        if (local)
            return local;
    }
    Gles2PackStatus status = gles2_texture_pack_rice(s->texture_pack, key, tile, tlut, s->rice_words, &source);
    *incomplete = status == GLES2_PACK_INCOMPLETE;
    return status == GLES2_PACK_FOUND && source != known ? replacement_texture(s, key, source, false, original) : 0;
}
#endif
static uint64_t hash_value(uint64_t hash, unsigned value) {
    return (hash ^ value) * UINT64_C(1099511628211);
}
static uint64_t finish_tile_hash(uint64_t hash, unsigned width, unsigned height, const Tile *tile, unsigned palette,
                                 unsigned tlut) {
    unsigned fields[] = {width, height, tile->fmt, tile->siz, palette, tlut};
    for (unsigned i = 0; i < sizeof(fields) / sizeof(*fields); ++i)
        hash = hash_value(hash, fields[i]);
    return hash;
}

/* Preserve the original IDs for direct-color tiles. Old indexed replacement filenames
 * used this same byte stream followed by the entire contents and validity of the TLUT. */
static uint64_t raw_tile_hash(RecompRDPGles2 *s, const Tile *t, unsigned width, unsigned height, bool indexed,
                              bool split_banks, unsigned mask, unsigned sample_size) {
    unsigned row_bytes = (width * (4u << sample_size) + 7) / 8;
    uint64_t hash = UINT64_C(1469598103934665603);
    for (unsigned y = 0; y < height; y++)
        for (unsigned bank = 0; bank < (split_banks ? 2u : 1u); bank++)
            for (unsigned x = 0; x < row_bytes; x += indexed && sample_size == 2 ? 2 : 1) {
                unsigned offset = (t->tmem * 8 + y * t->line * 8 + x) ^ ((y & 1) ? 4 : 0);
                offset = (offset & mask) | (bank * 2048);
                hash = hash_value(hash, indexed ? s->tmem[offset] : byte(s, offset));
            }
    if (indexed)
        for (unsigned i = 2048; i < 4096; i++)
            hash = hash_value(hash, (unsigned)s->tmem[i] | ((unsigned)s->tmem_invalid[i] << 8));
    return hash;
}

GLuint recomp_gles2_texture(RecompRDPGles2 *s, Tile *t, unsigned unit) {
    unsigned *width = &s->tex_width[unit], *height = &s->tex_height[unit];
    *width = tile_extent(s, 0, t->sl, t->sh, t->ms, t->cs, t->ss);
    *height = tile_extent(s, 1, t->tl, t->th, t->mt, t->ct, t->st);
    /* The RDP accepts a zero TMEM line stride: rows share a base, but
     * the address swizzle still uses the sampled row parity. */
    if (*width > 1024 || *height > 1024) {
        fprintf(stderr, "tile=%u fmt=%u siz=%u line=%u tmem=%u size=%ux%u active=%u mux=%06x:%08x mode=%08x\n",
                (unsigned)(t - s->tiles), t->fmt, t->siz, t->line, t->tmem, *width, *height, s->texture_tile, s->mux0,
                s->mux1, s->other_high);
        recomp_fail("GLES2 unsupported tile dimensions", t->sh);
    }
    unsigned tlut = (s->other_high >> 14) & 3;
    /* TEXTLUT enables palette lookup independently of the tile format. Conker
     * uses RGBA-tagged 4- and 8-bit indices as well as ordinary CI tiles. */
    bool indexed = (tlut & 2) != 0;
    bool split_banks = t->siz == 3 && !indexed;
    unsigned mask = indexed || split_banks ? 2047 : 4095;
    unsigned sample_size = t->siz == 3 ? 2 : t->siz;
    if (indexed && t->fmt == 1)
        sample_size = 1; /* YUV addresses index bytes, including 4-bit indices in the high nibble. */
    uint64_t hash;
    if (indexed) {
        /* Include the index pattern and exactly the palette words it samples. CI4 padding,
         * replicated TLUT lanes and the bank containing the palette do not identify the tile. */
        bool used[256] = {false};
        hash = UINT64_C(1469598103934665603);
        for (unsigned y = 0; y < *height; ++y)
            for (unsigned x = 0; x < *width; ++x) {
                unsigned index = indexed_texel(s, t, sample_size, x, y);
                used[index] = true;
                hash = hash_value(hash, index);
            }
        for (unsigned i = 0; i < (t->siz ? 256u : 16u); ++i)
            if (used[i])
                hash = hash_value(hash, palette_color(s, t, i));
    } else
        hash = raw_tile_hash(s, t, *width, *height, false, split_banks, mask, sample_size);
    hash = finish_tile_hash(hash, *width, *height, t, indexed ? 0 : t->palette, tlut);
#ifdef RECOMP_GLES2_TEXTURE_PACKS
    s->active_tile_key[unit] = hash;
    bool replace = !s->original_textures && gles2_texture_pack_enabled(s->texture_pack);
    size_t unidentified = replace ? gles2_texture_pack_unidentified(s->texture_pack) : 0;
    if (unidentified) {
        gles2_texture_pack_identify(s->texture_pack, hash, hash);
        if (indexed && gles2_texture_pack_unidentified(s->texture_pack)) {
            uint64_t legacy = raw_tile_hash(s, t, *width, *height, true, false, mask, sample_size);
            legacy = finish_tile_hash(legacy, *width, *height, t, t->palette, tlut);
            gles2_texture_pack_identify(s->texture_pack, legacy, hash);
        }
        unidentified = gles2_texture_pack_unidentified(s->texture_pack);
    }
    const uint64_t rice_texels = s->rice_active ? s->rice_words[t->tmem].load : 0;
    const uint64_t rice_palette =
        s->rice_active && indexed ? s->rice_words[256 + (t->siz == 0 ? 16 * t->palette : 0)].load : 0;
#endif
    // 4 KiB of TMEM holds at most 8192 distinct 4-bit texels. Some rectangles
    // span a much larger repeated address range. Bound enhancement cost there.
    bool enhance = s->hq2x && *width * *height <= 8192;
#ifdef RECOMP_GLES2_TEXTURE_PACKS
    enhance = enhance && !s->original_textures;
#endif
    bool wrap_x = enhance && !t->cs && t->ms && (1u << t->ms) == *width;
    bool wrap_y = enhance && !t->ct && t->mt && (1u << t->mt) == *height;
    unsigned hq2x_wrap = (unsigned)wrap_x | ((unsigned)wrap_y << 1);
    Texture *entry = texture_slot(&s->textures, GLES2_NATIVE_WAYS, hash, hq2x_wrap);
    bool cached = entry->id && entry->hash == hash && entry->hq2x_wrap == hq2x_wrap;
#ifdef RECOMP_GLES2_TEXTURE_PACKS
    /* An incomplete Rice lookup can become a match after a fresh TMEM load. */
    bool retry = cached && entry->retry_rice && rice_texels && (rice_texels & 511) == t->tmem &&
                 (entry->rice_texels != rice_texels || entry->rice_palette != rice_palette);
    retry |= cached && entry->unidentified != unidentified;
    bool incomplete = false;
    if (replace && (!cached || retry)) {
        Gles2PackTile key = {hash, *width, *height};
        uint8_t *original = NULL;
        GLuint texture = find_replacement(s, key, t, tlut, &incomplete, &original);
        if (texture) {
            if (original)
                decode_tile(s, t, *width, *height, indexed, mask, sample_size, tlut, original);
            if (cached)
                discard_texture(s, &s->textures, entry);
            return texture;
        }
    }
#endif
    if (!cached) {
        unsigned scale = enhance ? 2 : 1;
        size_t bytes = texture_size(*width * scale, *height * scale, s->mipmaps);
        reserve_texture(s, &s->textures, entry, bytes, 64u * 1024 * 1024);
    }
#ifdef RECOMP_GLES2_TEXTURE_PACKS
    if (!cached || retry) {
        entry->unidentified = unidentified;
        entry->retry_rice = incomplete;
        entry->rice_texels = rice_texels;
        entry->rice_palette = rice_palette;
    }
#endif
    if (cached) {
        entry->used = ++s->texture_clock;
#ifdef RECOMP_GLES2_TEXTURE_PACKS
        if (!s->editor || gles2_editor_touch(s->editor, hash, false))
#endif
            return entry->id;
    }
    size_t size = (size_t)*width * *height * 4;
    uint8_t *pixels = malloc(size);
    if (!pixels)
        recomp_fail("GLES2 texture allocation failed", 0);
    decode_tile(s, t, *width, *height, indexed, mask, sample_size, tlut, pixels);
#ifdef RECOMP_GLES2_TEXTURE_PACKS
    if (s->editor)
        gles2_editor_observe(s->editor, hash, *width, *height, pixels);
    if (cached) {
        free(pixels);
        return entry->id;
    }
#endif
    if (!entry->id)
        s->gl.GenTextures(1, &entry->id);
    s->gl.BindTexture(GL_TEXTURE_2D, entry->id);
    unsigned scale = 1;
    if (enhance) {
        uint32_t *scaled = malloc(size * 4);
        if (scaled) {
            gles2_hq2x(pixels, *width, *height, wrap_x, wrap_y, scaled);
            free(pixels);
            pixels = (uint8_t *)scaled;
            scale = 2;
        }
    }
    s->gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)(*width * scale), (GLsizei)(*height * scale), 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, pixels);
    if (s->mipmaps)
        s->gl.GenerateMipmap(GL_TEXTURE_2D);
    free(pixels);
    s->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    s->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    entry->hq2x_wrap = hq2x_wrap;
    entry->hash = hash;
    entry->used = ++s->texture_clock;
    return entry->id;
}

/* SETTIMG only changes a register. Some lists load a sentinel image during
 * cleanup, then overwrite TMEM before it is sampled. Track undefined DMA bytes
 * until a texture actually observes them; never fabricate a drawable texture.
 * Sources are RDP physical/segmented addresses, independent of CPU TLB aliases. */
static bool texture_source(RecompRDPGles2 *s, uint64_t offset, size_t bytes, uint32_t *physical) {
    uint64_t base = s->image_address & 0x03ffffff;
    uint64_t source = base + offset;
    if (source >= s->rt->rdram_size || bytes > s->rt->rdram_size - source)
        return false;
    *physical = (uint32_t)source;
    return true;
}
static void mark_source(RecompRDPGles2 *s, unsigned offset, bool valid, unsigned nibbles) {
    s->tmem_source[offset >> 3] = valid ? (s->image_address & 0x03ffffff) + 1 : 0;
#ifdef RECOMP_GLES2_TEXTURE_PACKS
    if (s->rice_active) {
        Gles2RiceWord *word = &s->rice_words[offset >> 3];
        if (word->load != s->rice_load) {
            word->load = s->rice_load;
            word->nibbles = 0;
        }
        unsigned mask = nibbles << (2 * (offset & 7));
        word->nibbles = (word->nibbles & ~mask) | (valid ? mask : 0);
    }
#else
    (void)nibbles;
#endif
}
static void tmem_store(RecompRDPGles2 *s, unsigned offset, bool valid, unsigned value) {
    offset &= 4095;
    s->tmem[offset] = (uint8_t)value;
    s->tmem_invalid[offset] = valid ? 0 : 255;
    mark_source(s, offset, valid, 3);
}
TextureTransfer recomp_gles2_texture_transfer(unsigned image_width, unsigned image_siz, unsigned tile_siz, uint32_t w0,
                                              uint32_t w1) {
    unsigned sl = (w0 >> 12) & 4095, tl = w0 & 4095, sh = (w1 >> 12) & 4095, th = w1 & 4095;
    unsigned bits = 4u << image_siz;
    unsigned op = (w0 >> 24) | 0xc0;
    if (sh < sl || (op != 0xf3 && th < tl))
        recomp_fail("GLES2 malformed texture DMA", w0);
    if (op == 0xf3) {
        unsigned words = ((sh - sl) >> (4 - tile_siz)) + 1;
        return (TextureTransfer){((uint64_t)tl * image_width + sl) * bits, 0, words * 64, 1, 0};
    }
    unsigned width = (sh - sl) / 4 + 1, height = (th - tl) / 4 + 1;
    if (op == 0xf0) {
        width = (sh >> 2) - (sl >> 2) + 1;
        height = (th >> 2) - (tl >> 2) + 1;
        bits = 16;
    }
    return (TextureTransfer){((uint64_t)(tl >> 2) * image_width + (sl >> 2)) * bits, image_width * bits, width * bits,
                             height, width};
}
void recomp_gles2_load_tile(RecompRDPGles2 *s, Tile *t, uint32_t w0, uint32_t w1, bool block, bool palette) {
    unsigned sl = (w0 >> 12) & 4095, tl = w0 & 4095, sh = (w1 >> 12) & 4095, th = w1 & 4095;
    unsigned bits = 4u << s->image_siz;
    TextureTransfer dma = recomp_gles2_texture_transfer(s->image_width, s->image_siz, t->siz, w0, w1);
#ifdef RECOMP_GLES2_TEXTURE_PACKS
    if (s->rice_active) {
        s->rice_load = ((s->rice_load & ~UINT64_C(511)) + 512) | t->tmem;
        gles2_texture_pack_record_load(s->texture_pack, s->rice_load, t, w0, w1, s->image_width, s->image_siz, &dma,
                                       s->draw_ram, s->rt->rdram_size, s->image_address & 0x03ffffff);
    }
#endif
    /* LOADTILE also latches the tile's sampling bounds. Raw RDP video may
     * upload and draw through this same tile without a separate SETTILESIZE. */
    if (!block && !palette) {
        t->sl = sl;
        t->tl = tl;
        t->sh = sh;
        t->th = th;
    }
    if (palette) {
        unsigned width = dma.width;
        for (unsigned y = 0; y < dma.rows; y++) {
            uint32_t src = 0;
            bool valid = texture_source(s, (dma.first_bit + y * dma.stride_bits) / 8, width * 2, &src);
            for (unsigned i = 0; i < width; i++)
                for (unsigned lane = 0; lane < 4; lane++) {
                    unsigned dst = t->tmem * 8 + y * t->line * 32 + i * 8 + lane * 2;
                    tmem_store(s, dst, valid, valid ? load(s, src + i * 2, 1) : 0);
                    tmem_store(s, dst + 1, valid, valid ? load(s, src + i * 2 + 1, 1) : 0);
                }
        }
    } else if (block) {
        /* LOADBLOCK coordinates are integer texels, unlike LOADTILE/TLUT's
         * quarter-texel bounds. Each transfer reads a complete 64-bit word. */
        unsigned bytes = dma.row_bits / 8;
        uint32_t src = 0;
        bool valid = texture_source(s, dma.first_bit / 8, bytes, &src);
        if (t->siz != 3) {
            /* Each DMA word replaces all eight bytes and their provenance.
             * Odd rows exchange the two 32-bit halves within that word. */
            for (unsigned i = 0; i < bytes; i += 8) {
                unsigned row = ((i / 8) * th) >> 11;
                unsigned word = (t->tmem + i / 8 + row * t->line) & 511;
                unsigned swap = (row & 1) * 4;
                for (unsigned b = 0; b < 8; b++)
                    s->tmem[word * 8 + (b ^ swap)] = valid ? load(s, src + i + b, 1) : 0;
                memset(s->tmem_invalid + word * 8, valid ? 0 : 255, 8);
                s->tmem_source[word] = valid ? (s->image_address & 0x03ffffff) + 1 : 0;
#ifdef RECOMP_GLES2_TEXTURE_PACKS
                if (s->rice_active)
                    s->rice_words[word] = (Gles2RiceWord){.load = s->rice_load, .nibbles = valid ? UINT16_MAX : 0};
#endif
            }
        } else {
            for (unsigned i = 0; i < bytes; i++) {
                unsigned word = i / 8, row = (word * th) >> 11;
                unsigned pixel = i / 4, part = i & 3;
                unsigned offset = t->tmem * 8 + pixel * 2 + (part & 1) + row * t->line * 8;
                offset = (offset & 2047) | (part >= 2 ? 2048 : 0);
                offset ^= (row & 1) ? 4 : 0;
                tmem_store(s, offset, valid, valid ? load(s, src + i, 1) : 0);
            }
        }
    } else {
        for (unsigned y = 0; y < dma.rows; y++)
            for (unsigned x = 0; x < dma.width; x++) {
                uint64_t bit = dma.first_bit + y * dma.stride_bits + x * bits;
                unsigned bpp = bits >= 8 ? bits / 8 : 1;
                uint32_t src = 0;
                bool valid = texture_source(s, bit / 8, bpp, &src);
                for (unsigned b = 0; b < bpp; b++) {
                    unsigned offset = t->tmem * 8 + y * t->line * 8 + ((x * bits) >> 3) + b;
                    if (bits == 32)
                        offset = ((t->tmem * 8 + y * t->line * 8 + x * 2 + (b & 1)) & 2047) | (b >= 2 ? 2048 : 0);
                    offset ^= (y & 1) ? 4 : 0;
                    offset &= 4095;
                    unsigned value = valid ? load(s, src + b, 1) : 0;
                    if (bits == 4) {
                        unsigned nibble = (value >> ((bit & 4) ? 0 : 4)) & 15, shift = (x & 1) ? 0 : 4;
                        unsigned mask = 15u << shift;
                        s->tmem[offset] = (uint8_t)((s->tmem[offset] & ~mask) | (nibble << shift));
                        s->tmem_invalid[offset] = (uint8_t)((s->tmem_invalid[offset] & ~mask) | (valid ? 0 : mask));
                        mark_source(s, offset, valid, shift ? 1 : 2);
                    } else
                        tmem_store(s, offset, valid, value);
                }
            }
    }
    s->textures_dirty = true;
}

#ifdef RECOMP_GLES2_TEXTURE_PACKS
void recomp_gles2_reload_textures(RecompRDPGles2 *s) {
    recomp_gles2_require_owner(s);
    recomp_gles2_flush(s);
    char title[21] = {0};
    if (s->rt->rom && s->rt->rom_size >= 52) {
        memcpy(title, s->rt->rom + 32, 20);
        for (size_t end = 20; end && (title[end - 1] == ' ' || title[end - 1] == 0); --end)
            title[end - 1] = 0;
    }
    gles2_texture_pack_close(s->texture_pack);
    s->texture_pack = gles2_texture_pack_open(s->texture_zip, s->texture_overrides, title);
    s->rice_active = !s->original_textures && gles2_texture_pack_has_rice(s->texture_pack);
    s->rice_load = 0;
    memset(s->rice_words, 0, sizeof(s->rice_words));
    recomp_gles2_clear_texture_caches(s);
    s->textures_dirty = true;
}

void recomp_rdp_gles2_set_texture_path(RecompRDPGles2 *s, const char *data_path) {
    static const char suffix[] = "/texture-overrides";
    size_t length = data_path ? strlen(data_path) : 0;
    char *path = length && length < SIZE_MAX - sizeof(suffix) ? malloc(length + sizeof(suffix)) : NULL;
    if (path) {
        memcpy(path, data_path, length);
        memcpy(path + length, suffix, sizeof(suffix));
    }
    free(s->texture_overrides);
    s->texture_overrides = path;
    SDL_free(s->texture_zip);
    s->texture_zip = NULL;
    const char *pack = getenv("RECOMP_TEXTURE_PACK");
    if (pack && *pack)
        s->texture_zip = SDL_strdup(pack);
    else if (path)
        SDL_asprintf(&s->texture_zip, "%s/textures.zip", path);
    recomp_gles2_reload_textures(s);
}
#endif
