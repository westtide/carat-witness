#!/usr/bin/env python3
"""Standard counterexample regression: one solve, strict deterministic replay."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

BUILD = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[1] / 'build'

SCALAR = '''1 sort bitvec 1
2 sort bitvec 8
3 zero 2
4 one 2
5 input 2 used
6 input 2 unused
7 state 2 pc
8 init 2 7 3
9 add 2 7 4
10 next 2 7 9
11 state 2 calls
12 init 2 11 3
13 eq 1 5 4
14 add 2 11 4
15 ite 2 13 14 11
16 next 2 11 15
17 eq 1 7 4
18 and 1 17 13
19 bad 18
'''
ARRAY = '''1 sort bitvec 1
2 sort bitvec 2
3 sort bitvec 8
4 sort array 2 3
5 zero 3
6 one 3
7 zero 2
8 input 2 index
9 input 3 value
10 input 4 unused_array
11 state 4 memory
12 init 4 11 5
13 write 4 11 8 9
14 next 4 11 13
15 state 3 pc
16 init 3 15 5
17 add 3 15 6
18 next 3 15 17
19 read 3 11 7
20 eq 1 15 6
21 neq 1 19 5
22 and 1 20 21
23 bad 22
'''
DIRECT = 'sat\nb0\n#0\n0 00000000\n1 00000000\n@0\n0 00000001\n1 00000000\n#1\n0 00000001\n1 00000001\n@1\n0 00000001\n1 00000000\n.\n'


class WitnessTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='carat-standard-cex-')
        self.directory = Path(self.temporary.name)
        self.model = self.directory / 'model.btor2'
        self.cex = self.directory / 'model.btor2.cexb'

    def tearDown(self):
        self.temporary.cleanup()

    def replay(self, model=SCALAR, trace=DIRECT, valid=True):
        self.model.write_text(model)
        self.cex.write_text(trace)
        result = subprocess.run([str(BUILD/'carat-replay'), str(self.model), str(self.cex)],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0 if valid else 1, result.stdout + result.stderr)
        self.assertEqual(set(p.name for p in self.directory.iterdir()),
                         {'model.btor2', 'model.btor2.cexb'})
        return result

    def solve(self, model=SCALAR, algorithm='ic3', verdict='Unsafe'):
        self.model.write_text(model)
        command = [str(BUILD/'carat'), '-a', algorithm, '-s', 'minicore',
                   '-k', '4', '-w', str(self.directory), str(self.model)]
        if algorithm == 'ic3':
            command[1:1] = ['--all', '--ctg_max_lvl', '0']
        result = subprocess.run(command, capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(verdict, result.stdout.splitlines(), result.stdout + result.stderr)
        expected = {'model.btor2', 'model.btor2.cexb'} if verdict == 'Unsafe' else {'model.btor2'}
        self.assertEqual(set(p.name for p in self.directory.iterdir()), expected)
        if verdict == 'Unsafe':
            trace = self.cex.read_text()
            self.replay(model, trace)
            return trace
        return result.stdout

    def test_original_ports_coi_counters_final_inputs(self):
        trace = self.solve()
        # Both source inputs exist at every @ frame, including final frame.
        # State ordinal 1 is the source counter outside the property COI.
        for section in trace.split('@')[1:]:
            assignments = section.split('#', 1)[0].splitlines()[1:]
            self.assertTrue(any(row.startswith('0 ') for row in assignments))
            self.assertTrue(any(row == '1 00000000' for row in assignments))
        self.assertIn('#0\n0 00000000\n1 00000000\n', trace)
        self.assertEqual(trace.splitlines()[:2], ['sat', 'b0'])

    def test_original_arrays_initialized_and_source_defaults(self):
        trace = self.solve(ARRAY)
        self.assertIn('0 [*] 00000000', trace)  # initialized memory state
        self.assertIn('2 [*] 00000000', trace)  # omitted original array input
        self.assertIn('0 [00] ', trace)  # concrete written memory cell

    def test_single_pass_bmc_wlbmc_kind(self):
        for algorithm in ('bmc', 'wlbmc', 'kind'):
            with self.subTest(algorithm=algorithm):
                if self.cex.exists():
                    self.cex.unlink()
                self.solve(algorithm=algorithm)

    def test_safe_has_no_aig_or_sidecar(self):
        self.solve(SCALAR.replace('19 bad 18', '19 bad -18').replace('18 and 1 17 13', '18 one 1'),
                   verdict='Safe')

    def test_replay_checks_adjacent_and_final_counter(self):
        self.replay()
        self.replay(trace=DIRECT.replace('#1\n0 00000001\n1 00000001',
                                         '#1\n0 00000001\n1 00000000'), valid=False)

    def test_missing_input_final_frame_is_incomplete(self):
        self.replay(trace=DIRECT.replace('@1\n0 00000001\n1 00000000', '@1\n0 00000001'), valid=False)

    def test_missing_free_state_is_incomplete(self):
        model = SCALAR + '20 state 2 free_choice\n'
        self.replay(model, valid=False)

    def test_wrong_bad_ordinal_truncation_unknown_bits(self):
        for trace in (DIRECT.replace('b0', 'b1'), DIRECT[:-2],
                      DIRECT.replace('0 00000001', '0 xxxxxxxx', 1),
                      DIRECT + 'sat\n', DIRECT.replace('@1', '@2')):
            with self.subTest(trace=trace):
                self.replay(trace=trace, valid=False)

    def test_wrong_initial_array_and_partial_array(self):
        model = ARRAY.replace('23 bad 22', '23 bad 20')
        trace = ('sat\nb0\n#0\n0 [*] 00000000\n1 00000000\n@0\n'
                 '0 00\n1 00000001\n2 [*] 00000000\n#1\n'
                 '0 [*] 00000000\n0 [00] 00000001\n1 00000001\n@1\n'
                 '0 00\n1 00000000\n2 [*] 00000000\n.\n')
        self.replay(model, trace)
        self.replay(model, trace.replace('0 [*] 00000000', '0 [*] 00000001', 1), valid=False)
        self.replay(model, trace.replace('2 [*] 00000000', '2 [00] 00000000', 1), valid=False)

    def test_constraint_final_frame_and_bad_are_checked(self):
        self.replay(SCALAR + '20 constraint 13\n')
        self.replay(SCALAR + '20 constraint -13\n', valid=False)
        self.replay(trace=DIRECT.replace('@1\n0 00000001', '@1\n0 00000000'), valid=False)

    def test_counter_wrap_is_computed_modulo_256(self):
        # A 9-bit progress state reaches 257; the source call counter is 8-bit.
        model = """1 sort bitvec 1
2 sort bitvec 9
3 sort bitvec 8
4 zero 2
5 one 2
6 constd 2 257
7 zero 3
8 one 3
9 input 1 executed
10 input 1 unused
11 state 2 pc
12 init 2 11 4
13 add 2 11 5
14 next 2 11 13
15 state 3 calls
16 init 3 15 7
17 add 3 15 8
18 ite 3 9 17 15
19 next 3 15 18
20 eq 1 11 6
21 and 1 20 9
22 bad 21
"""
        trace = 'sat\nb0\n'
        for time in range(258):
            trace += (f'#{time}\n0 {time:09b}\n1 {time % 256:08b}\n'
                      f'@{time}\n0 1\n1 0\n')
        self.replay(model, trace + '.\n')


if __name__ == '__main__':
    unittest.main()
