"""Exercise the demo download/cache failure paths without network or ROM assets."""
from pathlib import Path
import hashlib
from io import BytesIO
import sys
from tempfile import TemporaryDirectory
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[3]
sys.path[:0] = [str(ROOT / 'tools'), str(Path(__file__).resolve().parents[1])]
from common import RecompError
from fetch_rom import ensure_rom
from package_rom import strip_rom
from emit import analyze, decode, emit_function, FunctionRegistry


class Response(BytesIO):
    def geturl(self):
        return 'https://example.invalid/pinned-demo'


def main():
    payload = b'private fixture, not ROM content' * 8
    with TemporaryDirectory() as folder:
        destination = Path(folder) / 'demo.z64'
        profile = {'game': {'rom': destination, 'rom_size': len(payload)},
                   'download': {'url': 'https://example.invalid/pinned-demo',
                                'sha256': hashlib.sha256(payload).hexdigest()}}
        with patch('fetch_rom.urlopen', return_value=Response(payload)) as request:
            ensure_rom(profile)
            assert request.call_count == 1 and destination.read_bytes() == payload
        with patch('fetch_rom.urlopen', side_effect=AssertionError('cache used network')):
            ensure_rom(profile)
            destination.write_bytes(b'wrong existing file')
            try:
                ensure_rom(profile)
                raise AssertionError('corrupt cache accepted')
            except RecompError:
                assert destination.read_bytes() == b'wrong existing file'
        destination.unlink()
        for bad_payload in (payload[:-1], b'X' * len(payload), payload + b'extra'):
            with patch('fetch_rom.urlopen', return_value=Response(bad_payload)):
                try:
                    ensure_rom(profile)
                    raise AssertionError('invalid download accepted')
                except RecompError:
                    assert not destination.exists() and not list(Path(folder).glob('*.part'))
        class Interrupted(Response):
            def read(self, size=-1):
                if self.tell():
                    raise OSError('connection lost')
                return super().read(8)
        with patch('fetch_rom.urlopen', return_value=Interrupted(payload)):
            try:
                ensure_rom(profile)
                raise AssertionError('interrupted download accepted')
            except RecompError:
                assert not destination.exists() and not list(Path(folder).glob('*.part'))
        # A competing build publishes the same verified bytes before this one.
        def concurrent(url, timeout):
            destination.write_bytes(payload)
            return Response(payload)
        with patch('fetch_rom.urlopen', side_effect=concurrent):
            ensure_rom(profile)
            assert destination.read_bytes() == payload
    try:
        strip_rom(b'', b'', {}, {'packaging': {'allow_embedding': False}})
        raise AssertionError('embedding accepted')
    except RecompError:
        pass
    # Data-cache invalidation is coherent; tag operations remain unsupported.
    function = {'name': 'cache_fixture', 'vram': 0x80001000, 'size': 12, 'section': 0,
                'aliases': [], 'instruction_addresses': [0x80001000, 0x80001004, 0x80001008]}
    registry = FunctionRegistry([function])
    instructions = decode(bytes.fromhex('bc81000003e0000800000000'), function['vram'])
    assert not analyze(function, instructions, registry)
    assert 'coherent host RAM' in emit_function(function, instructions, registry)
    unsupported = decode(bytes.fromhex('bc85000003e0000800000000'), function['vram'])
    assert any('CACHE' in issue['reason'] for issue in analyze(function, unsupported, registry))
    print('Download/cache failure paths, concurrent publication, embedding guard and CACHE lowering passed.')


if __name__ == '__main__':
    main()
