#include "internal.h"
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
#ifdef RECOMP_GLES2_TEXTURE_PACKS
/* Native tiles turn over much faster than the HD working set. Keep uploaded
 * replacements (including their mip chains) resident across native evictions. */
static void discard_replacement(RecompRDPGles2 *s, Texture *entry) {
    if (entry->id)
        s->gl.DeleteTextures(1, &entry->id);
    s->replacement_bytes -= entry->replacement_bytes;
    *entry = (Texture){0};
}
void recomp_gles2_clear_replacements(RecompRDPGles2 *s) {
    for (unsigned i = 0; i < GLES2_REPLACEMENT_BUCKETS; i++)
        for (unsigned j = 0; j < GLES2_REPLACEMENT_WAYS; j++)
            discard_replacement(s, &s->replacements[i][j]);
}
static size_t replacement_size(unsigned width, unsigned height, bool mipmaps) {
    size_t bytes = (size_t)width * height * 4;
    while (mipmaps && (width > 1 || height > 1)) {
        width = width > 1 ? width / 2 : 1;
        height = height > 1 ? height / 2 : 1;
        bytes += (size_t)width * height * 4;
    }
    return bytes;
}
static bool reserve_replacement(RecompRDPGles2 *s, Texture *slot, size_t bytes) {
    const size_t budget = 256u * 1024 * 1024;
    if (!slot || bytes > budget)
        return false;
    discard_replacement(s, slot);
    while (s->replacement_bytes + bytes > budget) {
        Texture *oldest = NULL;
        for (unsigned i = 0; i < GLES2_REPLACEMENT_BUCKETS; i++)
            for (unsigned j = 0; j < GLES2_REPLACEMENT_WAYS; j++) {
                Texture *entry = &s->replacements[i][j];
                if (entry->id && (!oldest || entry->used < oldest->used))
                    oldest = entry;
            }
        if (!oldest)
            return false;
        discard_replacement(s, oldest);
    }
    slot->replacement_bytes = bytes;
    s->replacement_bytes += bytes;
    return true;
}
static Texture *replacement_entry(RecompRDPGles2 *s, const Gles2PackTile *key) {
    Texture *bucket = s->replacements[(key->hash ^ (key->hash >> 32)) % GLES2_REPLACEMENT_BUCKETS];
    Texture *oldest = bucket;
    for (unsigned j = 0; j < GLES2_REPLACEMENT_WAYS; ++j) {
        Texture *entry = &bucket[j];
        if (entry->id && entry->hash == key->hash && entry->has_rt64_id == key->has_id && entry->rt64_id == key->id)
            return entry;
        if (entry->used < oldest->used)
            oldest = entry;
    }
    return oldest;
}

// Install completed textures between display lists, even when the original tile
// has already left TMEM. Both texture units rebind before the next textured draw.
void recomp_gles2_stream_replacements(RecompRDPGles2 *s) {
    ++s->pack_draw;
    s->textures_dirty |= s->pack_pending;
    s->pack_pending = false;
    size_t bytes = 0;
    for (unsigned i = 0; i < 4 && bytes < 2 * 1024 * 1024; ++i) {
        Gles2PackTile key;
        Gles2PackImage image;
        if (!gles2_texture_pack_take(s->texture_pack, &key, &image))
            break;
        Texture *entry = replacement_entry(s, &key);
        size_t size = replacement_size(image.width, image.height, s->mipmaps);
        if (reserve_replacement(s, entry, size)) {
            s->gl.GenTextures(1, &entry->id);
            s->gl.BindTexture(GL_TEXTURE_2D, entry->id);
            s->gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image.width, image.height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                             image.pixels);
            if (s->mipmaps)
                s->gl.GenerateMipmap(GL_TEXTURE_2D);
            entry->hash = key.hash;
            entry->rt64_id = key.id;
            entry->has_rt64_id = key.has_id;
            entry->used = ++s->texture_clock;
            if (s->editor && key.has_id)
                gles2_editor_observe_replacement(s->editor, key.id, key.width, key.height, image.width, image.height,
                                                 image.pixels);
            bytes += size;
            s->textures_dirty = true;
        }
        gles2_texture_pack_free_image(&image);
    }
}
#endif
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
    uint64_t hash = UINT64_C(1469598103934665603);
    unsigned tlut = (s->other_high >> 14) & 3;
    /* TEXTLUT enables palette lookup independently of the tile format. Conker
     * uses RGBA-tagged 4- and 8-bit indices as well as ordinary CI tiles. */
    bool indexed = (tlut & 2) != 0;
    bool split_banks = t->siz == 3 && !indexed;
    unsigned mask = indexed || split_banks ? 2047 : 4095;
    unsigned sample_size = t->siz == 3 ? 2 : t->siz;
    if (indexed && t->fmt == 1)
        sample_size = 1; /* YUV addresses index bytes, including 4-bit indices in the high nibble. */
    unsigned row_bytes = (*width * (4u << sample_size) + 7) / 8;
    /* Cache the tile's contents, independent of unrelated TMEM loads and of
     * where the game placed the same texture in TMEM. */
    for (unsigned y = 0; y < *height; y++)
        for (unsigned bank = 0; bank < (split_banks ? 2u : 1u); bank++)
            for (unsigned x = 0; x < row_bytes; x += indexed && sample_size == 2 ? 2 : 1) {
                unsigned offset = (t->tmem * 8 + y * t->line * 8 + x) ^ ((y & 1) ? 4 : 0);
                hash ^= byte(s, (offset & mask) | (bank * 2048));
                hash *= UINT64_C(1099511628211);
            }
    if (indexed)
        for (unsigned i = 2048; i < 4096; i++) {
            hash ^= (unsigned)s->tmem[i] | ((unsigned)s->tmem_invalid[i] << 8);
            hash *= UINT64_C(1099511628211);
        }
    unsigned key[] = {*width, *height, t->fmt, t->siz, t->palette, tlut};
    for (unsigned i = 0; i < sizeof(key) / sizeof(*key); i++) {
        hash ^= key[i];
        hash *= UINT64_C(1099511628211);
    }
