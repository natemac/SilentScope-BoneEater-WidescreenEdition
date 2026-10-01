import copy
from datetime import datetime, timedelta, timezone
import math
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import analyze_framing_acceptance as analyzer
from test_sample_framing_pose import Memory, pose
import sample_framing_pose as sampler

EXPECTED = dict(pid=123, process_created_filetime=456, exe_sha256='a'*64, module_sha256='b'*64)


def capture(gated=0, stationary=False, angle=2):
    rows = []
    start = datetime(2026, 9, 27, tzinfo=timezone.utc)
    for i in range(7):
        m = Memory()
        m.pack(0xE00000, '<I', 4)
        m.pack(0xD10000 + 0x558, '<B', gated)
        m.set_pose(pose(math.radians(angle), position=(10 + (0 if stationary else max(0, i-2)), 20, 30)))
        row = sampler.sample(m)
        row['utc'] = (start + timedelta(milliseconds=200*i)).isoformat()
        row['tick_ms'] = 1000 + 200*i
        rows.append(row)
    return dict(metadata=EXPECTED.copy(), rows=rows)


class AcceptanceTests(unittest.TestCase):
    def verdict(self, doc):
        return analyzer.analyze(doc, EXPECTED)

    def test_valid_active_motion(self):
        r = self.verdict(capture())
        self.assertTrue(r['active_scope_pass'], r)
        self.assertFalse(r['script_gated_hold_pass'])
        self.assertEqual(r['intervals'][0]['baseline_rows'], [0, 1, 2])
        self.assertAlmostEqual(r['intervals'][0]['maximum_displacement'], 4)

    def test_gated_is_separate(self):
        r = self.verdict(capture(gated=1))
        self.assertFalse(r['active_scope_pass'])
        self.assertTrue(r['script_gated_hold_pass'])

    def test_neutral_stationary_and_insufficient_baseline(self):
        for d in (capture(angle=0), capture(stationary=True), capture(angle=.9)):
            self.assertFalse(self.verdict(d)['active_scope_pass'])
        d = capture(); del d['rows'][0]
        self.assertFalse(self.verdict(d)['active_scope_pass'])

    def test_context_only_change_even_if_flag_lies(self):
        for flag in (True, False):
            d = capture()
            d['rows'][3]['after']['context']['native_held_bits'] = 0
            d['rows'][3]['repeated_context_equal'] = flag
            self.assertFalse(self.verdict(d)['active_scope_pass'])

    def test_rejection_gap_reversal_identity_and_gate_split(self):
        for mutation in ('rejected', 'tickgap', 'tickreverse', 'utc_reverse', 'utc_gap', 'identity', 'gating'):
            d = capture(); r = d['rows'][3]
            if mutation == 'rejected': r['accepted'] = False
            elif mutation == 'tickgap': r['tick_ms'] += 1000
            elif mutation == 'tickreverse': r['tick_ms'] = 900
            elif mutation == 'utc_reverse': r['utc'] = d['rows'][0]['utc']
            elif mutation == 'utc_gap': r['utc'] = '2026-09-27T00:00:10+00:00'
            else:
                for side in ('before', 'after'):
                    if mutation == 'identity': r[side]['identity']['main_target'] = '0x1234560'
                    else: r[side]['context']['script_scope_off'] = 1
            self.assertFalse(self.verdict(d)['active_scope_pass'], mutation)

    def test_missing_nonfinite_wrong_types_and_false_excursion(self):
        for mutation in ('missing', 'nan', 'booltick', 'released', 'excursion', 'residual', 'fakepitch'):
            d = capture(); r = d['rows'][3]
            if mutation == 'missing': del r['repeated_context_equal']
            elif mutation == 'nan': r['observed_pitch_degrees'] = float('nan')
            elif mutation == 'booltick': r['tick_ms'] = True
            elif mutation == 'excursion': r['within_default_policy_excursion'] = False
            elif mutation == 'residual': r['plane_residual'] = -1
            elif mutation == 'fakepitch': r['observed_pitch_degrees'] = 4
            else:
                for side in ('before', 'after'): r[side]['context']['native_held_bits'] = 0
            self.assertFalse(self.verdict(d)['active_scope_pass'], mutation)

    def test_real_pitch_deviation_fails_retention(self):
        d = capture(); other = capture(angle=2.2)
        for i in range(3, 7): d['rows'][i] = other['rows'][i]
        self.assertFalse(self.verdict(d)['active_scope_pass'])

    def test_metadata_requires_nominated_expectations(self):
        for key in EXPECTED:
            d = capture(); del d['metadata'][key]
            self.assertFalse(self.verdict(d)['metadata_matches_supplied_expectations'])
        d = capture(); d['metadata']['pid'] = 124
        self.assertFalse(self.verdict(d)['active_scope_pass'])

    def test_motion_threshold_excludes_float_jitter(self):
        d = capture()
        for i in range(3, 7):
            r = d['rows'][i]
            p = pose(math.radians(2), position=(10 + i*.00001, 20, 30))
            for side in ('before', 'after'): r[side]['pose'] = copy.deepcopy(p)
            r.update(sampler.observed_pitch(p))
        self.assertFalse(self.verdict(d)['active_scope_pass'])

    def test_no_bridge_across_rejected_row(self):
        d = capture(); d['rows'].insert(3, {'accepted': False})
        self.assertFalse(self.verdict(d)['active_scope_pass'])

    def test_direction_only_authored_motion(self):
        d = capture(stationary=True)
        for i in range(3, 7):
            yaw = math.radians((i-2) * .1)
            p = pose(math.radians(2), forward=(math.sin(yaw), 0, math.cos(yaw)))
            for side in ('before', 'after'):
                d['rows'][i][side]['pose'] = copy.deepcopy(p)
            d['rows'][i].update(sampler.observed_pitch(p))
        r = self.verdict(d)
        self.assertTrue(r['active_scope_pass'], r)
        self.assertEqual(r['intervals'][0]['maximum_displacement'], 0)
        self.assertGreater(r['intervals'][0]['maximum_direction_degrees'], .3)

    def test_target_distance_only_is_not_authored_motion(self):
        d = capture(stationary=True)
        for i in range(3, 7):
            p = pose(math.radians(2))
            for field in ('wrapper_target', 'native_target'):
                p[field][:3] = [p['position'][j] + (p[field][j]-p['position'][j]) * i
                                for j in range(3)]
            for side in ('before', 'after'):
                d['rows'][i][side]['pose'] = copy.deepcopy(p)
            d['rows'][i].update(sampler.observed_pitch(p))
        self.assertFalse(self.verdict(d)['active_scope_pass'])

    def test_sustained_sign_inversion_fails(self):
        d, inverted = capture(), capture(angle=-2)
        d['rows'][3:] = inverted['rows'][3:]
        self.assertFalse(self.verdict(d)['active_scope_pass'])

    def test_later_valid_segment_stands_alone(self):
        d = capture()
        d['rows'] = [{'accepted': False}, {'accepted': False}] + d['rows']
        r = self.verdict(d)
        self.assertTrue(r['active_scope_pass'], r)
        self.assertEqual(len(r['intervals']), 1)
        self.assertEqual(r['intervals'][0]['first_row'], 2)
        self.assertEqual(r['intervals'][0]['baseline_rows'], [2, 3, 4])
        self.assertEqual(r['intervals'][0]['moving_rows'], [5, 6, 7, 8])
        self.assertEqual([v['row'] for v in r['breaks']], [0, 1])


if __name__ == '__main__': unittest.main()
