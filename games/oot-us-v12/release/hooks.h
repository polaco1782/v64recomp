#pragma once
#include "overlays.h"
static void oot_prepare_call(Runtime *rt, uint32_t address) {
#if BOOT_STARTUP
    if (address == 0x80003040 && !rt->section_loaded[2]
            && (uint32_t)recomp_thread_context(rt, 0)->gpr[31] == 0x800adacc) {
        uint8_t *image=malloc(OOT_DD_SIZE), digest[32]; unsigned length=0;
        require(image!=NULL, "OOT DD verification allocation failed", 0);
        for(uint32_t i=0;i<OOT_DD_SIZE;i++)image[i]=(uint8_t)recomp_load(rt,0x801c7740+i,1);
        bool valid=EVP_Digest(image,OOT_DD_SIZE,digest,&length,EVP_sha256(),NULL)
            && length==32 && !memcmp(digest,oot_dd_sha256,32);
        free(image);require(valid,"OOT DD code fingerprint mismatch",0x801c7740);
        rt->section_loaded[2]=true;
    }
    if (address == 0x800adaf8 && !recomp_load(rt, 0x80121ae2, 1))
        rt->section_loaded[2]=false;
    /* The pinned main-thread loader has completed its synchronous code DMA
     * before this osGetTime call. Activate only after verifying the whole image. */
    if (address == 0x80004ad0 && !rt->section_loaded[1]
            && (uint32_t)recomp_thread_context(rt, 0)->gpr[31] == 0x800005ec) {
        uint8_t *image = malloc(OOT_CODE_SIZE), digest[32];
        unsigned length = 0;
        require(image != NULL, "OOT code verification allocation failed", 0);
        for (uint32_t i=0; i<OOT_CODE_SIZE; i++) image[i]=(uint8_t)recomp_load(rt, 0x800116e0+i, 1);
        bool valid = EVP_Digest(image, OOT_CODE_SIZE, digest, &length, EVP_sha256(), NULL)
            && length == 32 && !memcmp(digest, oot_code_sha256, 32);
        free(image);
        require(valid, "OOT decompressed code fingerprint mismatch", 0x800116e0);
        rt->section_loaded[1] = true;
    }
#endif
}
void hook_oot_cic_boot_task(Runtime *rt, MipsContext *ctx) {
    recomp_store(rt, 0x80008ef8, 0, 4);
    recomp_store(rt, 0x80008efc, 0, 4);
    mips_set(ctx, 2, 0);
}
void hook_oot_start_thread(Runtime *rt, MipsContext *ctx) {
    hook_os_start_thread(rt, ctx);
    if (recomp_thread_id(rt, 0) == -1) recomp_thread_stop(rt, 0);
}
