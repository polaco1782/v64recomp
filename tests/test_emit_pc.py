"""Run with .venv/bin/python tests/test_emit_pc.py (requires a C compiler)."""
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import emit
from translate import translate


class GeneratedPcTests(unittest.TestCase):
    def test_instruction_paths_and_release_erasure(self):
        compiler = shutil.which('cc')
        self.assertIsNotNone(compiler)
        emit.LOG_EMISSION = False
        functions, cases = [], []

        def fixture(words, paths, **extra):
            address = 0x80001000 + len(functions) * 0x100
            function = dict(name='pc_probe', section=0, vram=address,
                            size=len(words)*4, **extra)
            native = dict(name='native', section=0, vram=0x80002000, size=8)
            registry = emit.FunctionRegistry([function, native])
            raw = struct.pack(f'>{len(words)}I', *words)
            functions.append(emit.emit_function(function, emit.decode(raw, address), registry,
                                               hooks={(0, 0x80002000): 'native_probe'}))
            self.assertFalse(any(line.strip() == ';' or line.endswith(': ;')
                                 for line in functions[-1].splitlines()))
            for argument, offsets in paths:
                expected = ','.join(f'0x{address+offset:08x}' for offset in offsets)
                cases.append(f'''{{
                    MipsContext ctx={{0}}; ctx.pc=0x12345678;
                    ctx.gpr[4]={argument}; ctx.gpr[25]=0x80002000; ctx.gpr[31]=0xf0000000;
                    uint32_t expected[]={{{expected}}}; count=0;
                    recomp_s0_{address:08x}(&runtime,&ctx);
                    assert(ctx.gpr[0]==0);
                #ifdef RECOMP_DEBUG
                    assert(count==sizeof(expected)/sizeof(*expected));
                    assert(!memcmp(trace,expected,sizeof(expected)));
                #else
                    (void)expected; assert(ctx.pc==0x12345678);
                #endif
                }}''')

        # All straight-line classes, including a memory op and an FP helper.
        fixture([0x24080007, 0, 0xac880000, 0x46000000, 0x03e00008, 0],
                [(0, [0, 4, 8, 12, 16, 20])])
        # Dynamic likely and ordinary branches: only likely annuls its slot.
        fixture([0x50800002, 0x24080007, 0x24090003, 0x03e00008, 0],
                [(0, [0, 4, 12, 16]), (1, [0, 8, 12, 16])])
        fixture([0x10800002, 0x24080007, 0x24090003, 0x03e00008, 0],
                [(0, [0, 4, 12, 16]), (1, [0, 4, 8, 12, 16])])
        # Constant branches exercise both folded paths.
        fixture([0x10000002, 0, 0, 0x03e00008, 0], [(0, [0, 4, 12, 16])])
        fixture([0x54000002, 0, 0, 0x03e00008, 0], [(0, [0, 8, 12, 16])])
        # Backward branch and checkpoint; the loop exits on its second visit.
        fixture([0x25080001, 0x29090002, 0x1520fffd, 0, 0x03e00008, 0],
                [(0, [0, 4, 8, 12, 0, 4, 8, 12, 16, 20])])
        # Direct and dynamic calls restore their call-site PC after the delay slot.
        for call in (0x0c000800, 0x0320f809):
            fixture([0x03e08025, call, 0x24080007, 0x0200f825, 0x03e00008, 0],
                    [(0, [0, 4, 8, 4, 12, 16, 20])])
        # Tail jumps also attribute native hooks to the transfer instruction.
        fixture([0x08000800, 0x24080007], [(0, [0, 4, 0])])

        harness = r'''
            #include "recomp.h"
            #include <assert.h>
            #include <stdlib.h>
            #include <string.h>
            static unsigned count;
            static uint32_t trace[128];
            #ifdef RECOMP_DEBUG
            static void track(MipsContext *ctx,uint32_t address) {
                RECOMP_TRACK_PC(ctx,address);
                assert(count<128); trace[count++]=ctx->pc;
            }
            #undef RECOMP_TRACK_PC
            #define RECOMP_TRACK_PC(ctx,address) track(ctx,address)
            #endif
            static void native_probe(Runtime *rt,MipsContext *ctx) {
                (void)rt; assert(ctx->gpr[8]==7);
                #ifdef RECOMP_DEBUG
                assert(ctx->pc==trace[count-1]);
                #endif
            }
            RecompFunction recomp_lookup(Runtime *rt,uint32_t address) {
                (void)rt; assert(address==0x80002000); return native_probe;
            }
            void recomp_store(Runtime *rt,uint32_t address,uint64_t value,unsigned bytes) {
                (void)rt; assert(address==0 && value==7 && bytes==4);
            }
            void mips_fp_binary(MipsContext *ctx,unsigned d,unsigned s,unsigned t,
                                enum MipsFpFormat fmt,enum MipsFpOperation op) {
                (void)ctx; assert(d==0 && s==0 && t==0 && fmt==MIPS_FP_S && op==MIPS_FP_ADD);
            }
            _Noreturn void recomp_fail(const char *reason,uint32_t address) {
                (void)reason; (void)address; abort();
            }
            _Noreturn void recomp_fellthrough(uint32_t address) { (void)address; abort(); }
        '''
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'probe.c'
            # Check the actual generated bodies with the real macro, before the
            # Debug-only trace observer above wraps it for path assertions.
            source.write_text('#include "recomp.h"\n' + '\n'.join(functions))
            command = [compiler, '-std=c11', '-I', str(ROOT / 'include')]
            preprocessed = subprocess.check_output([*command, '-E', '-P', str(source)], text=True)
            generated = preprocessed[preprocessed.index('void recomp_s0_'):]
            self.assertNotIn('->pc', generated)
            self.assertNotIn('RECOMP_TRACK_PC', generated)
            source.write_text(harness + '\n'.join(functions) + '\nint main(void) {\n'
                              'bool loaded=true; Runtime runtime={.section_loaded=&loaded,.section_count=1};\n'
                              '#ifndef RECOMP_DEBUG\n'
                              'int side_effect=0; RECOMP_TRACK_PC((side_effect++, (MipsContext *)0), ++side_effect);\n'
                              'assert(side_effect==0);\n#endif\n'
                              + '\n'.join(cases) + '\n(void)trace; return 0; }\n')
            for mode in ('Debug', 'Release'):
                binary = Path(directory) / mode
                defines = ['-DRECOMP_DEBUG'] if mode == 'Debug' else []
                subprocess.run([*command, *defines, '-O0', '-Wall', '-Wextra', '-Werror',
                                str(source), '-o', str(binary)], check=True)
                subprocess.run([str(binary)], check=True)

    def test_failure_boundary_keeps_constant_address(self):
        emit.LOG_EMISSION = False
        function = dict(name='boundary', aliases=[], section=0, vram=0x80001000,
                        rom_offset=0, size=4)
        profile = dict(game={}, native_hooks=dict(enabled=False), boundaries=[dict(
            symbol='boundary', vram=0x80001000, kind='unsupported_function',
            reason='probe boundary', expected_words=[0])])
        metadata = dict(functions=[function], sections=[dict(loaded_at_boot=True)], rom_sha1='probe')
        with tempfile.TemporaryDirectory() as directory:
            translate(bytes(4), metadata, profile, Path(directory), names=['boundary'])
            source = (Path(directory) / 'functions.c').read_text()
            self.assertNotIn('ctx->pc', source)
            harness = Path(directory) / 'boundary.c'
            harness.write_text(r'''
                #include "generated.h"
                #include <assert.h>
                #include <setjmp.h>
                #include <string.h>
                static jmp_buf failed;
                static MipsContext ctx;
                _Noreturn void recomp_fail(const char *reason,uint32_t address) {
                    assert(!strcmp(reason,"probe boundary") && address==0x80001000);
                    longjmp(failed,1);
                }
                int main(void) {
                    Runtime rt={0}; ctx.pc=0x12345678;
                    if (!setjmp(failed)) { recomp_s0_80001000(&rt,&ctx); return 1; }
                #ifdef RECOMP_DEBUG
                    assert(ctx.pc==0x80001000);
                #else
                    assert(ctx.pc==0x12345678);
                #endif
                    return 0;
                }
            ''')
            for mode in ('Debug', 'Release'):
                binary = Path(directory) / mode
                defines = ['-DRECOMP_DEBUG'] if mode == 'Debug' else []
                subprocess.run(['cc', '-std=c11', '-O0', '-Wall', '-Wextra', '-Werror',
                                *defines, '-I', str(ROOT / 'include'), '-I', directory,
                                str(Path(directory) / 'functions.c'), str(harness),
                                '-o', str(binary)], check=True)
                subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
