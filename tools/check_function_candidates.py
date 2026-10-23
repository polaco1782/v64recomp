#!/usr/bin/env python3
"""ROM-free regressions for indexed callback evidence in the heuristic scanner."""
import copy
import struct
import unittest

from inspect_function_candidates import inspect_candidates
from metadata_candidates import indexed_call_tables

BASE, TABLE, LEAF = 0x80010000, 0x80012000, 0x80010100


def lui(r, value):
    return 0x3c000000 | r << 16 | value


def addu(dst, left, right):
    return left << 21 | right << 16 | dst << 11 | 0x21


def sll(dst, src, shift):
    return src << 16 | dst << 11 | shift << 6


def lw(dst, base, offset):
    return 0x8c000000 | base << 21 | dst << 16 | (offset & 65535)


def jalr(src):
    return src << 21 | 31 << 11 | 9


JR_RA = 0x03e00008
# The zero selector bypasses a word-indexed table, as in Conker's dispatcher.
DISPATCH = [lui(25, 0x8001), 0x10800005, sll(8, 4, 2), addu(25, 25, 8),
            lw(25, 25, 0x2000), jalr(25), 0, JR_RA, 0]


def fixture(words):
    rom = bytearray(0x260)
    for n, word in enumerate(words):
        struct.pack_into('>I', rom, 0x40 + n * 4, word)
    struct.pack_into('>4I', rom, 0x140, 0x24020001, JR_RA, 0, 0)
    struct.pack_into('>4I', rom, 0x240, 0, LEAF, 0, 0xdeadbeef)
    function = dict(name='dispatcher', section=0, vram=BASE, rom_offset=0x40,
                    size=len(words)*4, origin='checked-root',
                    instruction_addresses=list(range(BASE, BASE+len(words)*4, 4)))
    metadata = dict(game='synthetic', rom_sha1='fixture', functions=[function],
                    sections=[dict(id=0, vram=BASE, rom_offset=0x40, size=0x200),
                              dict(id=1, vram=TABLE, rom_offset=0x240, size=0x20)])
    return bytes(rom), metadata, function


class IndexedCallbackTests(unittest.TestCase):
    def uses(self, words):
        rom, _, function = fixture(words)
        return indexed_call_tables(rom, function)

    def test_branch_delay_index_and_null_slot(self):
        self.assertEqual(self.uses(DISPATCH), [dict(pc=BASE+20, expected_word=jalr(25),
                         table_vram=TABLE, load_pc=BASE+16)])

    def test_target_is_read_before_call_delay_slot(self):
        words = DISPATCH.copy()
        words[6] = addu(25, 0, 0)
        self.assertEqual(len(self.uses(words)), 1)

    def test_register_copy(self):
        words = DISPATCH[:5] + [addu(9, 25, 0), jalr(9), 0, JR_RA, 0]
        self.assertEqual(self.uses(words)[0]['pc'], BASE+24)

    def test_overwritten_target(self):
        self.assertFalse(self.uses(DISPATCH[:5] + [addu(25, 0, 0), jalr(25), 0, JR_RA, 0]))

    def test_nonword_stride(self):
        words = DISPATCH.copy()
        words[2] = sll(8, 4, 1)
        self.assertFalse(self.uses(words))

    def test_conflicting_predecessors_remove_early_evidence(self):
        words = [0x10800004, 0, lui(25, 0x8001), 0x10000002, 0,
                 lui(25, 0x8002), sll(8, 4, 2), addu(25, 25, 8),
                 lw(25, 25, 0x2000), jalr(25), 0, JR_RA, 0]
        self.assertFalse(self.uses(words))

    def test_call_clobbers_volatile_base(self):
        words = [lui(25, 0x8001), 0x0c004040, 0, sll(8, 4, 2), addu(25, 25, 8),
                 lw(25, 25, 0x2000), jalr(25), 0, JR_RA, 0]
        self.assertFalse(self.uses(words))

    def test_likely_branch_annuls_base_write(self):
        words = [0x50800007, lui(25, 0x8001), sll(8, 4, 2), addu(25, 25, 8),
                 lw(25, 25, 0x2000), jalr(25), 0, JR_RA, 0]
        self.assertFalse(self.uses(words))

    def test_report_leaf_priority_and_no_metadata_mutation(self):
        rom, metadata, _ = fixture(DISPATCH)
        before = copy.deepcopy(metadata)
        report = inspect_candidates(rom, metadata)
        candidate = next(c for c in report['candidates'] if c['vram'] == LEAF)
        self.assertEqual(candidate['confidence'], 'likely')
        self.assertFalse(candidate['body']['stack_prologue'])
        ref = next(r for r in candidate['references'] if r['kind']=='indexed-table-call')
        self.assertEqual((ref['table_vram'], ref['table_index'], ref['pc']), (TABLE, 1, BASE+20))
        table = report['indirect_tables'][0]
        self.assertFalse(table['bounds_verified'])
        self.assertEqual(table['stop_reason'], 'non-pointer-word')
        self.assertEqual(metadata, before)

    def test_replaced_body_is_not_an_active_producer(self):
        rom, metadata, function = fixture(DISPATCH)
        function['origin'] = 'native-replacement'
        self.assertFalse(inspect_candidates(rom, metadata)['indirect_tables'])

    def test_checked_switch_words_excluded(self):
        rom, metadata, function = fixture(DISPATCH)
        function['indirect_transfers'] = [dict(pc=BASE+20, kind='jump', table_vram=TABLE,
                                             table_count=3, targets=[LEAF])]
        report = inspect_candidates(rom, metadata)
        self.assertFalse(report['indirect_tables'])
        self.assertFalse(any(c['vram']==LEAF for c in report['candidates']))

    def test_ambiguous_table_mapping_is_skipped(self):
        rom, metadata, _ = fixture(DISPATCH)
        metadata['sections'].append(dict(id=2, vram=TABLE, rom_offset=0x240, size=0x20))
        self.assertFalse(inspect_candidates(rom, metadata)['indirect_tables'])


if __name__ == '__main__':
    unittest.main()
