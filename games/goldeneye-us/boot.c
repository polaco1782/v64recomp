/* Bounded ROM-derived startup check. The main gameplay thread never runs here. */
#include "runtime.h"
#include "generated.h"
#include "goldeneye_boot_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boot_hooks.h"

static uint64_t fixed_time(void *opaque) { (void)opaque; return 0; }

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s NORMALIZED_ROM DATA_CAPTURE BOOT_REPORT\n", argv[0]);
        return 2;
    }
    data_output = argv[2];
    FILE *stream = fopen(argv[1], "rb");
    require(stream != NULL, "cannot open GoldenEye ROM", 0);
    uint8_t *rom = malloc(BOOT_ROM_SIZE);
    require(rom != NULL, "cannot allocate GoldenEye ROM", 0);
    bool read = fread(rom, 1, BOOT_ROM_SIZE, stream) == BOOT_ROM_SIZE;
    int extra = fgetc(stream), closed = fclose(stream);
    require(read && extra == EOF && closed == 0, "GoldenEye ROM size/read mismatch", 0);

    Runtime rt;
    require(recomp_runtime_init(&rt, BOOT_RAM_SIZE, recomp_functions, recomp_function_count,
                               recomp_boot_sections, recomp_section_count), "GoldenEye runtime init failed", 0);
    require(recomp_rom_attach(&rt, rom, BOOT_ROM_SIZE, boot_sha1), "GoldenEye ROM identity mismatch", 0);
    recomp_copy_to_ram(&rt, BOOT_INITIAL_LOAD_ADDRESS, rom + BOOT_INITIAL_ROM_OFFSET, BOOT_INITIAL_LOAD_SIZE);
    /* Poison BSS: successfully booting from calloc-zeroed RAM would miss a
     * broken translated clear loop. Guards check writes stay within its range. */
    for (uint32_t a = BOOT_BSS_START - 4; a < BOOT_BSS_END + 4; a += 4)
        recomp_store(&rt, a, 0xa5a5a5a5, 4);
    for (size_t i = 0; i < sizeof(boot_parameters) / sizeof(boot_parameters[0]); i++)
        recomp_store(&rt, boot_parameters[i][0], boot_parameters[i][1], 4);
    RecompClockConfig clock = {.ticks_per_second=BOOT_COUNTER_HZ, .now_ns=fixed_time};
    RecompPlatformConfig platform = {.vi_period_ticks=BOOT_VI_PERIOD_TICKS, .app_nmi_buffer=0x8000031c};
    /* Host test bookkeeping lives in unused high RAM, outside original data,
     * BSS and stacks. It is not a GoldenEye OS-global address mapping. */
    RecompSchedulerConfig scheduler = {.thread_tail=0x803fff80, .stop_before_entry=BOOT_MAIN_ENTRY};
    require(recomp_clock_init(&rt, &clock) && recomp_scheduler_init(&rt, &scheduler)
            && recomp_platform_init(&rt, &platform), "GoldenEye services init failed", 0);
    recomp_thread_create(&rt, 0x803ff000, 0, BOOT_ENTRY, 0, 0x803ab410, 255);
    recomp_thread_start(&rt, 0x803ff000);
    RecompRunResult run = recomp_scheduler_run(&rt, 65536);
    require(run == RECOMP_RUN_STOPPED && captured_data && entered_inflate && cleared_bss && prepared_context,
            "GoldenEye did not reach the main-thread boot boundary", BOOT_MAIN_ENTRY);
    const MipsContext *main_context = recomp_thread_context(&rt, BOOT_MAIN_THREAD);
    require(main_context->pc == BOOT_MAIN_ENTRY && recomp_thread_id(&rt, BOOT_MAIN_THREAD) == 3
            && recomp_thread_priority(&rt, BOOT_MAIN_THREAD) == 10,
            "GoldenEye main thread ABI mismatch", BOOT_MAIN_THREAD);
    require(recomp_load(&rt, BOOT_BSS_END, 4) == 0xa5a5a5a5, "GoldenEye BSS clear overran end", BOOT_BSS_END);

    stream = fopen(argv[3], "w");
    require(stream != NULL, "cannot open GoldenEye boot report", 0);
    int written = fprintf(stream,
        "{\"main_thread\":%u,\"main_entry\":%u,\"main_id\":3,\"main_priority\":10,"
        "\"main_stack\":%u,\"main_state\":\"stopped_before_entry\","
        "\"bss_cleared\":true,\"inflater_relocation_verified\":true,"
        "\"data_bytes\":%u,\"registered_functions\":%zu,\"gameplay_executed\":false}\n",
        BOOT_MAIN_THREAD, BOOT_MAIN_ENTRY, (uint32_t)main_context->gpr[29], BOOT_DATA_SIZE, recomp_function_count);
    closed = fclose(stream);
    require(written > 0 && closed == 0, "cannot write GoldenEye boot report", 0);
    puts("GoldenEye native bootstrap reached the original main thread (stopped before gameplay).");
    recomp_runtime_destroy(&rt);
    free(rom);
    return 0;
}
