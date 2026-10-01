import copy
import csv
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import probe_scope as probe
from game_api import APIError


def scope_row(pid=42, elapsed=0, x=.5, y=.5, held=False):
    row = {key: '0' for key in probe.SCOPE_FIELDS}
    row.update(pid=str(pid), elapsed_ms=str(elapsed), event='overlay_draw' if held else 'reticle_draw',
               hresult='0x00000000', viewport_status='resolved', target_width='1920', target_height='1080',
               aim_x=str(x), aim_y=str(y), ray_x=str(1920*x), ray_y=str(1080*y),
               overlay_x=str(1920*x), overlay_y=str(1080*y), native_age_ms='10', ray_age_ms='10',
               source_age_ms='1', scope_off_known='1', scope_off='0')
    return row


class FakeAPI:
    def __init__(self, fail_y=None, reset_noop=False):
        self.state = {'analogs': {'Gun X': [.5, False], 'Gun Y': [.5, False]},
                      'buttons': {'Scope Right': [0., False], 'Gun Pressed': [1., False]}}
        self.calls = []
        self.closed = False
        self.fail_y = fail_y
        self.reset_noop = reset_noop
        self.block_listener = False
        self.events = []

    def verify(self, pid, port):
        self.events.append('verify')
        if self.block_listener:
            raise RuntimeError('Listener changed owner')

    def request(self, module, function, values=None):
        if not self.events or self.events[-1] != 'verify':
            raise AssertionError('Every backend request must be preceded by listener verification')
        self.events.append(function)
        self.calls.append((module, function, copy.deepcopy(values)))
        if function == 'read':
            return [[key, *value] for key, value in self.state[module].items()]
        for pair in values:
            if function == 'write_reset':
                if not self.reset_noop:
                    self.state[module][pair[0]][1] = False
            else:
                self.state[module][pair[0]] = [float(pair[1]), True]
                if pair == ['Gun Y', self.fail_y]:
                    raise APIError('Response failed after applying input')
        return []

    def close(self):
        self.closed = True


class Simulation:
    def __init__(self, desktop, api, *, emit=True, foreign=False, rotate=False, lose_owner=False):
        self.path = desktop / 'scope-v3.csv'
        self.api = api
        self.now = 10.
        self.last_emit = self.now
        self.emit = emit
        self.foreign = foreign
        self.rotate = rotate
        self.lose_owner = lose_owner
        self.emissions = 0
        self.write(scope_row(elapsed=10000), replace=True)

    def write(self, row, replace=False):
        with self.path.open('w' if replace else 'a', newline='', encoding='utf-8') as stream:
            writer = csv.DictWriter(stream, fieldnames=probe.SCOPE_FIELDS)
            if replace:
                writer.writeheader()
            writer.writerow(row)

    def stamp(self):
        return {'tick_ms': round(self.now*1000), 'utc_ns': round(self.now*1e9),
                'perf_counter_ns': round(self.now*1e9)}

    def sleep(self, seconds):
        self.now += seconds
        if self.emit and self.now - self.last_emit >= 4.999:
            self.last_emit = self.now
            self.emissions += 1
            held = bool(self.api.state['buttons']['Scope Right'][0])
            x, y = (self.api.state['analogs'][key][0] for key in ('Gun X', 'Gun Y'))
            row = scope_row(43 if self.foreign else 42, round(self.now*1000), x, y, held)
            self.write(row, replace=self.rotate and held)
            if self.lose_owner and held:
                self.api.block_listener = True


