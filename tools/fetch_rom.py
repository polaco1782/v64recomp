"""Fetch an explicitly selected homebrew ROM; never store ROM content in tracked files."""
from __future__ import annotations

import hashlib
from http.client import HTTPException
import os
from pathlib import Path
import tempfile
from urllib.request import urlopen

from common import RecompError


def ensure_rom(profile: dict) -> None:
    game, source = profile['game'], profile['download']
    destination = Path(game['rom'])
    label = game['name']

    def verify(path: Path) -> None:
        digest = hashlib.sha256()
        size = 0
        with path.open('rb') as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b''):
                size += len(block)
                digest.update(block)
        if size != game['rom_size'] or digest.hexdigest() != source['sha256']:
            raise RecompError(f'{label} ROM size/checksum mismatch: {path}; '
                              'existing files are never overwritten')

    if destination.exists():
        verify(destination)
        print(f'{label}: using verified cached ROM {destination}', flush=True)
        return
    if not source['url'].startswith('https://'):
        raise RecompError(f'{label} ROM download requires HTTPS')
    destination.parent.mkdir(parents=True, exist_ok=True)
    print(f'{label}: downloading external ROM from {source["url"]}', flush=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=destination.parent, prefix=f'.{destination.stem}-',
                                         suffix='.part', delete=False) as output:
            temporary = Path(output.name)
            with urlopen(source['url'], timeout=60) as response:
                if not response.geturl().startswith('https://'):
                    raise RecompError(f'{label} ROM download redirected away from HTTPS')
                size = 0
                while block := response.read(1024 * 1024):
                    size += len(block)
                    if size > game['rom_size']:
                        raise RecompError(f'{label} ROM download exceeds the pinned size')
                    output.write(block)
        verify(temporary)
        # Publish atomically without replacing a file another build just created.
        try:
            os.link(temporary, destination)
        except FileExistsError:
            verify(destination)
        print(f'{label}: verified ROM cached at {destination}', flush=True)
    except (OSError, HTTPException) as error:
        raise RecompError(f'{label} ROM download failed: {error}. '
                          f'Retry online or place the pinned ROM at {destination}') from error
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
