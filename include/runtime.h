#ifndef RECOMP_RUNTIME_H
#define RECOMP_RUNTIME_H

#include "recomp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* N64 libultra o32 guest layouts; no host pointers are written to guest RAM. */
enum { RECOMP_THREAD_BYTES = 0x1b0, RECOMP_QUEUE_BYTES = 24,
       RECOMP_MESG_NOBLOCK = 0, RECOMP_MESG_BLOCK = 1,
       RECOMP_OS_STOPPED = 1, RECOMP_OS_RUNNABLE = 2,
       RECOMP_OS_RUNNING = 4, RECOMP_OS_WAITING = 8, RECOMP_OS_EVENTS = 15 };

typedef enum RecompThreadState {
    RECOMP_STOPPED, RECOMP_READY, RECOMP_RUNNING, RECOMP_WAIT_SEND,
    RECOMP_WAIT_RECV, RECOMP_DEAD
} RecompThreadState;
typedef enum RecompRunResult {
    RECOMP_RUN_COMPLETE, RECOMP_RUN_WAITING, RECOMP_RUN_STOPPED, RECOMP_RUN_BUDGET
} RecompRunResult;
typedef struct RecompThreadSnapshot {
    uint32_t object;
    int32_t id, priority;
    RecompThreadState state;
    bool current;
    MipsContext context;
} RecompThreadSnapshot;
/* Read-only diagnostic snapshot; also safe before scheduler initialization. */
size_t recomp_thread_snapshots(Runtime *, RecompThreadSnapshot *, size_t capacity);
/* Scheduler-owned native stack boundary for safe failure-path unwinding. */
void recomp_set_trace_ceiling(uintptr_t);

typedef struct RecompSchedulerConfig {
    uint32_t thread_tail; /* Guest address of an eight-byte tail sentinel. */
    uint32_t run_queue, active_queue, running_thread, event_table; /* Optional guest globals. */
    size_t native_stack_size; /* Zero uses minicoro's virtual-memory stack default. */
    unsigned checkpoint_interval; /* Backedges per yield; zero selects 64. */
    uint32_t stop_before_entry; /* Debugger stop before a thread's first instruction. */
    const uint32_t *stop_before_entries;
    size_t stop_before_count; /* Additional checked demo/debugger boundaries. */
    bool fpu_fr1; /* SDK uses 0x230-byte threads with 32 independent 64-bit FPRs. */
    uint32_t idle_thread; /* Optional priority-zero idle thread: return to the host instead of spinning. */
} RecompSchedulerConfig;

/* All APIs except event_post and task_post_completion run on one owner host thread. Guest code must
 * reach checkpoints/hooks to yield; max_resumes is not an instruction budget.
 * Join backend workers before destroying the runtime. */
bool recomp_scheduler_init(Runtime *, const RecompSchedulerConfig *);
void recomp_scheduler_destroy(Runtime *);
RecompRunResult recomp_scheduler_run(Runtime *, size_t max_resumes);
void recomp_thread_create(Runtime *, uint32_t object, int32_t id, uint32_t entry,
                          uint32_t argument, uint32_t stack, int32_t priority);
void recomp_thread_start(Runtime *, uint32_t object);
void recomp_thread_stop(Runtime *, uint32_t object); /* Zero selects current. */
void recomp_thread_destroy(Runtime *, uint32_t object);
void recomp_thread_yield(Runtime *);
int32_t recomp_thread_priority(Runtime *, uint32_t object);
int32_t recomp_thread_id(Runtime *, uint32_t object);
void recomp_thread_set_priority(Runtime *, uint32_t object, int32_t priority);
RecompThreadState recomp_thread_state(Runtime *, uint32_t object);
const MipsContext *recomp_thread_context(Runtime *, uint32_t object);

