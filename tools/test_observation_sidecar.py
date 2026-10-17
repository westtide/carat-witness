#!/usr/bin/env python3
"""Concrete observer regression, no solver/CPA. Evidence stays in a fresh tmpdir."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

BINARY = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 and not sys.argv[1].startswith('-') else Path(__file__).resolve().parents[1] / 'build/carat-replay'
BASE = '''1 sort bitvec 1
2 sort bitvec 2
3 zero 2
4 one 2
5 state 2 pc
6 init 2 5 3
7 add 2 5 4
8 next 2 5 7
9 input 2 x
10 eq 1 5 4
11 eq 1 9 4
12 and 1 10 11
13 bad 12
14 output 10 __c2btor_exec_7
15 output 9 __c2btor_value_7
16 output 12 __c2btor_bad_7
17 output 5 __c2btor_pc
18 one 1
19 constraint 18
'''
CEX = 'sat\nb0\n#0\n@0\n0 00\n#1\n@1\n0 01\n.\n'

class ObserverTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = Path(tempfile.mkdtemp(prefix='carat-observer-regression-'))
        print('Evidence:', cls.root, flush=True)

    def setUp(self):
        self.directory = self.root / self._testMethodName
        self.directory.mkdir()

    def run_case(self, name, model=BASE, cex=CEX, confirmed=True):
        folder = self.directory / name
        folder.mkdir()
        model_path, cex_path = folder/'model.btor2', folder/'original.cexb'
        model_path.write_text(model)
        cex_path.write_text(cex)
        before = [hashlib.sha256(path.read_bytes()).hexdigest() for path in (model_path, cex_path)]
        output = folder/'output'
        command = [str(BINARY), '--model', str(model_path), '--cex', str(cex_path), '--output-dir', str(output)]
        (folder/'command.json').write_text(json.dumps(command, indent=2)+'\n')
        process = subprocess.run(command, capture_output=True, text=True, timeout=10)
        (folder/'stdout').write_text(process.stdout)
        (folder/'stderr').write_text(process.stderr)
        self.assertEqual(process.returncode, 0 if confirmed else 1, process.stderr)
        self.assertEqual([hashlib.sha256(path.read_bytes()).hexdigest() for path in (model_path, cex_path)], before)
        if not confirmed:
            self.assertEqual(list(output.glob('*')) if output.exists() else [], [])
            return None
        copied = output/'model.btor2.cexb'
        self.assertEqual(copied.read_bytes(), cex_path.read_bytes())
        lines = (output/'model.btor2.cexb.obs.jsonl').read_bytes().splitlines(keepends=True)
        records = [json.loads(line) for line in lines]
        header, footer = records[0], records[-1]
        self.assertEqual(header['record'], 'header')
        self.assertEqual(header['format'], 'c2btor-observations-1')
        self.assertEqual(header['model_sha256'], before[0])
        self.assertEqual(header['cex_sha256'], before[1])
        self.assertEqual(footer['observations_sha256'], hashlib.sha256(b''.join(lines[:-1])).hexdigest())
        self.assertEqual(footer['record'], 'footer')
        self.assertTrue(footer['confirmed'])
        self.assertEqual(header['frames'], len(records)-2)
        self.assertEqual(footer['frames'], header['frames'])
        self.assertEqual(footer['badframe'], header['frames']-1)
        self.assertEqual([r['frame'] for r in records[1:-1]], list(range(header['frames'])))
        # Existing output can never overwrite either the new receipt or source.
        receipt = (output/'model.btor2.cexb.obs.jsonl').read_bytes()
        again = subprocess.run(command, capture_output=True, text=True, timeout=10)
        self.assertEqual(again.returncode, 1)
        self.assertEqual((output/'model.btor2.cexb.obs.jsonl').read_bytes(), receipt)
        return records

    def test_current_frame_gating_named_outputs_pc_expression_and_signed_value(self):
        records = self.run_case('direct')
        self.assertEqual(records[1], {'record':'frame','frame':0,'pc':'00','events':[]})
        self.assertEqual(records[2]['events'], [{'event':7,'exec':'1','value':'01','bad':'1'}])
        self.assertEqual(records[-1]['bad_node'], 13)
        self.assertEqual(records[-1]['bad_expression'], 12)
        model = BASE.replace('15 output 9 ', '15 output -9 ').replace('17 output 5 ', '17 output 7 ')
        records = self.run_case('signed-pc-combination', model)
        self.assertEqual(records[0]['pc'], {'node':7,'width':2})
        self.assertEqual([r['pc'] for r in records[1:-1]], ['01','10'])
        self.assertEqual(records[2]['events'][0]['value'], '10')
        self.assertEqual(next(d['value_node'] for d in records[0]['outputs'] if d['kind']=='value'), -9)

    def test_legacy_expression_symbols_and_pc_fallback(self):
        model = BASE.replace('10 eq 1 5 4', '10 eq 1 5 4 __c2btor_exec_7').replace('12 and 1 10 11','12 and 1 10 11 __c2btor_bad_7')
        model = model.replace('14 output 10 __c2btor_exec_7','14 output 10').replace('16 output 12 __c2btor_bad_7','16 output 12')
        model = model.replace('17 output 5 __c2btor_pc\n','').replace('16 output 12','17 output 12')
        model = model.replace('15 output 9 __c2btor_value_7', '15 or 2 9 3 __c2btor_value_7\n16 output 15')
        records = self.run_case('legacy', model)
        self.assertEqual(records[0]['pc'], {'node':5,'width':2})
        self.assertEqual(records[2]['events'][0]['value'], '01')
        for descriptor in records[0]['outputs']:
            self.assertTrue(descriptor['name'].startswith('__c2btor_'))

    def test_sparse_array_80bit_index_default_order_and_scalar_read(self):
        index = '0'*80
        model = '''1 sort bitvec 1
2 sort bitvec 80
3 sort bitvec 8
4 sort array 2 3
5 input 4 array
6 zero 2
7 read 3 5 6
8 zero 3
9 eq 1 7 8
10 one 1
11 bad 9
12 output 10 __c2btor_exec_1
13 output 7 __c2btor_value_1
14 output 9 __c2btor_bad_1
16 constraint 10
'''
        cex = f'sat\nb0\n#0\n@0\n0 [{index}] 00000000\n0 [*] 00000001\n.\n'
        records = self.run_case('array-index-before-default', model, cex)
        self.assertIsNone(records[0]['pc'])
        self.assertEqual(len(records[0]['outputs']), 3)
        self.assertEqual(records[1]['events'], [{'event':1,'exec':'1','value':'00000000','bad':'1'}])
        self.run_case('array-default-before-index', model, cex.replace(f'0 [{index}] 00000000\n0 [*] 00000001', f'0 [*] 00000001\n0 [{index}] 00000000'))
        self.run_case('array-partial-rejected', model, cex.replace('0 [*] 00000001\n',''), False)

    def test_verification_and_parser_failures_never_publish(self):
        cases = {
            'final-bad-false': (BASE,CEX.replace('@1\n0 01','@1\n0 00')),
            'constraint-false': (BASE.replace('19 constraint 18','19 constraint 11'),CEX),
            'reported-state-mismatch': (BASE,CEX.replace('#1\n',' #1\n0 00\n')),
            'missing-input': (BASE,CEX.replace('@1\n0 01','@1')),
            'duplicate-input': (BASE,CEX.replace('@1\n0 01','@1\n0 01\n0 01')),
            'wrong-width': (BASE,CEX.replace('@1\n0 01','@1\n0 1')),
            'out-of-range-port': (BASE,CEX.replace('@1\n0 01','@1\n1 01')),
            'unknown-value': (BASE,CEX.replace('@1\n0 01','@1\n0 0x')),
            'frame-gap': (BASE,CEX.replace('#1',' #2')),
            'data-after-dot': (BASE,CEX+'garbage\n'),
            'wrong-property': (BASE,CEX.replace('b0','b1')),
            'bad-observation-width': (BASE.replace('16 output 12','16 output 9'),CEX),
        }
        for name,(model,cex) in cases.items():
            with self.subTest(name=name): self.run_case(name,model,cex,False)

    def test_sha256_padding_and_large_stream_against_hashlib(self):
        for size in (0,1,55,56,63,64,65,65536,1000000):
            with self.subTest(size=size):
                self.run_case('sha-'+str(size), BASE+';'+('x'*size)+'\n')

if __name__ == '__main__': unittest.main()
