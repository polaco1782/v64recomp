"""Build a game-provided CPU translation with the shared RT64 adapter."""
from pathlib import Path
import subprocess
import sys

from common import RecompError


def build_renderer(output: Path, jobs: int, target: str, generated_option: str, *, debug: bool = False) -> Path:
    project = Path(__file__).resolve().parents[1]
    build = project / ('build/debug' if debug else 'build/renderer')
    subprocess.run([
        'cmake', '-S', str(project), '-B', str(build), '-G', 'Ninja',
        '-DRECOMP_BUILD_RT64=ON', '-DRECOMP_BUILD_AUDIO=ON',
        f'-DCMAKE_BUILD_TYPE={"Debug" if debug else "Release"}',
        '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON', '-DCMAKE_C_COMPILER=/usr/bin/gcc',
        '-DCMAKE_CXX_COMPILER=/usr/bin/g++', '-DCMAKE_ASM_COMPILER=/usr/bin/gcc',
        f'-D{generated_option}={output}',
    ], check=True)
    log = build / f'{target}-build.log'
    print(f'Building {target}; log: {log}', flush=True)
    with log.open('w') as stream:
        result = subprocess.run([
            'cmake', '--build', str(build), '--target', target, '-j', str(jobs),
        ], stdout=stream, stderr=subprocess.STDOUT)
    if result.returncode:
        print('\n'.join(log.read_text().splitlines()[-40:]), file=sys.stderr)
        raise RecompError(f'renderer build failed; see {log}')
    return build / target
