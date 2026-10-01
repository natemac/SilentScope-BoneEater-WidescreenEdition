import math
from pathlib import Path
import struct
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import sample_framing_pose as probe


def pose(angle=0.0, position=(10.0, 20.0, 30.0), forward=(0.0, 0.0, 1.0)):
    # Independent fixture basis: Y up, horizontal unit forward, 100-unit target.
    p = list(position)
    t = [p[i] + forward[i]*100 for i in range(3)]
    n = [p[i] + forward[i]*100*math.cos(angle) + (100*math.sin(angle) if i == 1 else 0)
         for i in range(3)]
    return {'position': p + [1.0], 'wrapper_target': t + [1.0],
            'up': [0.0, 1.0, 0.0, 0.0], 'native_target': n + [1.0], 'roll_radians': 0.0}


class Memory:
    base = 0x180000000
    main, native, target = 0x300000, 0x400000, 0x500000
    manager, scope, scope_camera, scope_target = 0x600000, 0x700000, 0x800000, 0x900000
    source, source_target = 0xB00000, 0xC00000

    def __init__(self, angle=0.0):
        self.bytes = {}
        self.wrapper_reads = 0
        self.mutate_second_pose = False
        self.clear_second_anchor = False
        self.q(self.base + 0x13DB588, self.main)
        self.q(self.main, self.base + 0x10C4B60)
        self.q(self.main + 8, self.native)
        self.q(self.main + 0x60, self.native)
        self.q(self.base + 0x12E1930, self.manager)
        self.q(self.manager + 0x340, self.native)
        for address, target in ((self.native, self.target), (self.scope_camera, self.scope_target),
                                (self.source, self.source_target)):
            self.q(address, self.base + 0x6FDF48)
            self.q(address + 0x1B0, target)
        self.q(self.target, self.base + 0x6FDC58)
        self.q(self.base + 0x6FDC58 + 0xA0, self.base + 0x66F50)
        self.pack(self.native + 0xEB4, '<2f', 1920, 1080)
        self.q(self.base + 0x13DCF88, self.scope)
        self.q(self.scope, self.base + 0x10C7350)
        self.q(self.scope + 8, self.scope_camera)
        self.q(self.scope + 0x60, self.scope_camera)
        self.q(self.main + 0x138, 0xA00000)
        self.q(0xA00000 + 0x2C0, 0xA10000)
        self.q(0xA10000, 0xA20000)
        self.q(0xA20000 + 0x38, self.source)
        self.q(self.base + 0x13DCF48, 0xD00000)
        self.q(0xD00000, 0xD10000)
        self.q(0xD10000, self.base + 0x10CA5E0)
        self.q(self.base + 0x13DCED0, 0xE00000)
        self.pack(0xE00000 + 0x0C, '<2f', 960, 540)
        self.set_pose(pose(angle))

    def pack(self, address, fmt, *values):
        for offset, byte in enumerate(struct.pack(fmt, *values)):
            self.bytes[address + offset] = byte

    def q(self, address, value): self.pack(address, '<Q', value)

    def set_pose(self, value):
        for field, offset in (('position', 0x10), ('wrapper_target', 0x20), ('up', 0x30)):
            self.pack(self.main + offset, '<4f', *value[field])
        self.pack(self.main + 0x40, '<2f', 1.0, value['roll_radians'])
        self.pack(self.target + 0x80, '<4f', *value['native_target'])

    def read(self, address, size):
        if address == self.base + 0x13DB588 and self.clear_second_anchor and self.wrapper_reads:
            return struct.pack('<Q', 0)
        if address == self.main and size == 0x68:
            self.wrapper_reads += 1
            if self.wrapper_reads == 2 and self.mutate_second_pose:
                self.pack(self.main + 0x10, '<f', 11.0)
        return bytes(self.bytes.get(address + offset, 0) for offset in range(size))

    def tick(self): return 1000


