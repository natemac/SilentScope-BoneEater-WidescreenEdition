import copy
from pathlib import Path
import struct
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import sample_scope_input_state as probe


class Memory:
    base = 0x180000000
    def __init__(self):
        self.data = {}
        self.collections = 0
        self.mutate_second = None
        self.put(self.base + 0x13DCF58, '<Q', 0x100000)
        self.put(0x100000, '<Q', self.base + 0x10C5FA8)
        self.put(0x1012D8, '<2f', .5, .5)
        self.put(self.base + 0x13DCF88, '<Q', 0x200000)
        self.put(0x200000, '<2Q', self.base + 0x10C7350, 0x300000)
        self.put(0x200060, '<Q', 0x300000)
        self.put(0x300000, '<Q', self.base + 0x6FDF48)
        self.bytes(0x200170, bytes(0xDC))
        self.put(0x200174, '<f', 1)
        self.put(0x200190, '<2f', 1054.26477, 476.424011)
        self.put(0x2001C2, '<B', 1)
        self.put(0x200070, '<8f', 1, 2, 3, 1, 4, 5, 6, 1)
        self.put(self.base + 0x13DCF78, '<Q', 0x400000)
        self.put(0x400000, '<Q', self.base + 0x10CA298)
        self.put(0x400048, '<I', 3)
        self.put(0x4000E0, '<Q', 0x500000)
        self.put(0x500000, '<Q', self.base + 0x10CA388)
        self.put(0x5056A0, '<2B', 1, 0)
        self.put(self.base + 0x13DCF48, '<Q', 0x600000)
        self.put(0x600000, '<Q', 0x700000)
        self.put(0x700000, '<Q', self.base + 0x10CA5E0)
        self.put(0x700558, '<B', 0)
        self.put(self.base + 0x13DCED0, '<Q', 0x800000)
        self.bytes(0x800000, bytes.fromhex('12345678abcdef0033333333'))
        self.put(0x80000C, '<2f', 1064.81677, 478.534424)

    def bytes(self, address, values): self.data.update({address + i: v for i, v in enumerate(values)})
    def put(self, address, fmt, *values): self.bytes(address, struct.pack(fmt, *values))
    def read(self, address, size):
        if address == self.base + 0x13DCF58:
            self.collections += 1
            if self.collections == 2 and self.mutate_second: self.mutate_second(self)
        try: return bytes(self.data[address + i] for i in range(size))
        except KeyError: raise OSError('unreadable')
    def tick(self): return 123456