void recomp_queue_create(Runtime *, uint32_t object, uint32_t messages, int32_t count);
int32_t recomp_queue_send(Runtime *, uint32_t object, uint32_t message, int32_t flag, bool jam);
int32_t recomp_queue_recv(Runtime *, uint32_t object, uint32_t destination, int32_t flag);
int32_t recomp_queue_peek(Runtime *, uint32_t object, uint32_t *message);
void recomp_event_bind(Runtime *, uint32_t event, uint32_t queue, uint32_t message);
/* Only event_post may be called from backend worker threads. Returns false if
 * the bounded pending FIFO is full or the event ID is invalid. Delivery runs
 * on the scheduler's owner thread, at a run/checkpoint boundary. */
bool recomp_event_post(Runtime *, uint32_t event);

typedef uint64_t (*RecompNowNs)(void *);
typedef struct RecompClockConfig {
    uint64_t ticks_per_second;
    RecompNowNs now_ns; /* NULL uses CLOCK_MONOTONIC. */
    void *userdata;
    uint32_t timer_sentinel; /* Optional guest __osBaseTimer, 32 bytes. */
} RecompClockConfig;
bool recomp_clock_init(Runtime *, const RecompClockConfig *);
void recomp_clock_destroy(Runtime *);
uint64_t recomp_count64(Runtime *);
uint64_t recomp_get_time(Runtime *);
void recomp_set_time(Runtime *, uint64_t);
void recomp_timer_set(Runtime *, uint32_t timer, uint64_t countdown, uint64_t interval,
                      uint32_t queue, uint32_t message);
bool recomp_timer_stop(Runtime *, uint32_t timer);
uint64_t recomp_clock_next_deadline(Runtime *); /* UINT64_MAX if none. */
uint32_t recomp_clock_timer_sentinel(Runtime *);
uint64_t recomp_clock_frequency(Runtime *);

typedef struct RecompBootLoad {
    uint32_t section, vram, rom_offset, size;
} RecompBootLoad;
/* SHA-1 binds the canonical ROM. The caller retains immutable bytes until destroy. */
bool recomp_rom_attach(Runtime *, const uint8_t *, size_t, const uint8_t sha1[20]);
/* Validate the complete load plan before writing RAM or activating sections. */
bool recomp_boot_load(Runtime *, const RecompBootLoad *, size_t);

typedef struct RecompPlatformConfig {
    uint32_t clock_rate, final_rom;
    uint32_t vi_clock; /* Optional guest osViClock, used by AI frequency setup. */
    uint32_t timer_list, current_time, base_counter, vi_intr_count;
    uint32_t app_nmi_buffer;
    uint64_t vi_period_ticks;
    const RecompBootLoad *code_loads;
    size_t code_load_count;
} RecompPlatformConfig;
typedef struct RecompViState {
    bool manager_created, black;
    uint32_t mode, special_features, framebuffer, retrace_count;
    uint64_t retraces;
    uint32_t current_framebuffer; /* Pending swaps become current at a VI retrace. */
} RecompViState;
bool recomp_platform_init(Runtime *, const RecompPlatformConfig *);
void recomp_platform_destroy(Runtime *);
RecompViState recomp_vi_state(Runtime *);
typedef struct RecompViRegisters {
    uint32_t status, origin, width, intr, current_line, burst, v_sync, h_sync,
             leap, h_start, v_start, v_burst, x_scale, y_scale;
} RecompViRegisters;
/* Snapshot field-zero VI registers from the guest OSViMode. False before a mode is set. */
bool recomp_vi_registers(Runtime *, RecompViRegisters *);
typedef struct RecompAiState {
    uint32_t clock, frequency, dac_rate, bit_rate;
    bool enabled;
    uint32_t queued, address, length;
    uint64_t submissions, completed, rejected, events;
} RecompAiState;
RecompAiState recomp_ai_state(Runtime *);
/* Next VI or AI completion in counter ticks, or UINT64_MAX when inactive.
 * Query after running the scheduler; this does not itself deliver events. */
