#define _POSIX_C_SOURCE 200809L
#include "runtime.h"
#include "runtime_debug.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { PAK_SIZE = 32768, PAK_BLOCK_SIZE = 32, PFS_ERR_NOPACK = 1,
       PFS_ERR_INCONSISTENT = 3, PFS_ERR_CONTRFAIL = 4, PFS_ERR_INVALID = 5,
       PFS_ERR_ID_FATAL = 10, PFS_ERR_DEVICE = 11 };
typedef struct ControllerPak {
    uint8_t bytes[PAK_SIZE], bank;
    char *path;
} ControllerPak;

struct RecompIO {
    RecompPad live[4], pending[4], result[4], query_result[4];
    bool initialized, published;
    uint8_t max_channels;
    uint32_t eeprom_size;
    uint64_t si_transfer_ticks, si_deadline;
    uint8_t eeprom[2048];
    char *path;
    ControllerPak *paks[4];
    RecompIOStats stats;
};
static RecompIO *io(Runtime *rt) {
    if (!rt->io) recomp_fail("native controller/EEPROM services not initialized", 0);
    return rt->io;
}
static uint32_t arg(MipsContext *ctx, unsigned n) { return (uint32_t)ctx->gpr[4+n]; }
static void result(MipsContext *ctx, int32_t value) { mips_set(ctx, 2, mips_s32((uint32_t)value)); }

static bool persist_image(const char *path, const uint8_t *bytes, size_t size);
static unsigned pak_u16(const uint8_t *p) { return (unsigned)p[0] * 256 + p[1]; }
static void pak_set_u16(uint8_t *p, unsigned value) { p[0] = (uint8_t)(value >> 8); p[1] = (uint8_t)value; }
static void pak_id_checksum(const uint8_t *id, uint16_t *sum, uint16_t *inverse) {
    *sum = *inverse = 0;
    for (unsigned i = 0; i < 28; i += 2) {
        unsigned word = pak_u16(id + i);
        *sum += word;
        *inverse += (uint16_t)~word;
    }
}
static void pak_format(ControllerPak *pak, unsigned port) {
    uint8_t *id = pak->bytes + 32;
    /* A stable identity per port; the checksummed ID is mirrored four times. */
    id[7] = (uint8_t)(port + 1);
    pak_set_u16(id + 24, 1);
    id[26] = 1;
    uint16_t sum, inverse;
    pak_id_checksum(id, &sum, &inverse);
    pak_set_u16(id + 28, sum); pak_set_u16(id + 30, inverse);
    memcpy(pak->bytes + 96, id, 32);
    memcpy(pak->bytes + 128, id, 32);
    memcpy(pak->bytes + 192, id, 32);
    for (unsigned page = 5; page < 128; page++) pak_set_u16(pak->bytes + 256 + page * 2, 3);
    pak->bytes[257] = (123 * 3) & 255;
    memcpy(pak->bytes + 512, pak->bytes + 256, 256);
}
static bool pak_present(RecompIO *s, unsigned port) {
    return port < 4 && s->live[port].connected && s->paks[port];
}
static bool pak_filesystem_valid(const uint8_t *bytes) {
    const uint8_t *inode = bytes + 256;
    unsigned sum = 0;
    for (unsigned n = 10; n < 256; n++) sum += inode[n];
    if ((sum & 255) != inode[1] || memcmp(inode, bytes + 512, 256)) return false;
    bool used[128] = {false};
    for (unsigned slot = 0; slot < 16; slot++) {
        const uint8_t *note = bytes + 768 + slot * 32;
        bool game = note[0] || note[1] || note[2] || note[3];
        bool company = note[4] || note[5];
        if (!game && !company) continue;
        if (!game || !company) return false;
        unsigned page = pak_u16(note + 6);
        do {
            if (page < 5 || page >= 128 || used[page]) return false;
            used[page] = true;
            page = pak_u16(inode + page * 2);
        } while (page != 1);
    }
    for (unsigned page = 5; page < 128; page++)
        if (!used[page] && pak_u16(inode + page * 2) != 3) return false;
    return true;
}