class ProbeScopeTests(unittest.TestCase):
    def simulate(self, api, **options):
        with tempfile.TemporaryDirectory() as folder:
            desktop = Path(folder)
            simulation = Simulation(desktop, api, **options)
            output = desktop / 'result.json'
            with patch.object(probe, 'Connection', return_value=api), \
                 patch.object(probe, 'verify_listener', side_effect=api.verify), \
                 patch.object(probe, 'stamp', side_effect=simulation.stamp), \
                 patch.object(probe.time, 'monotonic', side_effect=lambda: simulation.now), \
                 patch.object(probe.time, 'sleep', side_effect=simulation.sleep):
                report, error = probe.run_probe(42, 5730, output, desktop)
            self.assertEqual(report, json.loads(output.read_text(encoding='utf-8')))
            self.assertTrue(api.closed)
            # A physical trigger starts true and must remain completely untouched.
            self.assertEqual(api.state['buttons']['Gun Pressed'], [1., False])
            for _, operation, values in api.calls:
                if operation in ('write', 'write_reset'):
                    self.assertTrue(all(pair[0] != 'Gun Pressed' for pair in values))
            return report, error

    def test_five_points_readbacks_settle_freshness_and_cleanup(self):
        api = FakeAPI()
        report, error = self.simulate(api)
        self.assertIsNone(error)
        self.assertTrue(report['completed'] and report['cleanup_confirmed'])
        self.assertNotIn('fired', report)
        self.assertFalse(report['trigger_requested'] or report['shot_occurrence_verified'])
        self.assertEqual([p['name'] for p in report['points']], [p[0] for p in probe.POINTS])
        for point in report['points']:
            self.assertGreater(float(point['sample']['elapsed_ms']),
                               point['settled_observation_barrier']['scope_elapsed_ms'])
            self.assertGreaterEqual(point['settled_observation_barrier']['tick_ms'],
                                    point['inputs_written']['tick_ms'] + 2000)
            self.assertEqual(point['ray_error_logical_pixels'], [0., 0.])
        self.assertTrue(all(not state[1] for names in api.state.values() for state in names.values()))
        self.assertTrue(report['cleanup']['readback_confirmed'])

    def test_failed_write_after_application_is_released_and_reported(self):
        report, error = self.simulate(FakeAPI(fail_y=0.))
        self.assertIsInstance(error, APIError)
        self.assertFalse(report['completed'])
        self.assertTrue(report['cleanup_confirmed'])
        self.assertEqual(len(report['points']), 2)
        self.assertIn('error', report)

    def test_stale_or_foreign_appends_cannot_pass_preflight(self):
        for option in ({'emit': False}, {'foreign': True}):
            with self.subTest(option=option):
                api = FakeAPI()
                report, error = self.simulate(api, **option)
                self.assertIsNotNone(error)
                self.assertEqual(report['points'], [])
                self.assertFalse(any(operation == 'write' for _, operation, _ in api.calls))

    def test_rotation_during_probe_releases_owned_inputs(self):
        report, error = self.simulate(FakeAPI(), rotate=True)
        self.assertIsNotNone(error)
        self.assertIn('coverage', str(error))
        self.assertTrue(report['cleanup_confirmed'])

    def test_owner_change_prevents_cleanup_into_another_process(self):
        api = FakeAPI()
        report, error = self.simulate(api, lose_owner=True)
        self.assertIsNotNone(error)
        self.assertFalse(report['cleanup_confirmed'])
        self.assertTrue(report['cleanup']['pending_inputs'])
        self.assertIn('Listener changed owner', report['cleanup']['error']['message'])
        self.assertFalse(any(operation == 'write_reset' for _, operation, _ in api.calls))

    def test_reset_acknowledgement_without_inactive_readback_is_not_cleanup(self):
        report, error = self.simulate(FakeAPI(reset_noop=True))
        self.assertIsNotNone(error)
        self.assertTrue(report['completed'])
        self.assertTrue(report['cleanup']['reset_acknowledged'])
        self.assertFalse(report['cleanup_confirmed'] or report['cleanup']['readback_confirmed'])

    def test_preexisting_override_is_not_owned_or_reset(self):
        api = FakeAPI()
        api.state['analogs']['Gun Y'][1] = True
        report, error = self.simulate(api)
        self.assertIsNotNone(error)
        self.assertTrue(api.state['analogs']['Gun Y'][1])
        self.assertEqual(report['cleanup']['owned_inputs'], {})
        self.assertFalse(any(operation == 'write_reset' for _, operation, _ in api.calls))

    def test_sample_fields_reject_nonfinite_stale_or_unresolved(self):
        for field, value in (('ray_x', 'nan'), ('native_age_ms', '251'),
                             ('source_age_ms', '-1'), ('scope_off_known', '0'),
                             ('viewport_status', 'unresolved'), ('target_width', '0')):
            row = scope_row(held=True); row[field] = value
            with self.subTest(field=field), self.assertRaises(RuntimeError):
                probe.drawing(row, (.5, .5))
        self.assertFalse(probe.drawing(scope_row(x=.6, held=True), (.5, .5)))

    def test_clock_regression_rejects_same_pid(self):
        class Reader:
            gap_generation = 0
            def poll(self): return [scope_row(elapsed=90)]
        with self.assertRaisesRegex(RuntimeError, 'regressed'):
            probe.wait_for_row(Reader(), 100)


if __name__ == '__main__':
    unittest.main()
