"""Shared SDK symbol information and independently verified native replacements."""
from __future__ import annotations

import json
from pathlib import Path
import re

from common import RecompError

DATABASE = Path(__file__).with_name('symbols.json')


def integer(value, label: str, maximum: int = 0xffffffff) -> int:
    if type(value) is not int or not 0 <= value <= maximum:
        raise RecompError(f'invalid {label}: expected an unsigned integer')
    return value


def name(value, label: str = 'symbol') -> str:
    if not isinstance(value, str) or not value or len(value) > 1024 or not value.isprintable():
        raise RecompError(f'invalid {label}')
    return value


def normalize_native(record: dict) -> dict:
    if not isinstance(record, dict):
        raise RecompError('invalid native signature')
    if set(record) - {'handler', 'body_size', 'sha256', 'expected_words', 'notes'}:
        raise RecompError('unknown native signature fields')
    handler = record.get('handler')
    if not isinstance(handler, str) or not re.fullmatch(r'[A-Za-z_][A-Za-z_0-9]*', handler):
        raise RecompError('invalid native handler')
    size = integer(record.get('body_size'), 'native body size')
    words = record.get('expected_words')
    digest = record.get('sha256')
    if (not isinstance(words, list) or not words or size % 4 or size < len(words) * 4
            or not isinstance(digest, str) or not re.fullmatch(r'[0-9a-f]{64}', digest)):
        raise RecompError('invalid complete-body native fingerprint')
    words = [integer(w, 'native instruction word') for w in words]
    result = dict(handler=handler, body_size=size, sha256=digest, expected_words=words)
    if record.get('notes'):
        if not isinstance(record['notes'], list):
            raise RecompError('native signature notes must be a list')
        result['notes'] = sorted({name(n, 'signature note') for n in record['notes']})
    return result


def normalize_sdk(record: dict, sources: dict) -> dict:
    if not isinstance(record, dict):
        raise RecompError('invalid SDK signature')
    if set(record) - {'body_size', 'crc32_prefix', 'crc32_body', 'references', 'sources'}:
        raise RecompError('unknown SDK signature fields')
    size = integer(record.get('body_size'), 'SDK body size')
    if not size or size % 4:
        raise RecompError('SDK body size must be a positive multiple of four')
    prefix = integer(record.get('crc32_prefix'), 'SDK prefix CRC')
    body = integer(record.get('crc32_body'), 'SDK body CRC')
    source_ids = record.get('sources')
    if (not isinstance(source_ids, list) or not source_ids
            or any(not isinstance(s, str) or s not in sources for s in source_ids)):
        raise RecompError('SDK signature references an unknown source')
    references = record.get('references')
    if not isinstance(references, list):
        raise RecompError('SDK references must be a list')
    grouped = {}
    for reference in references:
        if not isinstance(reference, list) or len(reference) != 3:
            raise RecompError('invalid SDK relocation record')
        kind, target, offsets = reference
        if kind not in ('hi16', 'lo16', 'targ26'):
            raise RecompError('unknown SDK relocation kind')
        name(target, 'SDK reference target')
        if not isinstance(offsets, list) or not offsets:
            raise RecompError('SDK reference offsets must be a nonempty list')
        for offset in offsets:
            integer(offset, 'SDK reference offset')
            if offset % 4 or offset + 4 > size:
                raise RecompError('SDK reference outside its signature body')
        grouped.setdefault((kind, target), set()).update(offsets)
    return dict(body_size=size, crc32_prefix=prefix, crc32_body=body,
                references=[[kind, target, sorted(offsets)]
                            for (kind, target), offsets in sorted(grouped.items())],
                sources=sorted(set(source_ids)))


def native_key(record: dict) -> tuple:
    return (record['body_size'], record['sha256'], record['handler'], tuple(record['expected_words']))


def sdk_key(record: dict) -> tuple:
    return (record['body_size'], record['crc32_prefix'], record['crc32_body'],
            json.dumps(record['references'], separators=(',', ':')))