static void poll_io(Runtime *rt) {
    RecompIO *s = io(rt);
    if (!s->stats.read_pending && !s->stats.query_pending) return;
    if (s->si_transfer_ticks && recomp_count64(rt) < s->si_deadline) return;
    if (!s->published) {
        memcpy(s->stats.query_pending?s->query_result:s->result,s->pending,sizeof(s->result));
        s->published = true;
        if(s->stats.read_pending)s->stats.reads_completed++;else s->stats.queries_completed++;
    }
    if (recomp_event_post(rt, 5)) {
        s->stats.si_events++; s->stats.read_pending = false; s->stats.query_pending = false;
    }
}

bool recomp_io_init(Runtime *rt, const RecompIOConfig *config) {
    if (rt->io || !rt->scheduler || !config || config->connected_mask > 15 || config->pak_mask > 15
            || (config->eeprom_size != 0 && config->eeprom_size != 512 && config->eeprom_size != 2048)
            || (config->eeprom_path && (!config->eeprom_size || !*config->eeprom_path))) return false;
    for (unsigned n = 0; n < 4; n++) {
        if (!config->pak_paths[n]) continue;
        if (!(config->pak_mask & (1u << n)) || !*config->pak_paths[n]
                || (config->eeprom_path && !strcmp(config->pak_paths[n], config->eeprom_path))) return false;
        for (unsigned j = 0; j < n; j++)
            if (config->pak_paths[j] && !strcmp(config->pak_paths[n], config->pak_paths[j])) return false;
    }
    RecompIO *s = calloc(1, sizeof(*s)); if (!s) return false;
    s->max_channels = 4;
    s->si_transfer_ticks = config->si_transfer_ticks;
    s->eeprom_size = config->eeprom_size; memset(s->eeprom, 0xff, sizeof(s->eeprom));
    for (unsigned n=0;n<4;n++) s->live[n].connected = (config->connected_mask & (1u<<n)) != 0;
    if (config->eeprom_path) {
        s->path = strdup(config->eeprom_path);
        if (!s->path) { free(s); return false; }
        FILE *file = fopen(s->path, "rb");
        if (file) {
            bool valid = fread(s->eeprom, 1, s->eeprom_size, file) == s->eeprom_size
                      && fgetc(file) == EOF && !ferror(file);
            if (fclose(file)) valid = false;
            RECOMP_LOG("save", "EEPROM load path=%s bytes=%" PRIu32 " result=%s", s->path, s->eeprom_size, valid?"ok":"invalid image/read failure");
            if (!valid) { free(s->path); free(s); return false; }
        } else if (errno != ENOENT) {
            RECOMP_LOG("save", "EEPROM load path=%s failed: %s", s->path, strerror(errno));
            free(s->path); free(s); return false;
        } else RECOMP_LOG("save", "EEPROM new image path=%s bytes=%" PRIu32 " initial=0xff", s->path, s->eeprom_size);
    }
    RECOMP_LOG("input", "controller mask=0x%x EEPROM bytes=%" PRIu32 " storage=%s SI delay=%" PRIu64 " ticks",
        config->connected_mask, s->eeprom_size, s->path?s->path:"memory", s->si_transfer_ticks);
    rt->io = s; rt->destroy_io = recomp_io_destroy; rt->poll_io = poll_io;
    for (unsigned n = 0; n < 4; n++) {
        if (!(config->pak_mask & (1u << n))) continue;
        ControllerPak *pak = s->paks[n] = calloc(1, sizeof(*pak));
        if (!pak) { recomp_io_destroy(rt); return false; }
        pak_format(pak, n);
        if (!config->pak_paths[n]) continue;
        pak->path = strdup(config->pak_paths[n]);
        if (!pak->path) { recomp_io_destroy(rt); return false; }
        FILE *file = fopen(pak->path, "rb");
        bool valid;
        if (file) {
            valid = fread(pak->bytes, 1, PAK_SIZE, file) == PAK_SIZE && fgetc(file) == EOF && !ferror(file);
            if (fclose(file)) valid = false;
        } else {
            valid = errno == ENOENT && persist_image(pak->path, pak->bytes, PAK_SIZE);
        }
        RECOMP_LOG("save", "Controller Pak port=%u path=%s result=%s", n, pak->path, valid?"ok":"load/create failed");
        if (!valid) { recomp_io_destroy(rt); return false; }
    }
    return true;
}
void recomp_io_destroy(Runtime *rt) {
    if (!rt->io) return;
    for (unsigned n = 0; n < 4; n++) {
        if (!rt->io->paks[n]) continue;
        free(rt->io->paks[n]->path); free(rt->io->paks[n]);
    }
    free(rt->io->path); free(rt->io); rt->io = NULL; rt->destroy_io = NULL; rt->poll_io = NULL;
}
bool recomp_input_set(Runtime *rt, unsigned port, const RecompPad *pad) {
    if (port >= 4 || !pad) return false;
    io(rt)->live[port] = *pad; return true;
}
RecompIOStats recomp_io_stats(Runtime *rt) { return io(rt)->stats; }