class Tests(unittest.TestCase):
    def test_readonly_native_halfway_state_not_a_computed_ray(self):
        reader = Memory(); before = copy.deepcopy(reader.data)
        row = probe.sample(reader)
        self.assertTrue(row['accepted'], row)
        self.assertTrue(row['dynamic_fields_equal'])
        self.assertEqual(row['after']['gains_xy'], [.5, .5])
        self.assertEqual(row['after']['state_1c4'], 0)
        self.assertEqual(row['after']['target_proximity_eligibility_56a0'], 1)
        self.assertEqual(row['after']['input_prefix_00_0b'], '12345678abcdef0033333333')
        self.assertEqual(row['before']['current_game_input_xy'], row['after']['current_game_input_xy'])
        self.assertNotIn('predicted_ray', row)
        self.assertLess(row['read_calls'], probe.MAX_READS)
        self.assertLess(row['read_bytes'], probe.MAX_BYTES)
        self.assertEqual(reader.data, before)

    def test_exact_owner_type_loaded_state_and_unreadable_rejected(self):
        for address, fmt, value in ((0x100000, '<Q', 1), (0x200000, '<Q', 1),
                (0x200008, '<Q', 0), (0x200060, '<Q', 0), (0x300000, '<Q', 1),
                (0x400000, '<Q', 1), (0x400048, '<I', 2), (0x500000, '<Q', 1),
                (0x700000, '<Q', 1), (Memory.base + 0x13DCED0, '<Q', 0)):
            with self.subTest(address=hex(address)):
                reader = Memory(); reader.put(address, fmt, value)
                row = probe.sample(reader)
                self.assertFalse(row['accepted'], row)
                self.assertNotIn('after', row)
        reader = Memory(); reader.data.clear()
        self.assertIn('unreadable', probe.sample(reader)['error'])

    def test_state_gain_and_owner_changes_during_pair_rejected(self):
        changes = ((0x1012D8, '<f', .7), (0x2001C4, '<I', 3), (0x2001C8, '<I', 1),
                   (0x2001C1, '<B', 1), (0x5056A0, '<B', 0), (0x5056A1, '<B', 1),
                   (0x700558, '<B', 1))
        for address, fmt, value in changes:
            reader = Memory()
            reader.mutate_second = lambda r, a=address, f=fmt, v=value: r.put(a, f, v)
            row = probe.sample(reader)
            self.assertFalse(row['accepted'], row)
            self.assertIn('gain/state changed', row['error'])
        reader = Memory()
        reader.put(0x301000, '<Q', reader.base + 0x6FDF48)
        reader.mutate_second = lambda r: r.put(0x200060, '<Q', 0x301000)
        self.assertIn('ownership changed', probe.sample(reader)['error'])

    def test_unpublished_scope_is_reported_before_any_dereference(self):
        reader = Memory(); reader.put(reader.base + 0x13DCF88, '<Q', 0)
        before = copy.deepcopy(reader.data)
        row = probe.sample(reader)
        self.assertFalse(row['accepted'])
        self.assertEqual(row['status'], 'identity_unavailable')
        self.assertEqual(row['unavailable_field'], 'ScopeCamera singleton')
        self.assertEqual(row['pointer'], '0x0')
        self.assertEqual((row['read_calls'], row['read_bytes']), (4, 32))
        self.assertNotIn('before', row)
        self.assertEqual(reader.data, before)

    def test_invalid_scope_pointer_is_distinct_from_unloaded(self):
        for value in (0x800000000000, 0xFFFFFFFFFFFFFFFF, 0x200001):
            reader = Memory(); reader.put(reader.base + 0x13DCF88, '<Q', value)
            row = probe.sample(reader)
            self.assertFalse(row['accepted'])
            self.assertEqual(row['status'], 'invalid_pointer')
            self.assertEqual(row['pointer'], hex(value))
            self.assertEqual((row['read_calls'], row['read_bytes']), (4, 32))

    def test_other_unpublished_chain_levels_keep_their_exact_field(self):
        cases = ((Memory.base + 0x13DCF58, 'Config singleton'),
                 (0x200008, 'ScopeCamera active native camera'),
                 (0x200060, 'ScopeCamera owned native camera'),
                 (Memory.base + 0x13DCF78, 'Battle manager singleton'),
                 (0x4000E0, 'GameUI::Scope owner'),
                 (Memory.base + 0x13DCF48, 'ScopeOffUI holder'),
                 (0x600000, 'ScopeOffUI owner'),
                 (Memory.base + 0x13DCED0, 'Game input singleton'))
        for address, name in cases:
            reader = Memory(); reader.put(address, '<Q', 0)
            row = probe.sample(reader)
            self.assertFalse(row['accepted'])
            self.assertEqual(row['status'], 'identity_unavailable')
            self.assertEqual(row['unavailable_field'], name)

    def test_scope_unpublishing_between_collects_rejects_complete_pair(self):
        reader = Memory()
        reader.mutate_second = lambda r: r.put(r.base + 0x13DCF88, '<Q', 0)
        row = probe.sample(reader)
        self.assertFalse(row['accepted'])
        self.assertEqual(row['status'], 'identity_unavailable')
        self.assertNotIn('before', row)
        self.assertNotIn('after', row)

    def test_dynamic_pair_movement_retained_without_atomicity_claim(self):
        reader = Memory()
        def advance(r):
            r.put(0x200180, '<f', 4)
            r.put(0x200184, '<2f', 1080, 480)
            r.put(0x80000C, '<2f', 1070, 479)
            r.put(0x200080, '<4f', 5, 6, 7, 1)
        reader.mutate_second = advance
        row = probe.sample(reader)
        self.assertTrue(row['accepted'], row)
        self.assertFalse(row['dynamic_fields_equal'])
        self.assertEqual(row['before']['fields']['timer_180'], 0)
        self.assertEqual(row['after']['fields']['timer_180'], 4)
        self.assertNotEqual(row['before']['current_game_input_xy'], row['after']['current_game_input_xy'])
        self.assertNotEqual(row['before']['scope_world_target_xyzw'], row['after']['scope_world_target_xyzw'])

    def test_finite_nondefault_gain_is_evidence_not_silently_replaced(self):
        reader = Memory(); reader.put(0x1012D8, '<2f', 1, -.25)
        row = probe.sample(reader)
        self.assertTrue(row['accepted'], row)
        self.assertEqual(row['after']['gains_xy'], [1, -.25])

    def test_nonfinite_and_unsupported_flags_modes_rejected(self):
        for address, fmt, value in ((0x1012D8, '<f', float('nan')),
                (0x200184, '<f', float('inf')), (0x80000C, '<f', float('nan')),
                (0x200080, '<f', float('inf')), (0x200248, '<B', 2),
                (0x5056A0, '<B', 2), (0x5056A1, '<B', 2), (0x700558, '<B', 2),
                (0x2001C4, '<I', 5), (0x2001C8, '<I', 4)):
            reader = Memory(); reader.put(address, fmt, value)
            self.assertFalse(probe.sample(reader)['accepted'])

    def test_capture_start_deadline_count_and_no_catchup(self):
        for cost in (0, .35):
            now = [0.0]; starts = []
            def clock(): return now[0]
            def sleep(seconds): now[0] += seconds
            def sample(reader):
                starts.append(now[0]); now[0] += cost
                return {'accepted': True}
            with patch.object(probe, 'sample', sample):
                rows = probe.capture_rows(None, clock=clock, sleep=sleep)
            self.assertGreater(len(rows), 0)
            self.assertLessEqual(len(rows), probe.LIMIT)
            self.assertTrue(all(t < probe.MAX_SECONDS for t in starts))
            self.assertTrue(all(b - a >= probe.INTERVAL - 1e-10 for a, b in zip(starts, starts[1:])))
            self.assertLessEqual(now[0], probe.MAX_SECONDS + cost)


if __name__ == '__main__': unittest.main()
