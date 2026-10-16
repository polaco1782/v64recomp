#include "runtime.h"

bool recomp_vi_registers(Runtime *rt, RecompViRegisters *regs) {
    RecompViState vi=recomp_vi_state(rt);
    if(!regs || !vi.mode)return false;
    uint32_t mode=vi.mode;
    *regs=(RecompViRegisters){
        .status=vi.black || !vi.framebuffer ? vi.special_features&~3u : vi.special_features,
        .origin=!vi.black && vi.framebuffer ? recomp_ram_address(rt,vi.framebuffer,1)+(uint32_t)recomp_load(rt,mode+40,4) : 0,
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
    regs->x_scale=(regs->x_scale&~0xfffu)|((uint32_t)((regs->x_scale&0xfff)*vi.x_factor)&0xfff);
    regs->y_scale=(regs->y_scale&~0xfffu)|((uint32_t)((regs->y_scale&0xfff)*vi.y_factor)&0xfff);
    /* Games can suppress scanout with a reversed active interval while
     * preparing their first framebuffer. Do not pass a negative extent to
     * RT64, which represents framebuffer dimensions as unsigned values. */
    if((regs->h_start&0x3ff)<=((regs->h_start>>16)&0x3ff)
            || (regs->v_start&0x3ff)<=((regs->v_start>>16)&0x3ff)
            || !regs->width || !(regs->x_scale&0xfff) || !(regs->y_scale&0xfff))
        regs->status&=~3u;
    if(!vi.black && vi.framebuffer)recomp_physical_address(rt,regs->origin,1);
    return true;
}
