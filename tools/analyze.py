#!/usr/bin/env python3
"""Inspect a ROM, generated CPU code, or a runtime failure."""
import argparse
import runpy
import sys

COMMANDS = {
    'address': 'inspect_address',
    'callbacks': 'inspect_callbacks',
    'candidates': 'inspect_function_candidates',
    'crash': 'inspect_crash',
    'diagnose': 'diagnose_rom',
    'disassemble': 'disassemble_function',
    'dump-cpu': 'dump_cpu_code',
    'export-rom': 'export_analysis_rom',
    'probe-roots': 'probe_function_roots',
    'review-candidates': 'review_candidates',
    'review-discovery': 'review_discovery',
    'rom': 'inspect_rom',
    'rsp-task': 'inspect_rsp_task',
    'sdk-symbols': 'inspect_sdk_symbols',
}


def main():
    parser = argparse.ArgumentParser(description=__doc__, usage='%(prog)s COMMAND [options]')
    parser.add_argument('command', choices=COMMANDS, help='use COMMAND --help for options')
    command = parser.parse_args(sys.argv[1:2]).command
    sys.argv[:2] = [f'{sys.argv[0]} {command}']
    runpy.run_module('analysis.' + COMMANDS[command], run_name='__main__')


if __name__ == '__main__':
    main()
