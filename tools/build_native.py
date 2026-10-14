"""Build cached generated ROM C and a selected native application."""
from pathlib import Path
import hashlib,json,subprocess


def build_native(output: Path,cc: str,*,application: Path,executable_name: str,
                 optimization: str='2',sanitize_address: bool=False,
                 audio: bool = True, extra_sources: tuple[Path, ...] = ()) -> Path:
    """Compile cached ROM functions and the game-provided application."""
    project=Path(__file__).resolve().parents[1]
    binary=output/executable_name
    sanitizer='address,undefined' if sanitize_address else 'undefined'
    flags=[cc,'-std=c11',f'-O{optimization}','-g','-Wall','-Wextra','-Werror','-Wno-unused-label',
           '-fno-fast-math','-frounding-math','-ffp-contract=off',f'-fsanitize={sanitizer}',
           '-fno-sanitize-recover=all','-fno-omit-frame-pointer','-I',str(project/'include'),
           '-pthread','-I',str(output)]
    digest=hashlib.sha256(json.dumps(flags).encode())
    digest.update(subprocess.check_output([cc,'--version']))
    for path in (output/'functions.c',output/'generated.h',project/'include/recomp.h'):
        digest.update(path.read_bytes())
    cache=project/'build/native-objects';cache.mkdir(parents=True,exist_ok=True)
    compiled=cache/f'{digest.hexdigest()}.o'
    if not compiled.exists():
        temporary=compiled.with_suffix('.tmp.o')
        subprocess.run([*flags,'-c',str(output/'functions.c'),'-o',str(temporary)],check=True)
        temporary.replace(compiled)
    command=[*flags,str(compiled),
             *(str(project/f'runtime/{name}.c') for name in ('memory','fpu','scheduler','worker','os',
                                                         'bootstrap','time','platform','tasks','io','video'))]
    if audio:
        sdl_flags=subprocess.check_output(['pkg-config','--cflags','--libs','sdl2'],text=True).split()
        command.extend(['-DRECOMP_WITH_AUDIO','-fno-strict-aliasing','-I',str(project/'third_party/rsp-hle'),
                        str(project/'backends/audio.c'),
                        *(str(project/f'third_party/rsp-hle/{name}.c') for name in ('alist','alist_audio','alist_nead','audio','memory')),
                        *sdl_flags])
    command.extend([str(application),*(str(p) for p in extra_sources),
                    '-lcrypto','-lm','-o',str(binary)])
    subprocess.run([*command,'-rdynamic','-ldl'],check=True)
    return binary
