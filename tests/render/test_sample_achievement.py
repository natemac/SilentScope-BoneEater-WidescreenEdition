import copy
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import sample_achievement as probe
from test_sample_kill_camera import Memory as BaseMemory


class Memory(BaseMemory):
    def __init__(self):
        super().__init__()
        self.collections = 0
        self.mutate_second = None
        for address in (0x700000, 0x800000, 0x900000):
            self.put(address + 0x48, '<I', 3)
        self.put(0x9000A0, '<Q', 0xC00000)
        self.put(0x8004C0, '<Q', 0xD02000)
        for leaf, marker, root, group in ((0xA00000, 0xC01000, 0xC00000, 8),
                                         (0xB00000, 0xD01000, 0xD00000, 9)):
            self.node(root, 'root', group, 0)
            self.put(root, '<Q', self.base + 0x10CEB48)
            self.node(marker, 'root_marker', group, 0)
            self.put(marker + 0x10, '<Q', root)
            self.put(marker + 0x18, '<Q', leaf)
            self.put(root + 0x18, '<Q', marker)
            self.put(leaf + 0x10, '<Q', marker)
            for address in (leaf, marker, root):
                self.put(address + 0xA0, '<4f', 1, 1, 1, 1)
                self.put(address + 0xC9, '<B', 1)
        self.node(0xD02000, 'lcd_bt_enemyDmg_root', 9, 0)
        self.put(0xD02010, '<Q', 0xD00000)
        for leaf, size in ((0xA00000, (800, 82)), (0xB00000, (768, 85))):
            self.put(leaf + 0x98, '<2f', *size)
        for leaf, source, camera, index in ((0xA00000, 0xE00000, 0xF00000, 0),
                                          (0xB00000, 0xE01000, 0xF01000, 2)):
            self.put(leaf + 0x110, '<Q', source)
            self.block(source, 0x580)
            self.put(source, '<Q', self.base + 0x10CC0D8)
            self.put(source + 0x1A8, '<Q', camera)
            self.put(source + 0x570, '<I', index)
            self.put(self.base + 0x13DB5A0 + index * 8, '<Q', camera)
            self.put(camera, '<Q', self.base + 0x6FDF48)
            material, texture = source + 0x600, source + 0x700
            self.block(material, 0x60)
            self.block(texture, 0x68)
            self.put(source + 0x308, '<Q', material)
            self.put(material, '<Q', self.base + 0x706570)
            self.put(material + 0x50, '<Q', texture)
            self.put(texture, '<Q', 0x12340000 + index)
            self.put(texture + 0x18, '<2H', 1024, 2048)
        self.pose(0, 192, -85)

    def pose(self, active, elapsed, y):
        self.put(0x8004A0, '<fB', elapsed, active)
        self.put(0xA00060, '<2f', 0, y)
        self.put(0xB00060, '<2f', 80, y)
        for leaf in (0xA00000, 0xB00000):
            self.put(leaf + 0x70, '<2f', 0, y)

    def read(self, address, size):
        if address == self.base + 0x13DCF78:
            self.collections += 1
            if self.collections == 2 and self.mutate_second:
                self.mutate_second(self)
        return super().read(address, size)


