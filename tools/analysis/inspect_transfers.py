"""Compare cartridge-bound Debug edge traces with static CPU transfer coverage."""
import argparse
from collections import Counter, defaultdict
import csv
import json
from pathlib import Path

from common import RecompError, write_json


def read_edges(path, report):
    header = f"# v64recomp-edges-v1 {report['cartridge_sha1']} {report['section_layout_sha256']}"
    with path.open(newline='') as stream:
        if stream.readline().rstrip() != header:
            raise RecompError(f'{path}: different cartridge, section layout or trace version')
        reader = csv.DictReader(stream)
        if reader.fieldnames != ['section', 'pc', 'kind', 'target_section', 'target']:
            raise RecompError(f'{path}: invalid edge columns')
        for row in reader:
            try:
                section, pc = int(row['section']), int(row['pc'], 0)
                sid = int(row['target_section']) if row['target_section'] else None
                target = int(row['target'], 0)
                kind = row['kind']
                if (None in row or kind not in ('call', 'relocated-call', 'return', 'tail', 'jump')
                        or not 0 <= pc <= 0xffffffff or not 0 <= target <= 0xffffffff
                        or section < 0 or sid is not None and sid < 0):
                    raise ValueError()
            except (TypeError, ValueError) as exc:
                raise RecompError(f'{path}:{reader.line_num + 1}: invalid edge') from exc
            yield section, pc, kind, sid, target


def compare(report, traces):
    if 'transfer_sites' not in report:
        raise RecompError('regenerate coverage.json with transfer-site reporting')
    observed = defaultdict(set)
    for trace in traces:
        for section, pc, kind, target_section, target in read_edges(trace, report):
            observed[section, pc].add((kind, target_section, target))
    rows = []
    functions = {(f['section'], f['vram']): f['status'] for f in report['functions'] if f['selected']}
    for site in report['transfer_sites']:
        key = site['section'], site['pc']
        known = {(t['section'], t['pc']) for t in site['targets']}
        edges = []
        for kind, sid, target in sorted(observed.pop(key, ()), key=edge_order):
            if sid is None:
                evidence = 'unresolved-section'
            elif kind != site['kind']:
                evidence = 'review'
            elif (sid, target) in known:
                evidence = 'static-target'
            elif site['status'] in ('checked', 'bounded'):
                evidence = 'review'
            elif kind == 'return' and site['kind'] == 'return':
                evidence = 'observed-return'
            elif kind in ('call', 'relocated-call', 'tail') and functions.get((sid, target)) in ('ready', 'hooked'):
                evidence = 'registered-target'
            else:
                evidence = 'review'
            edges.append(dict(kind=kind, section=sid, pc=target, evidence=evidence))
        rows.append(dict(site, observed=edges))
    for (section, pc), edges in sorted(observed.items()):
        rows.append(dict(section=section, pc=pc, kind='unlisted', status='unknown', targets=[], observed=[
            dict(kind=k, section=s, pc=t, evidence='unlisted-site') for k, s, t in sorted(edges, key=edge_order)]))
    return rows


def edge_order(edge):
    kind, section, pc = edge
    return kind, -1 if section is None else section, pc


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('coverage', type=Path, help='generated coverage.json')
    parser.add_argument('trace', nargs='*', type=Path, help='one or more --trace-edges captures')
    parser.add_argument('--site', type=lambda s: int(s, 0), help='show a specific instruction PC')
    parser.add_argument('--section', type=int, help='restrict to a source section ID')
    parser.add_argument('--output', type=Path, help='write all matching sites and targets as JSON')
    args = parser.parse_args()
    try:
        report = json.loads(args.coverage.read_text())
        rows = [row for row in compare(report, args.trace)
                if (args.site is None or row['pc'] == args.site)
                and (args.section is None or row['section'] == args.section)]
        counts = Counter(row['status'] for row in rows)
        evidence = Counter(edge['evidence'] for row in rows for edge in row['observed'])
        print(f'{len(rows)} transfer sites: {dict(counts)}')
        print(f'{sum(bool(row["observed"]) for row in rows)} exercised sites, {sum(evidence.values())} distinct edges: {dict(evidence)}')
        if args.site is not None:
            for row in rows:
                print(json.dumps(row, indent=2))
        if args.output:
            write_json(args.output, dict(cartridge_sha1=report['cartridge_sha1'], section_layout_sha256=report['section_layout_sha256'],
                                         sites=rows))
    except (RecompError, KeyError, ValueError, OSError, csv.Error) as exc:
        parser.exit(1, f'transfer report: {exc}\n')


if __name__ == '__main__':
    main()