def reorganize(database: dict) -> dict:
    """Validate, deduplicate, sort, and rebuild library jump/call indexes."""
    if (not isinstance(database, dict) or type(database.get('format_version')) is not int
            or database['format_version'] != 1):
        raise RecompError('unsupported symbol database version')
    if set(database) != {'format_version', 'sources', 'symbols'}:
        raise RecompError('unknown or missing symbol database fields')
    sources, symbols = database.get('sources'), database.get('symbols')
    if not isinstance(sources, dict) or not isinstance(symbols, dict):
        raise RecompError('symbol database requires sources and symbols tables')
    for source_id, source in sources.items():
        name(source_id, 'source ID')
        if not isinstance(source, dict):
            raise RecompError('invalid symbol database source')
        for field in ('url', 'revision', 'sha256', 'license', 'license_sha256'):
            name(source.get(field), f'source {field}')
        for field in ('sha256', 'license_sha256'):
            if not re.fullmatch(r'[0-9a-f]{64}', source[field]):
                raise RecompError(f'invalid source {field}')
    result = {}
    native_owners = {}
    native_prefixes = {}
    for symbol, entry in symbols.items():
        name(symbol)
        if not isinstance(entry, dict):
            raise RecompError('invalid symbol entry')
        if set(entry) - {'native_signatures', 'sdk_signatures', 'callers', 'callees'}:
            raise RecompError('unknown symbol entry fields')
        natives, sdk = {}, {}
        for field in ('native_signatures', 'sdk_signatures'):
            if not isinstance(entry.get(field, []), list):
                raise RecompError(f'{field} must be a list')
        for raw in entry.get('native_signatures', []):
            record = normalize_native(raw)
            key = native_key(record)
            previous = natives.get(key)
            if previous:
                record['notes'] = sorted(set(previous.get('notes', []) + record.get('notes', [])))
                if not record['notes']:
                    del record['notes']
            natives[key] = record
            identity = (record['body_size'], record['sha256'])
            owner = (symbol, record['handler'])
            if identity in native_owners and native_owners[identity] != owner:
                raise RecompError('conflicting native bindings for a complete body')
            prefix = tuple(record['expected_words'])
            if identity in native_prefixes and native_prefixes[identity] != prefix:
                raise RecompError('conflicting native prefixes for a complete body')
            native_owners[identity] = owner
            native_prefixes[identity] = prefix
        for raw in entry.get('sdk_signatures', []):
            record = normalize_sdk(raw, sources)
            key = sdk_key(record)
            if key in sdk:
                record['sources'] = sorted(set(sdk[key]['sources'] + record['sources']))
            sdk[key] = record
        if natives or sdk:
            organized = {}
            if natives:
                organized['native_signatures'] = [natives[k] for k in sorted(natives)]
            if sdk:
                organized['sdk_signatures'] = [sdk[k] for k in sorted(sdk)]
            result[symbol] = organized
    callers, callees = {}, {}
    for symbol, entry in result.items():
        for signature in entry.get('sdk_signatures', []):
            for kind, target, _ in signature['references']:
                if kind == 'targ26':
                    callers.setdefault(target, set()).add(symbol)
                    callees.setdefault(symbol, set()).add(target)
    for symbol in set(callers) | set(callees):
        entry = result.setdefault(symbol, {})
        if symbol in callers:
            entry['callers'] = sorted(callers[symbol])
        if symbol in callees:
            entry['callees'] = sorted(callees[symbol])
    return dict(format_version=1, sources=dict(sorted(sources.items())),
                symbols=dict(sorted(result.items())))


def load_database(path: Path = DATABASE) -> dict:
    def unique_keys(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise RecompError(f'duplicate symbol database key: {key}')
            result[key] = value
        return result

    try:
        return reorganize(json.loads(path.read_text(), object_pairs_hook=unique_keys))
    except (OSError, ValueError) as error:
        raise RecompError(f'cannot read symbol database {path}: {error}') from error


def native_signatures(database: dict) -> tuple[dict, ...]:
    """Only measured SHA-256 bodies authorize native replacements, never SDK CRCs."""
    return tuple(dict(symbol=symbol, **{k: record[k] for k in
                 ('handler', 'body_size', 'sha256', 'expected_words')})
                 for symbol, entry in database['symbols'].items()
                 for record in entry.get('native_signatures', []))


def encode_database(database: dict) -> str:
    """Keep each signature on one line and group all variants under their symbol."""
    database = reorganize(database)
    lines = ['{', '  "format_version": 1,', '  "sources": ' +
             json.dumps(database['sources'], indent=2, ensure_ascii=True).replace('\n', '\n  ') + ',',
             '  "symbols": {']
    symbols = list(database['symbols'].items())
    for index, (symbol, entry) in enumerate(symbols):
        lines.append('    ' + json.dumps(symbol) + ': {')
        fields = list(entry.items())
        for field_index, (field, value) in enumerate(fields):
            suffix = ',' if field_index + 1 < len(fields) else ''
            if field.endswith('_signatures'):
                lines.append('      ' + json.dumps(field) + ': [')
                for item_index, item in enumerate(value):
                    lines.append('        ' + json.dumps(item, ensure_ascii=True) +
                                 (',' if item_index + 1 < len(value) else ''))
                lines.append('      ]' + suffix)
            else:
                lines.append('      ' + json.dumps(field) + ': ' + json.dumps(value) + suffix)
        lines.append('    }' + (',' if index + 1 < len(symbols) else ''))
    lines.extend(['  }', '}'])
    return '\n'.join(lines) + '\n'