class Tests(unittest.TestCase):
    def test_idle_hold_slide_pair_and_readonly_budget(self):
        for active, elapsed, y in ((0, 192, -85), (1, 0, 0), (1, 180, 0), (1, 185, -42.5)):
            reader = Memory(); reader.pose(active, elapsed, y)
            before = copy.deepcopy(reader.data)
            row = probe.sample(reader)
            self.assertTrue(row['accepted'], row)
            self.assertTrue(row['expected_routing'])
            self.assertEqual(row['nodes']['nick']['cached_xy'], [0, y])
            self.assertEqual(row['nodes']['title']['local_xy'], [80, y])
            self.assertEqual(row['local_y_difference'], 0)
            self.assertEqual(row['active'], active)
            self.assertEqual(row['sources']['title']['display_selector_570'], 2)
            self.assertLess(row['read_calls'], probe.MAX_READS)
            self.assertLess(row['read_bytes'], probe.MAX_BYTES)
            self.assertEqual(reader.data, before)

    def test_unexpected_but_valid_routing_is_reported(self):
        reader = Memory()
        reader.put(0xE00570, '<I', 2)
        reader.put(0xE001A8, '<Q', 0xF01000)
        row = probe.sample(reader)
        self.assertTrue(row['accepted'], row)
        self.assertFalse(row['expected_routing'])

    def test_exact_ancestor_owner_type_name_group_and_extent_rejections(self):
        cases = ((0x700048, '<I', 2), (0x900000, '<Q', 1), (0xC00000, '<Q', 1),
                 (0xC00010, '<Q', 0xC01000), (0xA00010, '<Q', 0xD01000),
                 (0xC01040, '<B', ord('X')), (0xA0003C, '<I', 9),
                 (0xD02010, '<Q', 0xC00000), (0x9000A0, '<Q', 0xD00000),
                 (0xA00098, '<f', 768), (0xE00600, '<Q', 1),
                 (0xE00718, '<H', 0), (0xF00000, '<Q', 0x123))
        for address, fmt, value in cases:
            with self.subTest(address=hex(address)):
                reader = Memory(); reader.put(address, fmt, value)
                row = probe.sample(reader)
                self.assertFalse(row['accepted'], row)
                self.assertNotIn('nodes', row)

    def test_identity_and_activation_transition_rejected(self):
        for mutation in (lambda r: r.put(0xE00700, '<Q', 0xABC),
                         lambda r: r.put(0xC01028, '<Q', 0x1234),
                         lambda r: r.put(0x8004A4, '<B', 1),
                         lambda r: r.put(0xE00570, '<I', 2)):
            reader = Memory(); reader.mutate_second = mutation
            self.assertFalse(probe.sample(reader)['accepted'])

    def test_timer_can_advance_without_claiming_pose_atomicity(self):
        reader = Memory(); reader.pose(1, 20, 0)
        reader.mutate_second = lambda r: r.put(0x8004A0, '<f', 24)
        row = probe.sample(reader)
        self.assertTrue(row['accepted'], row)
        self.assertEqual(row['elapsed_native_units'], 20)
        self.assertEqual(row['elapsed_after_native_units'], 24)

    def test_malformed_floats_flags_and_unreadable_process(self):
        for address, fmt, value in ((0xA00070, '<f', float('nan')),
                                    (0x8004A0, '<f', -1), (0x8004A0, '<f', float('inf')),
                                    (0x8004A4, '<B', 2), (0xC010C9, '<B', 2),
                                    (0xE001A8, '<Q', 0), (0x600588, '<f', 0)):
            reader = Memory(); reader.put(address, fmt, value)
            self.assertFalse(probe.sample(reader)['accepted'])
        reader = Memory(); reader.data.clear()
        row = probe.sample(reader)
        self.assertFalse(row['accepted'])
        self.assertIn('unreadable', row['error'])

    def test_read_budget_is_hard_bounded(self):
        reader = Memory(); bounded = probe.BoundedReads(reader)
        for _ in range(probe.MAX_READS): bounded.read(0xA00000, 1)
        with self.assertRaisesRegex(ValueError, 'budget'): bounded.read(0xA00000, 1)
        self.assertEqual(bounded.count, probe.MAX_READS)
        bounded = probe.BoundedReads(reader)
        reader.read = lambda address, size: bytes(size)
        for _ in range(probe.MAX_BYTES // 4096): bounded.read(0xA00000, 4096)
        with self.assertRaisesRegex(ValueError, 'budget'): bounded.read(0xA00000, 1)
        self.assertEqual(bounded.bytes, probe.MAX_BYTES)

    def test_capture_count_deadline_and_no_catchup_burst(self):
        for cost in (0, 0.35):
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