void hook_os_pfs_init_pak(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    RecompIO *s = io(rt);
    uint32_t pfs = arg(ctx, 1), port = arg(ctx, 2);
    if (port >= 4) { result(ctx, PFS_ERR_INVALID); return; }
    if (!pak_present(s, port)) { result(ctx, PFS_ERR_NOPACK); return; }
    recomp_ram_address(rt, arg(ctx, 0), RECOMP_QUEUE_BYTES);
    recomp_ram_address(rt, pfs, 0x68);
    recomp_store(rt, pfs, 0, 4);
    recomp_store(rt, pfs + 4, arg(ctx, 0), 4);
    recomp_store(rt, pfs + 8, port, 4);
    ControllerPak *pak = s->paks[port];
    const uint8_t *id = pak->bytes + 32;
    uint16_t sum, inverse;
    pak_id_checksum(id, &sum, &inverse);
    if (sum != pak_u16(id + 28) || inverse != pak_u16(id + 30)) {
        recomp_store(rt, pfs, 4, 4); /* PFS_ID_BROKEN; preserve the on-disk image. */
        result(ctx, PFS_ERR_ID_FATAL); return;
    }
    if (!(pak_u16(id + 24) & 1) || id[26] != 1) { result(ctx, PFS_ERR_DEVICE); return; }
    /* Report damaged filesystems without silently repairing or discarding saves. */
    if (!pak_filesystem_valid(pak->bytes)) { result(ctx, PFS_ERR_INCONSISTENT); return; }
    recomp_copy_to_ram(rt, pfs + 12, id, 32);
    recomp_copy_to_ram(rt, pfs + 44, pak->bytes + 224, 32);
    recomp_store(rt, pfs + 0x4c, id[27], 4);
    recomp_store(rt, pfs + 0x50, 16, 4);
    recomp_store(rt, pfs + 0x54, 8, 4);
    recomp_store(rt, pfs + 0x58, 16, 4);
    recomp_store(rt, pfs + 0x5c, 24, 4);
    recomp_store(rt, pfs + 0x60, 5, 4);
    recomp_store(rt, pfs + 0x64, 1, 1);
    recomp_store(rt, pfs + 0x65, 0, 1);
    pak->bank = 0;
    recomp_store(rt, pfs, 1, 4); /* PFS_INITIALIZED */
    RECOMP_LOG("save", "Controller Pak init port=%" PRIu32 " result=ok storage=%s", port, pak->path?pak->path:"memory");
    result(ctx, 0);
}
void hook_os_pfs_is_plug(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    RecompIO *s = io(rt);
    uint8_t mask = 0;
    for (unsigned n = 0; n < 4; n++) if (pak_present(s, n)) mask |= (uint8_t)(1u << n);
    recomp_store(rt, arg(ctx, 1), mask, 1);
    result(ctx, 0);
}
void hook_os_pfs_get_status(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    unsigned port = arg(ctx, 1);
    result(ctx, port >= 4 ? PFS_ERR_INVALID : pak_present(io(rt), port) ? 0 : PFS_ERR_NOPACK);
}
static void transfer_pak(Runtime *rt, MipsContext *ctx, bool write_data) {
    RecompIO *s = io(rt);
    unsigned port = arg(ctx, 1), block = (uint16_t)arg(ctx, 2);
    if (port >= 4) { result(ctx, PFS_ERR_INVALID); return; }
    if (!pak_present(s, port)) { result(ctx, PFS_ERR_NOPACK); return; }
    recomp_ram_address(rt, arg(ctx, 0), RECOMP_QUEUE_BYTES);
    uint32_t address = arg(ctx, 3);
    ControllerPak *pak = s->paks[port];
    if (block > PAK_SIZE / PAK_BLOCK_SIZE) { result(ctx, PFS_ERR_INVALID); return; }
    recomp_ram_address(rt, address, PAK_BLOCK_SIZE);
    if (block == PAK_SIZE / PAK_BLOCK_SIZE) {
        /* 0x8000 is the bank-select register, outside the save image. */
        if (write_data) pak->bank = (uint8_t)recomp_load(rt, address, 1);
        else for (unsigned n = 0; n < PAK_BLOCK_SIZE; n++) recomp_store(rt, address + n, pak->bank, 1);
        result(ctx, 0); return;
    }
    if (pak->bank) { result(ctx, PFS_ERR_DEVICE); return; }
    if (write_data) {
        int force = (int32_t)recomp_load(rt, (uint32_t)ctx->gpr[29] + 16, 4);
        if (force != 1 && block > 0 && block < 7) { result(ctx, 0); return; }
        uint8_t next[PAK_SIZE];
        memcpy(next, pak->bytes, sizeof(next));
        for (unsigned n = 0; n < PAK_BLOCK_SIZE; n++) next[block * PAK_BLOCK_SIZE + n] = (uint8_t)recomp_load(rt, address + n, 1);
        if (!persist_image(pak->path, next, sizeof(next))) {
            s->stats.persistence_failures++; result(ctx, PFS_ERR_CONTRFAIL); return;
        }
        memcpy(pak->bytes, next, sizeof(next)); s->stats.pak_writes++;
    } else {
        recomp_copy_to_ram(rt, address, pak->bytes + block * PAK_BLOCK_SIZE, PAK_BLOCK_SIZE);
        s->stats.pak_reads++;
    }
    result(ctx, 0);
}
void hook_os_cont_ram_read(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    transfer_pak(rt, ctx, false);
}
void hook_os_cont_ram_write(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    transfer_pak(rt, ctx, true);
}
void hook_os_cont_set_ch(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    unsigned channels = (uint8_t)arg(ctx, 0);
    io(rt)->max_channels = channels > 4 ? 4 : (uint8_t)channels;
    result(ctx, 0);
}
void hook_os_motor_init(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    (void)io(rt);
    uint32_t pfs=arg(ctx,1), channel=arg(ctx,2);
    if(channel>=4) { result(ctx,5); return; } /* PFS_ERR_INVALID */
    recomp_ram_address(rt,pfs,0x68);
    recomp_store(rt,pfs,0,4);
    recomp_store(rt,pfs+4,arg(ctx,0),4);
    recomp_store(rt,pfs+8,channel,4);
    recomp_store(rt,pfs+0x65,0x80,1);
    result(ctx, pak_present(io(rt), channel) ? PFS_ERR_DEVICE : PFS_ERR_NOPACK);
}
void hook_os_motor_start(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    (void)io(rt);
    result(ctx,5); /* PFS_ERR_INVALID: osMotorInit could not initialize a Pak. */
}
void hook_os_motor_stop(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    hook_os_motor_start(rt,ctx);
}

