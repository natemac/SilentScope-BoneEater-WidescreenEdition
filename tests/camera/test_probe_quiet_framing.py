import copy
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import probe_quiet_framing as probe


class Clock:
    value = 0.0
    def now(self): return self.value
    def sleep(self, seconds): self.value += seconds
    def stamp(self, reader):
        return {'tick_ms': round(self.value*1000), 'utc_ns': round(self.value*1e9),
                'perf_counter_ns': round(self.value*1e9)}


class FakeAPI:
    def __init__(self, clock):
        self.clock, self.deadline, self.cleaning = clock, None, False
        self.calls, self.closed = [], False
        self.fail_after_y = None
        self.fail_reset = False
        self.stuck_reset = False
        self.state = {'analogs': {'Gun X': [.5, False], 'Gun Y': [.5, False]},
                      'buttons': {'Scope Right': [0, False], 'Gun Pressed': [0, False]}}

    def request(self, module, function, values=None):
        self.calls.append((module, function, copy.deepcopy(values), self.clock.value))
        self.clock.value += .001
        if function == 'read':
            return [[name, *value] for name, value in self.state[module].items()]
        if function == 'write_reset' and self.fail_reset:
            raise probe.APIError('server unavailable during reset')
        for name, *value in values:
            if function == 'write_reset':
                if not self.stuck_reset: self.state[module][name][1] = False
            else:
                self.state[module][name] = [float(value[0]), True]
                if name == 'Gun Y' and value[0] == self.fail_after_y:
                    raise RuntimeError('response failed after mutation')

    def close(self): self.closed = True