#ifdef RECOMP_GLES2_TEXTURE_PACKS
    uint64_t rt64_id = 0;
    bool replace = !s->original_textures && gles2_texture_pack_enabled(s->texture_pack);
    unsigned rt64_width = t->ms ? 1u << t->ms : *width;
    unsigned rt64_height = t->mt ? 1u << t->mt : *height;
    if (t->ms && (t->cs & 2) && rt64_width > *width)
        rt64_width = *width;
    if (t->mt && (t->ct & 2) && rt64_height > *height)
        rt64_height = *height;
    const uint64_t rice_texels = s->rice_active ? s->rice_words[t->tmem].load : 0;
    const uint64_t rice_palette =
        s->rice_active && indexed ? s->rice_words[256 + (t->siz == 0 ? 16 * t->palette : 0)].load : 0;
    const bool can_id = rt64_width == *width && rt64_height == *height;
    /* An active pack needs the exact versioned identity before cache lookup.
     * The editor alone can reuse the identity of an existing visual preview
     * and only fingerprint decoded/uploads, not repeated state changes. */
    bool has_rt64_id =
        replace && can_id &&
        gles2_texture_rt64_id(s->tmem, t->fmt, t->siz, t->line, t->tmem, t->palette, tlut, *width, *height, &rt64_id);
    Gles2PackTile pack_key = {hash, rt64_id, *width, *height, has_rt64_id};
    if (replace) {
        Texture *r = replacement_entry(s, &pack_key);
        if (r->id && r->hash == hash && r->has_rt64_id == has_rt64_id && r->rt64_id == rt64_id) {
            r->used = ++s->texture_clock;
            s->texture_id_valid[unit] = has_rt64_id;
            s->texture_id[unit] = rt64_id;
            return r->id;
        }
    }
#endif
    Texture *cached = NULL, *oldest = &s->textures[0];
    for (unsigned i = 0; i < 128; i++) {
        Texture *candidate = &s->textures[i];
        if (candidate->id && candidate->hash == hash
#ifdef RECOMP_GLES2_TEXTURE_PACKS
            && (!replace || (candidate->has_rt64_id == has_rt64_id && candidate->rt64_id == rt64_id))
#endif
        ) {
            cached = candidate;
            break;
        }
        if (candidate->used < oldest->used)
            oldest = candidate;
    }
    Texture *entry = cached ? cached : oldest;
