#define _POSIX_C_SOURCE 200809L
#include "runtime.h"
#include "runtime_debug.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct RecompIO {
    RecompPad live[4], pending[4], result[4], query_result[4];
    bool initialized, published;
    uint8_t max_channels;
    uint32_t eeprom_size;
    uint64_t si_transfer_ticks, si_deadline;
    uint8_t eeprom[2048];
    char *path;
    RecompIOStats stats;
};
static RecompIO *io(Runtime *rt) {
    if (!rt->io) recomp_fail("native controller/EEPROM services not initialized", 0);
    return rt->io;
}
static uint32_t arg(MipsContext *ctx, unsigned n) { return (uint32_t)ctx->gpr[4+n]; }
static void result(MipsContext *ctx, int32_t value) { mips_set(ctx, 2, mips_s32((uint32_t)value)); }

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
    if (rt->io || !rt->scheduler || !config || config->connected_mask > 15
            || (config->eeprom_size != 0 && config->eeprom_size != 512 && config->eeprom_size != 2048)
            || (config->eeprom_path && (!config->eeprom_size || !*config->eeprom_path))) return false;
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
    rt->io = s; rt->destroy_io = recomp_io_destroy; rt->poll_io = poll_io; return true;
}
void recomp_io_destroy(Runtime *rt) {
    if (!rt->io) return;
    free(rt->io->path); free(rt->io); rt->io = NULL; rt->destroy_io = NULL; rt->poll_io = NULL;
}
bool recomp_input_set(Runtime *rt, unsigned port, const RecompPad *pad) {
    if (port >= 4 || !pad) return false;
    io(rt)->live[port] = *pad; return true;
}
RecompIOStats recomp_io_stats(Runtime *rt) { return io(rt)->stats; }

void hook_os_pfs_init_pak(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    RECOMP_LOG("save", "Controller Pak init port=%" PRIu32 " result=PFS_ERR_NOPACK", arg(ctx, 2));
    (void)io(rt);
    /* RecompIO exposes controllers without a Controller Pak. */
    result(ctx, 2); /* PFS_ERR_NOPACK */
}
void hook_os_pfs_is_plug(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    (void)io(rt);
    recomp_store(rt, arg(ctx, 1), 0, 1); /* No port has an accessory. */
    result(ctx, 0);
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
    result(ctx,2); /* PFS_ERR_NOPACK: no Rumble Pak is configured. */
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
        recomp_store(rt, status+n*4+2, 0, 1); /* No controller accessory. */
        recomp_store(rt, status+n*4+3, connected ? 0 : 8, 1);
    }
    recomp_store(rt, pattern, mask, 1); s->initialized = true;
    /* SDK initialization consumes its own SI notifications; none are left for the caller. */
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
        recomp_store(rt,status+n*4+2,0,1); /* No accessory installed. */
        recomp_store(rt,status+n*4+3,pads[n].connected?0:8,1);
    }
}
void hook_os_eeprom_probe(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    RecompIO *s = io(rt); recomp_ram_address(rt, arg(ctx, 0), RECOMP_QUEUE_BYTES);
    result(ctx, s->eeprom_size == 2048 ? 2 : s->eeprom_size == 512 ? 1 : 0);
    RECOMP_LOG("save", "EEPROM probe bytes=%" PRIu32 " result=%" PRId32, s->eeprom_size, (int32_t)ctx->gpr[2]);
}
static bool persist(RecompIO *s, const uint8_t *bytes) {
    if (!s->path) return true;
    size_t length = strlen(s->path) + 16; char *temp = malloc(length); if (!temp) return false;
    snprintf(temp, length, "%s.tmp.XXXXXX", s->path);
    int fd = mkstemp(temp); bool okay = fd >= 0;
    if (okay) {
        size_t written = 0;
        while (written < s->eeprom_size) {
            ssize_t count = write(fd, bytes+written, s->eeprom_size-written);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) { okay = false; break; }
            written += (size_t)count;
        }
        if (okay && fsync(fd)) okay = false;
        if (close(fd)) okay = false;
        if (okay && rename(temp, s->path)) okay = false;
    }
    if (!okay && fd >= 0) unlink(temp);
    RECOMP_LOG("save", "EEPROM persist path=%s bytes=%" PRIu32 " result=%s", s->path, s->eeprom_size, okay?"ok":"failed");
    free(temp); return okay;
}
static void transfer_eeprom(Runtime *rt, MipsContext *ctx, bool write_data) {
    RecompIO *s = io(rt); recomp_ram_address(rt, arg(ctx, 0), RECOMP_QUEUE_BYTES);
    uint32_t block = (uint8_t)arg(ctx, 1), address = arg(ctx, 2), count = arg(ctx, 3);
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
        if (!persist(s, next)) {
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
    transfer_eeprom(rt, ctx, false);
}
void hook_os_eeprom_long_write(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    transfer_eeprom(rt, ctx, true);
}