uint64_t recomp_platform_next_deadline(Runtime *);
typedef struct RecompAiOutput {
    /* Owner thread, once when each accepted DMA becomes the FIFO head.
     * Copy PCM before returning; this callback must not yield guest threads. */
    bool (*start)(Runtime *, void *, uint32_t address, uint32_t bytes, uint32_t clock, uint32_t divisor);
    void *opaque;
} RecompAiOutput;
bool recomp_ai_set_output(Runtime *, const RecompAiOutput *);
void hook_os_ai_set_frequency(Runtime *, MipsContext *);
void hook_os_ai_set_next_buffer(Runtime *, MipsContext *);
void hook_os_ai_get_length(Runtime *, MipsContext *);
void hook_os_ai_get_status(Runtime *, MipsContext *);
void hook_os_pi_get_cmd_queue(Runtime *, MipsContext *);
void hook_os_virtual_to_physical(Runtime *, MipsContext *);
void hook_os_disable_int(Runtime *, MipsContext *);
void hook_os_restore_int(Runtime *, MipsContext *);

typedef struct RecompPad {
    bool connected;
    uint16_t buttons;
    int8_t stick_x, stick_y;
} RecompPad;
typedef struct RecompIOConfig {
    uint8_t connected_mask;
    uint32_t eeprom_size; /* 0, 512 or 2048 bytes. */
    const char *eeprom_path; /* NULL keeps an in-memory device. Copied at init. */
    /* SI completion delay; zero completes at the next scheduler poll. */
    uint64_t si_transfer_ticks;
} RecompIOConfig;
typedef struct RecompIOStats {
    uint64_t reads_started, reads_completed, si_events;
    uint64_t queries_started, queries_completed;
    uint64_t eeprom_reads, eeprom_writes, persistence_failures;
    bool read_pending;
    bool query_pending;
} RecompIOStats;
/* Owner-thread input snapshots; adapters submit N64 button masks and stick values. */
bool recomp_io_init(Runtime *, const RecompIOConfig *);
void recomp_io_destroy(Runtime *);
bool recomp_input_set(Runtime *, unsigned, const RecompPad *);
RecompIOStats recomp_io_stats(Runtime *);
void hook_os_cont_init(Runtime *, MipsContext *);
void hook_os_cont_start_read(Runtime *, MipsContext *);
void hook_os_cont_get_read(Runtime *, MipsContext *);
void hook_os_cont_start_query(Runtime *, MipsContext *);
void hook_os_cont_get_query(Runtime *, MipsContext *);
void hook_os_eeprom_probe(Runtime *, MipsContext *);
void hook_os_eeprom_long_read(Runtime *, MipsContext *);
void hook_os_eeprom_long_write(Runtime *, MipsContext *);

void hook_os_initialize(Runtime *, MipsContext *);
void hook_os_get_count(Runtime *, MipsContext *);
void hook_os_get_time(Runtime *, MipsContext *);
void hook_os_set_time(Runtime *, MipsContext *);
void hook_os_set_timer(Runtime *, MipsContext *);
void hook_os_create_vi_manager(Runtime *, MipsContext *);
void hook_os_vi_set_mode(Runtime *, MipsContext *);
void hook_os_vi_black(Runtime *, MipsContext *);
void hook_os_vi_set_special_features(Runtime *, MipsContext *);
void hook_os_vi_set_event(Runtime *, MipsContext *);
void hook_os_vi_swap_buffer(Runtime *, MipsContext *);
void hook_os_vi_get_current_framebuffer(Runtime *, MipsContext *);
void hook_os_vi_get_next_framebuffer(Runtime *, MipsContext *);
void hook_os_set_int_mask(Runtime *, MipsContext *);
void hook_os_pfs_init_pak(Runtime *, MipsContext *);
void hook_os_pfs_is_plug(Runtime *, MipsContext *);
void hook_os_cont_set_ch(Runtime *, MipsContext *);
void hook_os_motor_init(Runtime *, MipsContext *);
void hook_os_motor_start(Runtime *, MipsContext *);
void hook_os_motor_stop(Runtime *, MipsContext *);
void hook_os_create_pi_manager(Runtime *, MipsContext *);
void hook_os_pi_start_dma(Runtime *, MipsContext *);
void hook_os_pi_raw_read_io(Runtime *, MipsContext *);