class QuietProbeTests(unittest.TestCase):
    def setup_probe(self):
        clock = Clock()
        api = FakeAPI(clock)
        report = {'phases': [], 'cleanup_confirmed': False, 'release_acknowledged': False}
        return clock, api, report

    def row(self, clock, api):
        context = {'script_scope_off': 0, 'native_held_bits': 4 if api.state['buttons']['Scope Right'][0] else 0}
        side = {'identity': {'main': '0x1234'}, 'context': context}
        tick = round(clock.value*1000)
        y = api.state['analogs']['Gun Y'][0]
        held = bool(api.state['buttons']['Scope Right'][0])
        degrees = 6.0 if held or y == .1 else -6.0 if y == .9 else 0.0
        return {'accepted': True, 'repeated_context_equal': True, 'before': side, 'after': copy.deepcopy(side),
                'tick_ms': tick, 'tick_end_ms': tick, 'observed_pitch_degrees': degrees}

    def run_probe(self, clock, api, report, sampling=None):
        sampling = sampling or (lambda reader: self.row(clock, api))
        with patch.object(probe, 'sample', side_effect=sampling), \
                patch.object(probe, 'stamp', side_effect=clock.stamp):
            probe.run(None, api, report, clock=clock.now, sleep=clock.sleep)

    def assert_released_only_owned(self, api):
        self.assertTrue(all(not value[1] for fields in api.state.values() for value in fields.values()))
        writes = [(m, pair[0]) for m, f, pairs, _ in api.calls if f == 'write' for pair in pairs]
        self.assertTrue(set(writes) <= {('analogs', 'Gun X'), ('analogs', 'Gun Y'), ('buttons', 'Scope Right')})
        resets = [(m, pair[0]) for m, f, pairs, _ in api.calls if f == 'write_reset' for pair in pairs]
        self.assertCountEqual(resets, [('analogs', 'Gun X'), ('analogs', 'Gun Y'), ('buttons', 'Scope Right')])

    def test_six_phases_ownership_stamps_boundaries_and_explicit_cleanup(self):
        self.assertIn('writes temporary API overrides', probe.LIMIT_NOTE)
        self.assertIn('never requests trigger input', probe.LIMIT_NOTE)
        clock, api, report = self.setup_probe()
        self.run_probe(clock, api, report)
        self.assertEqual([p['name'] for p in report['phases']], [p[0] for p in probe.PHASES])
        self.assertLess(clock.value, 30)
        self.assertTrue(report['release_acknowledged'] and report['cleanup_confirmed'])
        starts = []
        for phase in report['phases']:
            begin = phase['inputs_written']['tick_ms']
            self.assertEqual(phase['settle_until_tick_ms'], begin + 2000)
            self.assertEqual(phase['planned_end_tick_ms'], begin + 4000)
            self.assertGreaterEqual(phase['settled_observations'], 3)
            starts.extend(r['tick_ms'] for r in phase['samples'])
            self.assertTrue(all(begin <= r['tick_ms'] <= begin+4000 for r in phase['samples']))
        self.assertLessEqual(len(starts), 150)
        self.assertTrue(all(b-a >= 199 for a, b in zip(starts, starts[1:])))
        self.assertEqual([p['settled_pitch_degrees'][0] for p in report['phases']], [0, 6, 6, 6, -6, 0])
        self.assert_released_only_owned(api)

    def test_preexisting_override_or_held_gun_rejects_before_writes(self):
        clock, api, report = self.setup_probe()
        api.state['analogs']['Gun X'][1] = True
        with self.assertRaisesRegex(RuntimeError, 'overridden'):
            self.run_probe(clock, api, report)
        self.assertFalse(any(f in ('write', 'write_reset') for _, f, _, _ in api.calls))
        clock, api, report = self.setup_probe()
        api.state['buttons']['Scope Right'][0] = 1
        with self.assertRaisesRegex(RuntimeError, 'Release physical'):
            self.run_probe(clock, api, report)
        self.assertEqual(api.calls, [])

    def test_failed_write_response_still_releases_and_checks(self):
        clock, api, report = self.setup_probe()
        api.fail_after_y = .9
        with self.assertRaisesRegex(RuntimeError, 'after mutation'):
            self.run_probe(clock, api, report)
        self.assertTrue(report['cleanup_confirmed'])
        self.assertFalse(report['release_acknowledged']) # Body did not reach normal context exit.
        self.assert_released_only_owned(api)

    def test_sampling_failure_and_deadline_recover_owned_inputs(self):
        clock, api, report = self.setup_probe()
        count = [0]
        def sampling(reader):
            count[0] += 1
            if count[0] == 4: raise RuntimeError('unexpected sample failure')
            return self.row(clock, api)
        with self.assertRaisesRegex(RuntimeError, 'sample failure'):
            self.run_probe(clock, api, report, sampling)
        self.assert_released_only_owned(api)
        clock, api, report = self.setup_probe()
        count = [0]
        def stall(reader):
            count[0] += 1
            if count[0] == 2: clock.value += 31
            return self.row(clock, api)
        with self.assertRaisesRegex(RuntimeError, 'deadline'):
            self.run_probe(clock, api, report, stall)
        self.assertTrue(api.cleaning and report['cleanup_confirmed'])
        self.assert_released_only_owned(api)

    def test_changed_identity_or_missing_settled_rows_rejects(self):
        for cause in ('identity', 'rejected', 'off'):
            clock, api, report = self.setup_probe()
            count = [0]
            def sampling(reader):
                count[0] += 1
                value = self.row(clock, api)
                if count[0] > 1:
                    if cause == 'identity': value['before']['identity'] = {'main': '0x9999'}
                    elif cause == 'rejected': value['accepted'] = False
                    else: value['before']['context']['script_scope_off'] = 1
                return value
            with self.assertRaises(RuntimeError): self.run_probe(clock, api, report, sampling)
            self.assert_released_only_owned(api)

    def test_cleanup_failure_is_not_silently_reported_success(self):
        for failure in ('unreachable', 'stuck'):
            clock, api, report = self.setup_probe()
            api.fail_reset = failure == 'unreachable'
            api.stuck_reset = failure == 'stuck'
            with self.assertRaises(RuntimeError): self.run_probe(clock, api, report)
            self.assertFalse(report['cleanup_confirmed'])
            self.assertIn('cleanup_error', report)
            resets = [m for m, f, _, _ in api.calls if f == 'write_reset']
            self.assertIn('analogs', resets); self.assertIn('buttons', resets)

    def test_verified_connection_checks_each_request_and_cleanup(self):
        events = []
        connection = probe.VerifiedConnection(object(), 43488, 5730)
        with patch.object(probe, 'process_alive', side_effect=lambda r: events.append('alive')), \
                patch.object(probe, 'verify_listener', side_effect=lambda p, port: events.append((p, port))), \
                patch.object(probe.Connection, 'request', side_effect=lambda *args: events.append('request')):
            connection.request('buttons', 'read')
            connection.request('analogs', 'write', [['Gun X', .5]])
            connection.deadline = 0; connection.cleaning = True
            connection.request('analogs', 'write_reset', [['Gun X']])
        self.assertEqual(events, ['alive', (43488, 5730), 'request']*3)
        with patch.object(probe, 'process_alive', side_effect=RuntimeError('dead')), \
                patch.object(probe.Connection, 'request') as original:
            with self.assertRaises(probe.APIError): connection.request('buttons', 'read')
            original.assert_not_called()

    def test_api_readback_shape_and_ownership_requirements(self):
        for rows in ([['a', 0, False], ['a', 0, False]], [['a', float('nan'), False]], [['a', 1, 'false']]):
            with self.assertRaises(RuntimeError): probe.states(rows)
        clock, api, report = self.setup_probe()
        readback = {m: [[n, *s] for n, s in values.items()] for m, values in api.state.items()}
        with self.assertRaises(RuntimeError): probe.expected(readback, .5, False)


if __name__ == '__main__': unittest.main()
