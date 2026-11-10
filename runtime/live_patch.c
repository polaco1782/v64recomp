#define _POSIX_C_SOURCE 200809L
#include "runtime.h"
#include "recomp_live.h"

#include <dlfcn.h>
#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

typedef struct LiveModule {
    struct LiveModule *next;
    void *handle;
    const RecompLiveManifest *manifest;
} LiveModule;

static const RecompFunctionEntry *find_entry(Runtime *rt, uint32_t address, uint32_t site, uint32_t section,
                                             bool checked) {
    const RecompFunctionEntry *found = NULL;
    bool unloaded = false;
    for (LiveModule *module = rt->live_modules; module; module = module->next) {
        const RecompLiveManifest *m = module->manifest;
        if (checked) {
            bool allowed = false;
            for (size_t i = 0; i < m->call_count; i++)
                if (m->calls[i].site == site && m->calls[i].target == address && m->calls[i].section == section)
                    allowed = true;
            if (!allowed)
                continue;
        }
        for (size_t i = 0; i < m->function_count; i++) {
            const RecompFunctionEntry *entry = &m->functions[i];
            if (entry->address != address || entry->section >= rt->section_count)
                continue;
            if (!rt->section_loaded[entry->section]) {
                unloaded = true;
                continue;
            }
            if (found && found->section != entry->section)
                recomp_fail("ambiguous loaded live function", address);
            if (!found)
                found = entry;
        }
    }
    if (!found && unloaded)
        recomp_fail("live function section is unloaded", address);
    return found;
}

static LiveModule *find_jump(Runtime *rt, uint32_t address, uint32_t site, uint32_t section) {
    bool unloaded = false;
    for (LiveModule *module = rt->live_modules; module; module = module->next) {
        const RecompLiveManifest *m = module->manifest;
        bool allowed = false;
        for (size_t i = 0; i < m->jump_count; i++)
            if (m->jumps[i].site == site && m->jumps[i].target == address && m->jumps[i].section == section)
                allowed = true;
        if (!allowed)
            continue;
        for (size_t i = 0; i < m->function_count; i++) {
            const RecompFunctionEntry *entry = &m->functions[i];
            if (entry->section >= rt->section_count)
                continue;
            if (!rt->section_loaded[entry->section]) {
                unloaded = true;
                continue;
            }
            if (m->resume || entry->address == address)
                return module;
        }
    }
    if (unloaded)
        recomp_fail("live jump section is unloaded", address);
    return NULL;
}

static bool valid_manifest(Runtime *rt, const RecompLiveManifest *m) {
    if (!m || m->abi != RECOMP_LIVE_ABI || m->runtime_size != sizeof(Runtime) ||
        m->context_size != sizeof(MipsContext) || m->entry_size != sizeof(RecompFunctionEntry) ||
        m->debug_build !=
#ifdef RECOMP_DEBUG
            1u ||
#else
            0u ||
#endif
        memcmp(m->rom_sha1, rt->rom_sha1, 20) || !m->functions || !m->function_count || m->function_count > 1000000 ||
        (m->call_count && !m->calls) || m->call_count > 1000000 || (m->jump_count && !m->jumps) ||
        m->jump_count > 1000000)
        return false;
    for (size_t i = 0; i < m->function_count; i++)
        if (!m->functions[i].function || !m->functions[i].size || m->functions[i].address % 4 ||
            m->functions[i].section >= rt->section_count)
            return false;
    return true;
}

static bool load_module(Runtime *rt, const char *path) {
    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        fprintf(stderr, "live repair: cannot load %s: %s\n", path, dlerror());
        return false;
    }
    const RecompLiveManifest *manifest = dlsym(handle, "recomp_live_manifest");
    if (!valid_manifest(rt, manifest)) {
        fprintf(stderr, "live repair: module ABI, ROM, or registry mismatch: %s\n", path);
        dlclose(handle);
        return false;
    }
    LiveModule *module = malloc(sizeof(*module));
    if (!module) {
        dlclose(handle);
        return false;
    }
    *module = (LiveModule){.next = rt->live_modules, .handle = handle, .manifest = manifest};
    rt->live_modules = module;
    fprintf(stderr, "live repair: loaded %s (%zu functions)\n", path, manifest->function_count);
    return true;
}