void hook_os_cont_init(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    RecompIO *s = io(rt);
    if (s->initialized) { result(ctx, 0); return; }
    uint32_t pattern = arg(ctx, 1), status = arg(ctx, 2); uint8_t mask = 0;
    recomp_ram_address(rt, arg(ctx, 0), RECOMP_QUEUE_BYTES);
    recomp_ram_address(rt, pattern, 1); recomp_ram_address(rt, status, 16);
    for (unsigned n=0;n<4;n++) {
        bool connected = s->live[n].connected;
        if (connected) mask |= (uint8_t)(1u<<n);
        recomp_store(rt, status+n*4, connected ? 5 : 0, 2);
        recomp_store(rt, status+n*4+2, pak_present(s, n) ? 1 : 0, 1);
        recomp_store(rt, status+n*4+3, connected ? 0 : 8, 1);
    }
    recomp_store(rt, pattern, mask, 1); s->initialized = true;
    /* SDK initialization consumes its own SI notifications; none are left for the caller. */
    result(ctx, 0);
}
void hook_os_cont_reset(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    RecompIO *s = io(rt);
    if (!s->initialized) recomp_fail("controller reset before osContInit", ctx->pc);
    uint32_t status = arg(ctx, 1);
    recomp_ram_address(rt, arg(ctx, 0), RECOMP_QUEUE_BYTES);
    /* Finish an earlier asynchronous transfer without consuming its event or
     * discarding its input snapshot. Guest execution is serialized once idle. */
    while (s->stats.read_pending || s->stats.query_pending) recomp_thread_yield(rt);
    if (s->max_channels) recomp_ram_address(rt, status, s->max_channels * 4);
    for (unsigned n = 0; n < s->max_channels; n++) {
        bool connected = s->live[n].connected;
        recomp_store(rt, status + n * 4 + 3, connected ? 0 : 8, 1);
        if (connected) {
            recomp_store(rt, status + n * 4, 5, 2);
            recomp_store(rt, status + n * 4 + 2, pak_present(s, n) ? 1 : 0, 1);
        }
    }
    /* osContReset waits for and consumes all of its own SI notifications. */
    result(ctx, 0);
}
void hook_os_cont_start_read(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    RecompIO *s = io(rt);
    if (!s->initialized) recomp_fail("controller read before osContInit", ctx->pc);
    recomp_ram_address(rt, arg(ctx, 0), RECOMP_QUEUE_BYTES);
    if (s->stats.read_pending || s->stats.query_pending) { result(ctx, -1); return; }
    memcpy(s->pending, s->live, sizeof(s->pending)); s->published = false;
    if (s->si_transfer_ticks) {
        uint64_t now = recomp_count64(rt);
        if (s->si_transfer_ticks > UINT64_MAX-now) recomp_fail("SI schedule exhausted", ctx->pc);
        s->si_deadline = now+s->si_transfer_ticks;
    }
    s->stats.read_pending = true; s->stats.reads_started++; result(ctx, 0);
}
void hook_os_cont_get_read(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    RecompIO *s = io(rt);
    if (!s->initialized) recomp_fail("controller result before osContInit", ctx->pc);
    uint32_t pads = arg(ctx, 0);
    if (s->max_channels) recomp_ram_address(rt, pads, s->max_channels * 6);
    for (unsigned n=0;n<s->max_channels;n++) {
        const RecompPad *pad = &s->result[n];
        recomp_store(rt, pads+n*6+4, pad->connected ? 0 : 8, 1);
        if (pad->connected) {
            recomp_store(rt, pads+n*6, pad->buttons, 2);
            recomp_store(rt, pads+n*6+2, (uint8_t)pad->stick_x, 1);
            recomp_store(rt, pads+n*6+3, (uint8_t)pad->stick_y, 1);
        }
    }
}
void hook_os_cont_start_query(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    RecompIO *s=io(rt);
    if(!s->initialized)recomp_fail("controller query before osContInit",ctx->pc);
    recomp_ram_address(rt,arg(ctx,0),RECOMP_QUEUE_BYTES);
    if(s->stats.read_pending || s->stats.query_pending){result(ctx,-1);return;}
    memcpy(s->pending,s->live,sizeof(s->pending));s->published=false;
    if (s->si_transfer_ticks) {
        uint64_t now = recomp_count64(rt);
        if (s->si_transfer_ticks > UINT64_MAX-now) recomp_fail("SI schedule exhausted", ctx->pc);
        s->si_deadline = now+s->si_transfer_ticks;
    }
    s->stats.query_pending=true;s->stats.queries_started++;result(ctx,0);
}
void hook_os_cont_get_query(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    RecompIO *s=io(rt);
    if(!s->initialized)recomp_fail("controller status before osContInit",ctx->pc);
    uint32_t status=arg(ctx,0);
    if(s->max_channels)recomp_ram_address(rt,status,s->max_channels*4);
    const RecompPad *pads=s->stats.queries_completed?s->query_result:s->live;
    for(unsigned n=0;n<s->max_channels;n++) {
        recomp_store(rt,status+n*4,pads[n].connected?5:0,2);
        recomp_store(rt,status+n*4+2,pads[n].connected && s->paks[n] ? 1 : 0,1);
        recomp_store(rt,status+n*4+3,pads[n].connected?0:8,1);
    }
}
void hook_os_eeprom_probe(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    RecompIO *s = io(rt); recomp_ram_address(rt, arg(ctx, 0), RECOMP_QUEUE_BYTES);
    result(ctx, s->eeprom_size == 2048 ? 2 : s->eeprom_size == 512 ? 1 : 0);
    RECOMP_LOG("save", "EEPROM probe bytes=%" PRIu32 " result=%" PRId32, s->eeprom_size, (int32_t)ctx->gpr[2]);
}
static bool persist_image(const char *path, const uint8_t *bytes, size_t size) {
    if (!path) return true;
    size_t length = strlen(path) + 16; char *temp = malloc(length); if (!temp) return false;
    snprintf(temp, length, "%s.tmp.XXXXXX", path);
    int fd = mkstemp(temp); bool okay = fd >= 0;
    if (okay) {
        size_t written = 0;
        while (written < size) {
            ssize_t count = write(fd, bytes+written, size-written);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) { okay = false; break; }
            written += (size_t)count;
        }
        if (okay && fsync(fd)) okay = false;
        if (close(fd)) okay = false;
        if (okay && rename(temp, path)) okay = false;
    }
    if (!okay && fd >= 0) unlink(temp);
    RECOMP_LOG("save", "save persist path=%s bytes=%zu result=%s", path, size, okay?"ok":"failed");
    free(temp); return okay;
}
static void transfer_eeprom(Runtime *rt, MipsContext *ctx, bool write_data, uint32_t count) {
    RecompIO *s = io(rt); recomp_ram_address(rt, arg(ctx, 0), RECOMP_QUEUE_BYTES);
    uint32_t block = (uint8_t)arg(ctx, 1), address = arg(ctx, 2);
    RECOMP_LOG("save", "EEPROM %s block=%" PRIu32 " offset=0x%04" PRIx32 " ram=0x%08" PRIx32
        " bytes=%" PRIu32 " path=%s", write_data?"write":"read", block, block*8, address, count, s->path?s->path:"memory");
    if (!s->eeprom_size || s->stats.read_pending || s->stats.query_pending) {
        RECOMP_LOG("save", "EEPROM transfer rejected: absent device or SI busy result=8");
        result(ctx, 8); return;
    }
    if (count % 8 || block*8 > s->eeprom_size || count > s->eeprom_size-block*8) {
        RECOMP_LOG("save", "EEPROM transfer rejected: invalid range/alignment result=-1");
        result(ctx, -1); return;
    }
    if (!count) { result(ctx, 0); return; }
    recomp_ram_address(rt, address, count);
    if (write_data) {
        uint8_t next[2048]; memcpy(next, s->eeprom, sizeof(next));
        for (uint32_t n=0;n<count;n++) next[block*8+n] = (uint8_t)recomp_load(rt, address+n, 1);
        if (!persist_image(s->path, next, s->eeprom_size)) {
            RECOMP_LOG("save", "EEPROM write failed: persistence failure result=-1");
            s->stats.persistence_failures++; result(ctx, -1); return;
        }
        memcpy(s->eeprom, next, sizeof(next)); s->stats.eeprom_writes++;
    } else {
        recomp_copy_to_ram(rt, address, s->eeprom+block*8, count); s->stats.eeprom_reads++;
    }
    /* These blocking SDK calls consume their internal SI/timer notifications. */
    result(ctx, 0);
    RECOMP_LOG("save", "EEPROM %s complete result=0", write_data?"write":"read");
}
void hook_os_eeprom_long_read(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    transfer_eeprom(rt, ctx, false, arg(ctx, 3));
}
void hook_os_eeprom_long_write(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    transfer_eeprom(rt, ctx, true, arg(ctx, 3));
}
void hook_os_eeprom_read(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    transfer_eeprom(rt, ctx, false, 8);
}
void hook_os_eeprom_write(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    transfer_eeprom(rt, ctx, true, 8);
}