#ifdef RECOMP_GLES2_TEXTURE_PACKS
    /* On an incomplete first draw, retry after a fresh load at the tile
     * base. Keep the resident GL texture if the retry still has no match. */
    bool retry = cached && cached->retry_rice && rice_texels && (rice_texels & 511) == t->tmem &&
                 (cached->rice_texels != rice_texels || cached->rice_palette != rice_palette);
    if (!cached || retry || (cached->retry_draw && cached->retry_draw < s->pack_draw)) {
        Gles2PackStatus status = GLES2_PACK_MISS;
        if (replace && (has_rt64_id || s->rice_active))
            status = gles2_texture_pack_request(s->texture_pack, pack_key, t, tlut, s->rice_words, s->max_texture_size);
        entry->retry_rice = status == GLES2_PACK_INCOMPLETE;
        entry->retry_draw = status == GLES2_PACK_PENDING ? s->pack_draw : 0;
        entry->rice_texels = rice_texels;
        entry->rice_palette = rice_palette;
    }
    s->pack_pending |= entry->retry_draw != 0;
#endif
    if (cached) {
        entry->used = ++s->texture_clock;
#ifdef RECOMP_GLES2_TEXTURE_PACKS
        s->texture_id_valid[unit] = entry->has_rt64_id;
        s->texture_id[unit] = entry->rt64_id;
#endif
        return entry->id;
    }
#ifdef RECOMP_GLES2_TEXTURE_PACKS
    if (!replace && s->editor && can_id)
        has_rt64_id = gles2_texture_rt64_id(s->tmem, t->fmt, t->siz, t->line, t->tmem, t->palette, tlut, *width,
                                            *height, &rt64_id);
    s->texture_id_valid[unit] = has_rt64_id;
    s->texture_id[unit] = rt64_id;
#endif

    size_t size = (size_t)*width * *height * 4;
    uint8_t *pixels = malloc(size);
    if (!pixels)
        recomp_fail("GLES2 texture allocation failed", 0);
    for (unsigned y = 0; y < *height; y++)
        for (unsigned x = 0; x < *width; x++) {
            unsigned o = t->tmem * 8 + y * t->line * 8 + ((x << sample_size) >> 1);
            o ^= (y & 1) ? 4 : 0;
            unsigned b0 = byte(s, o & mask), b1 = !indexed && t->siz >= 2 ? byte(s, (o + 1) & mask) : 0,
                     nibble = (b0 >> ((sample_size == 0 && (x & 1)) ? 0 : 4)) & 15;
            uint8_t *p = pixels + ((size_t)y * *width + x) * 4;
            if (indexed) {
                unsigned index = t->siz == 0 ? t->palette * 16 + nibble : b0;
                unsigned k = 2048 + index * 8;
                pixel16(byte(s, k) * 256 + byte(s, k + 1), p, tlut == 3);
            } else if (t->siz == 0) {
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
#ifdef RECOMP_GLES2_TEXTURE_PACKS
    entry->has_rt64_id = has_rt64_id;
    entry->rt64_id = rt64_id;
    if (s->editor && has_rt64_id)
        gles2_editor_observe(s->editor, rt64_id, *width, *height, pixels);
#endif
    if (!entry->id)
        s->gl.GenTextures(1, &entry->id);
    s->gl.BindTexture(GL_TEXTURE_2D, entry->id);
    s->gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)*width, (GLsizei)*height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     pixels);
    if (s->mipmaps)
        s->gl.GenerateMipmap(GL_TEXTURE_2D);
    free(pixels);
    s->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    s->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
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
        for (unsigned i = 0; i < bytes; i++) {
            unsigned word = i / 8, row = (word * th) >> 11;
            unsigned offset = t->tmem * 8 + i + row * t->line * 8;
            if (t->siz == 3) {
                unsigned pixel = i / 4, part = i & 3;
                offset = t->tmem * 8 + pixel * 2 + (part & 1) + row * t->line * 8;
                offset = (offset & 2047) | (part >= 2 ? 2048 : 0);
            }
            offset ^= (row & 1) ? 4 : 0;
            tmem_store(s, offset, valid, valid ? load(s, src + i, 1) : 0);
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
    s->texture_pack = gles2_texture_pack_open(getenv("RECOMP_TEXTURE_PACK"), s->texture_overrides, title);
    s->rice_active = !s->original_textures && gles2_texture_pack_has_rice(s->texture_pack);
    s->rice_load = 0;
    memset(s->rice_words, 0, sizeof(s->rice_words));
    for (unsigned i = 0; i < sizeof(s->textures) / sizeof(*s->textures); ++i) {
        if (s->textures[i].id)
            s->gl.DeleteTextures(1, &s->textures[i].id);
        s->textures[i] = (Texture){0};
    }
    recomp_gles2_clear_replacements(s);
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
    recomp_gles2_reload_textures(s);
}
#endif