/* Guest execution owns the scheduler token here. The owner is asleep in its
 * normal scheduler wait; no device polling or guest thread dispatch occurs. */
static bool wait_for_repair(Runtime *rt, uint32_t address, uint32_t site, uint32_t section, bool checked, bool jump) {
    const char *dir = getenv("RECOMP_LIVE_PATCH_DIR");
    if (!dir)
        dir = rt->live_patch_dir;
    if (!dir || !*dir || !strcmp(dir, "off") || !rt->rom)
        return false;
    char control[4096];
    if (snprintf(control, sizeof(control), "%s/function-%08x.path", dir, address) >= (int)sizeof(control))
        return false;
    fprintf(stderr, "live repair: paused at %s target 0x%08x, site 0x%08x, section %u; waiting for %s\n",
            jump      ? "checked jump"
            : checked ? "checked call"
                      : "missing function",
            address, site, section, control);
    char request[4096], temporary[4096];
    bool request_ready = false;
    if (snprintf(request, sizeof(request), "%s/request-%08x.json", dir, address) < (int)sizeof(request) &&
        snprintf(temporary, sizeof(temporary), "%s/.request-%08x-%ld.tmp", dir, address, (long)getpid()) <
            (int)sizeof(temporary)) {
        FILE *file = fopen(temporary, "w");
        if (file) {
            fprintf(file,
                    "{\"format_version\":1,\"address\":\"0x%08x\",\"site\":\"0x%08x\","
                    "\"section\":%u,\"kind\":\"%s\",\"rom_sha1\":\"",
                    address, site, section,
                    jump      ? "jump"
                    : checked ? "call"
                              : "missing_function");
            for (size_t i = 0; i < sizeof(rt->rom_sha1); i++)
                fprintf(file, "%02x", rt->rom_sha1[i]);
            fprintf(file, "\"}\n");
            if (fclose(file) == 0 && rename(temporary, request) == 0)
                request_ready = true;
            else
                unlink(temporary);
        }
    }
    recomp_clock_pause(rt);
    pid_t helper = 0;
    const char *automatic = getenv("RECOMP_LIVE_AUTO");
    if ((!automatic || strcmp(automatic, "off")) && rt->live_patch_python && rt->live_patch_script &&
        rt->live_patch_game && rt->live_patch_build_dir) {
        char target_text[16], site_text[16], section_text[16];
        snprintf(target_text, sizeof(target_text), "0x%08x", address);
        snprintf(site_text, sizeof(site_text), "0x%08x", site);
        snprintf(section_text, sizeof(section_text), "%u", section);
        char *const args[] = {(char *)rt->live_patch_python,
                              (char *)rt->live_patch_script,
                              "--game",
                              (char *)rt->live_patch_game,
                              "--address",
                              target_text,
                              "--site",
                              site_text,
                              "--section",
                              section_text,
                              "--kind",
                              jump      ? "jump"
                              : checked ? "call"
                                        : "missing_function",
                              "--build-dir",
                              (char *)rt->live_patch_build_dir,
                              "--patch-dir",
                              (char *)dir,
                              NULL};
        int error = posix_spawn(&helper, rt->live_patch_python, NULL, NULL, args, environ);
        if (error) {
            fprintf(stderr, "live repair: cannot start helper: %s\n", strerror(error));
            helper = 0;
        }
    }
    char attempted[4096] = {0};
    for (;;) {
        if (helper) {
            int status = 0;
            if (waitpid(helper, &status, WNOHANG) == helper) {
                if (!WIFEXITED(status) || WEXITSTATUS(status))
                    fprintf(stderr, "live repair: automatic helper stopped; still waiting for a module\n");
                helper = 0;
            }
        }
        if (jump ? find_jump(rt, address, site, section) != NULL
                 : find_entry(rt, address, site, section, checked) != NULL) {
            recomp_clock_resume(rt);
            if (request_ready)
                unlink(request);
            fprintf(stderr, "live repair: resuming target 0x%08x\n", address);
            return true;
        }
        FILE *file = fopen(control, "r");
        if (file) {
            char path[4096];
            if (fgets(path, sizeof(path), file)) {
                path[strcspn(path, "\r\n")] = 0;
                if (!strcmp(path, "abort")) {
                    fclose(file);
                    recomp_clock_resume(rt);
                    if (request_ready)
                        unlink(request);
                    return false;
                }
                if (path[0] == '/' && strcmp(path, attempted)) {
                    snprintf(attempted, sizeof(attempted), "%s", path);
                    (void)load_module(rt, path);
                }
            }
            fclose(file);
        }
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 250000000};
        while (nanosleep(&delay, &delay) && errno == EINTR) {
        }
    }
}