void hook_os_memcpy(Runtime *, MipsContext *);
void hook_os_memset(Runtime *, MipsContext *);
void hook_os_bcopy(Runtime *, MipsContext *);
void hook_os_bzero(Runtime *, MipsContext *);
void hook_os_bcmp(Runtime *, MipsContext *);
void hook_os_strlen(Runtime *, MipsContext *);
void hook_os_strchr(Runtime *, MipsContext *);

typedef enum RecompTaskCompletion { RECOMP_SP_DONE, RECOMP_SP_YIELDED, RECOMP_DP_DONE } RecompTaskCompletion;
enum { RECOMP_TASK_YIELDED = 1, RECOMP_TASK_DP_WAIT = 2, RECOMP_TASK_LOADABLE = 4,
       RECOMP_TASK_USER_FLAGS = 0xf0 }; /* SDK OS_TASK_USR0..USR3: application metadata. */
typedef struct RecompTask {
    uint64_t token;
    uint32_t guest_address, words[16]; /* OSTask snapshot, guest pointers retained. */
    bool resumed;
} RecompTask;
typedef struct RecompTaskBackend {
    void *userdata;
    bool (*submit)(Runtime *, void *, const RecompTask *);
    bool (*yield)(Runtime *, void *, uint64_t token);
} RecompTaskBackend;
typedef struct RecompTaskStats {
    uint64_t submissions, sp_events, dp_events, yielded, rejected_completions;
    size_t pending_records;
} RecompTaskStats;
bool recomp_tasks_init(Runtime *, const RecompTaskBackend *);
void recomp_tasks_destroy(Runtime *);
/* Callbacks run on the owner thread and must return without yielding. Copy the
 * descriptor for worker use. Only post_completion is safe from worker threads.
 * It reports FIFO admission; stale/duplicate receipts are rejected on polling. */
bool recomp_task_post_completion(Runtime *, uint64_t, RecompTaskCompletion);
/* Owner-thread completion additionally validates the token and rejects duplicates. */
bool recomp_task_complete(Runtime *, uint64_t, RecompTaskCompletion);
/* Owner-thread backend notice after a real SP receipt: this graphics list
 * ended without issuing a DP full sync. Retires its record without a DP event. */
bool recomp_task_finish_without_dp(Runtime *, uint64_t);
RecompTaskStats recomp_task_stats(Runtime *);
bool recomp_task_snapshot(Runtime *, RecompTask *);
void hook_os_sp_task_load(Runtime *, MipsContext *);
void hook_os_sp_task_start_go(Runtime *, MipsContext *);
void hook_os_sp_task_yield(Runtime *, MipsContext *);
void hook_os_sp_task_yielded(Runtime *, MipsContext *);

/* Native replacements share the generated-function ABI. */
void hook_os_create_thread(Runtime *, MipsContext *);
void hook_os_start_thread(Runtime *, MipsContext *);
void hook_os_stop_thread(Runtime *, MipsContext *);
void hook_os_destroy_thread(Runtime *, MipsContext *);
void hook_os_yield_thread(Runtime *, MipsContext *);
void hook_os_get_thread_pri(Runtime *, MipsContext *);
void hook_os_get_thread_id(Runtime *, MipsContext *);
void hook_os_set_thread_pri(Runtime *, MipsContext *);
void hook_os_create_mesg_queue(Runtime *, MipsContext *);
void hook_os_send_mesg(Runtime *, MipsContext *);
void hook_os_jam_mesg(Runtime *, MipsContext *);
void hook_os_recv_mesg(Runtime *, MipsContext *);
void hook_os_set_event_mesg(Runtime *, MipsContext *);
void hook_os_writeback_dcache_all(Runtime *, MipsContext *);
void hook_os_cache_range(Runtime *, MipsContext *);
/* Global libultra page pairs. ASID mappings and privileged instructions are unsupported. */
void recomp_map_pages(Runtime *, unsigned, uint32_t, uint32_t, uint32_t, uint32_t);
void recomp_unmap_pages(Runtime *);
void hook_os_map_tlb(Runtime *, MipsContext *);
void hook_os_unmap_tlb_all(Runtime *, MipsContext *);

#ifdef __cplusplus
}
#endif
#endif
