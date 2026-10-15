"""Emit ROM load mappings and the small set of globals used by native services."""
from pathlib import Path
from common import RecompError


def write_game_config(profile: dict,metadata: dict,output: Path,extra_symbols=()) -> None:
    game,boot=profile['game'],profile['bootstrap']
    constants={'GAME_ENTRY_POINT':metadata['entry_vram'],'GAME_RAM_SIZE':game['ram_size'],'GAME_ROM_SIZE':game['rom_size']}
    constants.update({f'GAME_{name.upper()}':boot[name] for name in
        ('counter_hz','timer_sentinel','vi_period_ticks','idle_thread','main_thread')})
    constants.update({f'GAME_{name.upper()}':metadata['symbols'][name] for name in
        ('gDmaIoMesg','gDmaMesgQueue','gIntrMesgQueue',*extra_symbols)})
    constants.update({f'GAME_{name.upper()}':value for name,value in profile['runtime'].items()})
    lines=['/* Generated from verified ROM mappings and native-service globals. */']
    lines.extend(f'#define {name} UINT64_C(0x{value:x})' for name,value in constants.items())
    lines.append('static const uint8_t game_sha1[20] = {'+','.join(f'0x{b:02x}' for b in bytes.fromhex(game['rom_sha1']))+'};')
    lines.append('static const RecompBootLoad game_code_loads[] = {')
    lines.extend(f'    {{{s["id"]},UINT32_C(0x{s["vram"]:x}),UINT32_C(0x{s["rom_offset"]:x}),UINT32_C(0x{s["size"]:x})}},' for s in metadata['sections'])
    lines.append('};')
    selected=[s for s in metadata['sections'] if s['name'] in boot['load_sections']]
    if len(selected)!=len(boot['load_sections']):raise RecompError('bootstrap load section missing or duplicated')
    lines.append('static const RecompBootLoad game_boot_loads[] = {')
    lines.extend(f'    {{{s["id"]},UINT32_C(0x{s["vram"]:x}),UINT32_C(0x{s["rom_offset"]:x}),UINT32_C(0x{s["size"]:x})}},' for s in selected)
    lines.extend(['};','static const uint32_t game_parameters[][2] = {'])
    lines.extend(f'    {{UINT32_C({address}),UINT32_C(0x{value:x})}},' for address,value in boot['parameters'].items())
    lines.extend(['};','static const RecompPlatformConfig game_platform = {'])
    lines.extend(f'    .{name}=UINT32_C(0x{value:x}),' for name,value in profile['platform'].items())
    lines.extend(['    .vi_period_ticks=GAME_VI_PERIOD_TICKS, .code_loads=game_code_loads,',
                 '    .code_load_count=sizeof(game_code_loads)/sizeof(game_code_loads[0]),','};'])
    (output/'game_config.h').write_text('\n'.join(lines)+'\n')