class FramingPoseTests(unittest.TestCase):
    def test_known_signed_angles_and_no_forced_policy_clamp(self):
        for degrees in (0.0, 6.0, -6.0, 8.0, -8.0, 12.0):
            result = probe.observed_pitch(pose(math.radians(degrees)))
            self.assertAlmostEqual(result['observed_pitch_degrees'], degrees, places=10)
            self.assertEqual(result['within_default_policy_excursion'], abs(degrees) <= 8)
            self.assertLess(result['direction_residual'], 1e-12)

    def test_fresh_scripted_baseline_while_difference_held(self):
        for origin, direction in (((10, 20, 30), (0, 0, 1)), ((100, -25, 70), (1, 0, 0)),
                                  ((-300, 88, 105), (0.6, 0, 0.8))):
            result = probe.observed_pitch(pose(math.radians(6), origin, direction))
            self.assertAlmostEqual(result['observed_pitch_degrees'], 6, places=9)

    def test_real_float_packing_and_repeat_snapshot(self):
        for degrees in (0, 6, -6):
            row = probe.sample(Memory(math.radians(degrees)))
            self.assertTrue(row['accepted'], row)
            self.assertTrue(row['repeated_identity_equal'] and row['repeated_pose_equal'])
            self.assertAlmostEqual(row['observed_pitch_degrees'], degrees, places=4)
            self.assertLess(row['read_calls'], probe.MAX_READS)
            self.assertLess(row['read_bytes'], probe.MAX_BYTES)

    def test_direction_and_distance_must_both_match(self):
        for kind in ('sideways', 'distance'):
            value = pose(math.radians(6))
            if kind == 'sideways':
                value['native_target'][0] += 5
            else:
                value['native_target'][:3] = [value['position'][i] +
                    1.1*(value['native_target'][i] - value['position'][i]) for i in range(3)]
            with self.assertRaisesRegex(ValueError, 'pure pitch'):
                probe.observed_pitch(value)

    def test_nonfinite_roll_degenerate_and_pole_rejections(self):
        cases = []
        value = pose(); value['roll_radians'] = .1; cases.append(value)
        value = pose(); value['wrapper_target'] = value['position'][:]; cases.append(value)
        value = pose(); value['up'] = [0, 0, 1, 0]; cases.append(value)
        value = pose(); value['up'] = [0, .001, 1, 0]
        value['native_target'] = pose(math.radians(6))['native_target']; cases.append(value)
        for value in cases:
            with self.assertRaises(probe.UnsupportedPose): probe.observed_pitch(value)
        value = pose(); value['native_target'][0] = float('nan')
        with self.assertRaises(ValueError): probe.observed_pitch(value)
        value = pose(); value['wrapper_target'][3] = 0.0; value['native_target'][3] = -0.0
        with self.assertRaisesRegex(ValueError, 'W changed'): probe.observed_pitch(value)

    def test_exact_identity_dimensions_and_alias_rejections(self):
        changes = (
            lambda m: m.q(m.main, m.base + 0x10C7350),
            lambda m: m.q(m.manager + 0x340, m.scope_camera),
            lambda m: m.pack(m.native + 0xEB4, '<2f', 800, 1280),
            lambda m: m.q(m.native + 0x1B8, 0xFF0000),
            lambda m: m.q(m.target, m.base + 0x6FDF48),
            lambda m: m.q(m.base + 0x6FDC58 + 0xA0, m.base + 0x66F51),
            lambda m: m.q(m.scope_camera + 0x1B0, m.target),
            lambda m: m.q(m.source + 0x1B0, m.target),
            lambda m: m.q(m.base + 0x13DCF88, 0),
            lambda m: m.q(m.base + 0x13DB588, 0x800000000000),
        )
        for change in changes:
            m = Memory(); change(m)
            self.assertFalse(probe.sample(m)['accepted'])

    def test_changed_pose_or_unpublished_identity_rejected(self):
        m = Memory(); m.mutate_second_pose = True
        row = probe.sample(m)
        self.assertFalse(row['accepted'])
        self.assertTrue(row['repeated_identity_equal'])
        self.assertFalse(row['repeated_pose_equal'])
        m = Memory(); m.clear_second_anchor = True
        row = probe.sample(m)
        self.assertFalse(row['accepted'])
        self.assertIn('Active wrapper', row['error'])

    def test_unsupported_pose_is_not_reported_as_adapter_failure(self):
        m = Memory(); value = pose(); value['roll_radians'] = .2; m.set_pose(value)
        row = probe.sample(m)
        self.assertFalse(row['accepted'])
        self.assertEqual(row['status'], 'unsupported_pose')
        self.assertIn('before', row)
        self.assertNotIn('observed_pitch_degrees', row)

    def test_context_only_mutation_is_not_pose_rejection(self):
        m = Memory(math.radians(2))
        original_collect = probe.collect
        calls = [0]
        def changing_context(reader):
            calls[0] += 1
            if calls[0] == 2:
                m.pack(0xE00000, '<I', 4)
            return original_collect(reader)
        with patch.object(probe, 'collect', side_effect=changing_context):
            row = probe.sample(m)
        self.assertTrue(row['accepted'], row)
        self.assertEqual(row['status'], 'observed_pure_pitch')
        self.assertTrue(row['repeated_identity_equal'])
        self.assertTrue(row['repeated_pose_equal'])
        self.assertFalse(row['repeated_context_equal'])
        self.assertEqual(row['before']['context']['native_held_bits'], 0)
        self.assertEqual(row['after']['context']['native_held_bits'], 4)

    def test_budgets_and_partial_read_errors(self):
        with patch.object(probe, 'collect', side_effect=lambda r: [r.read(1, 1) for _ in range(257)]):
            row = probe.sample(Memory())
            self.assertFalse(row['accepted']); self.assertEqual(row['read_calls'], 256)
        with patch.object(probe, 'collect', side_effect=lambda r: [r.read(1, 4096) for _ in range(17)]):
            row = probe.sample(Memory())
            self.assertFalse(row['accepted']); self.assertEqual(row['read_bytes'], 65536)
        with patch.object(probe, 'collect', side_effect=OSError('short read')):
            row = probe.sample(Memory())
            self.assertFalse(row['accepted']); self.assertEqual(row['error'], 'short read')

    def test_schedule_bounds_and_no_catchup(self):
        for slow in (False, True):
            now, starts = [0.0], []
            def clock(): return now[0]
            def sleep(seconds): now[0] += seconds
            def observation(reader):
                starts.append(now[0])
                now[0] += .65 if slow and len(starts) % 10 == 0 else .001
                return {'accepted': True}
            with patch.object(probe, 'sample', side_effect=observation):
                rows = probe.capture_rows(None, clock=clock, sleep=sleep)
            self.assertLessEqual(len(rows), 150)
            self.assertTrue(all(t < 30 for t in starts))
            self.assertTrue(all(b-a >= .2-1e-10 for a, b in zip(starts, starts[1:])))
            self.assertEqual([r['index'] for r in rows], list(range(len(rows))))


if __name__ == '__main__': unittest.main()
