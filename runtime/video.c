#include "runtime.h"

bool recomp_vi_registers(Runtime *rt, RecompViRegisters *regs) {
    RecompViState vi=recomp_vi_state(rt);
    if(!regs || !vi.mode)return false;
    uint32_t mode=vi.mode;
    *regs=(RecompViRegisters){
        .status=vi.black || !vi.framebuffer ? vi.special_features&~3u : vi.special_features,
        .origin=vi.framebuffer ? recomp_ram_address(rt,vi.framebuffer,1)+(uint32_t)recomp_load(rt,mode+40,4) : 0,
        .width=(uint32_t)recomp_load(rt,mode+8,4),
        .intr=(uint32_t)recomp_load(rt,mode+56,4),
        .burst=(uint32_t)recomp_load(rt,mode+12,4),
        .v_sync=(uint32_t)recomp_load(rt,mode+16,4),
        .h_sync=(uint32_t)recomp_load(rt,mode+20,4),
        .leap=(uint32_t)recomp_load(rt,mode+24,4),
        .h_start=(uint32_t)recomp_load(rt,mode+28,4),
        .v_start=(uint32_t)recomp_load(rt,mode+48,4),
        .v_burst=(uint32_t)recomp_load(rt,mode+52,4),
        .x_scale=(uint32_t)recomp_load(rt,mode+32,4),
        .y_scale=(uint32_t)recomp_load(rt,mode+44,4),
    };
    if(vi.framebuffer)recomp_ram_address(rt,regs->origin,1);
    return true;
}
