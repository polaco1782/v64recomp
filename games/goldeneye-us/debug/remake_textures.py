#!/usr/bin/env python3
"""Run the shared texture tool with GoldenEye's local dump and replacement paths."""
from pathlib import Path
import sys

project = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(project / 'tools'))
from common import RecompError
from remake_textures import main


if __name__ == '__main__':
    try:
        sys.exit(main(default_root=project / 'local/textures/goldeneye-us'))
    except (RecompError, OSError, ValueError) as error:
        print(f'error: {error}', file=sys.stderr)
        sys.exit(1)