RecompFunction recomp_live_lookup(Runtime *rt, uint32_t address) {
    const RecompFunctionEntry *entry = find_entry(rt, address, 0, 0, false);
    if (!entry && wait_for_repair(rt, address, 0, 0, false, false))
        entry = find_entry(rt, address, 0, 0, false);
    if (!entry)
        return NULL;
    return entry->function;
}

void recomp_live_checked_call(Runtime *rt, MipsContext *ctx, uint32_t target, uint32_t site, uint32_t section) {
    target = recomp_canonical_function_address(rt, target);
    const RecompFunctionEntry *entry = find_entry(rt, target, site, section, true);
    if (!entry && wait_for_repair(rt, target, site, section, true, false))
        entry = find_entry(rt, target, site, section, true);
    if (!entry)
        recomp_fail("unlisted indirect call target", target);
    recomp_record_call(rt, target);
    if (rt->prepare_call)
        rt->prepare_call(rt, target);
    entry->function(rt, ctx);
}

void recomp_live_checked_jump(Runtime *rt, MipsContext *ctx, uint32_t target, uint32_t site, uint32_t section,
                              uint32_t return_address, uint32_t entry_stack_pointer) {
    target = recomp_canonical_function_address(rt, target);
    LiveModule *module = find_jump(rt, target, site, section);
    if (!module && wait_for_repair(rt, target, site, section, false, true))
        module = find_jump(rt, target, site, section);
    if (!module)
        recomp_fail("unlisted indirect jump target", target);
    if (module->manifest->resume) {
        module->manifest->resume(rt, ctx, target, return_address, entry_stack_pointer);
        return;
    }
    for (size_t i = 0; i < module->manifest->function_count; i++) {
        const RecompFunctionEntry *entry = &module->manifest->functions[i];
        if (entry->address == target && entry->section < rt->section_count && rt->section_loaded[entry->section]) {
            recomp_record_call(rt, target);
            if (rt->prepare_call)
                rt->prepare_call(rt, target);
            entry->function(rt, ctx);
            return;
        }
    }
    recomp_fail("live jump target became unloaded", target);
}

void recomp_live_indirect_jump(Runtime *rt, MipsContext *ctx, uint32_t target, uint32_t site, uint32_t section,
                               uint32_t return_address, uint32_t entry_stack_pointer) {
    if (recomp_has_function(rt, target)) {
        recomp_lookup(rt, target)(rt, ctx);
        return;
    }
    recomp_live_checked_jump(rt, ctx, target, site, section, return_address, entry_stack_pointer);
}

bool recomp_live_boundary(Runtime *rt, MipsContext *ctx, uint32_t address) {
    const RecompFunctionEntry *entry = find_entry(rt, address, 0, 0, false);
    if (!entry && wait_for_repair(rt, address, 0, 0, false, false))
        entry = find_entry(rt, address, 0, 0, false);
    if (!entry)
        return false;
    entry->function(rt, ctx);
    return true;
}

void recomp_live_destroy(Runtime *rt) {
    LiveModule *module = rt->live_modules;
    while (module) {
        LiveModule *next = module->next;
        dlclose(module->handle);
        free(module);
        module = next;
    }
    rt->live_modules = NULL;
}
