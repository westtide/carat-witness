#!/usr/bin/env python3
"""Independent explicit-choice probe regressions. No SAT or CPA calls."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

BINARY = Path(sys.argv.pop(1)).resolve() if len(sys.argv)>1 and not sys.argv[1].startswith('-') else Path(__file__).resolve().parents[1]/'build/carat-replay'
BASE='''1 sort bitvec 1
2 sort bitvec 2
3 zero 2
4 one 2
5 state 2 pc
6 init 2 5 3
7 add 2 5 4
8 next 2 5 7
9 input 2 choice
10 eq 1 5 4
11 eq 1 9 4
12 and 1 10 11
13 bad 12
14 output 10 __c2btor_exec_1
15 output 9 __c2btor_value_1
16 output 12 __c2btor_bad_1
17 output 5 __c2btor_pc
18 one 1
19 constraint 18
20 input 2 unrelated
'''

class ProbeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root=Path(tempfile.mkdtemp(prefix='carat-concrete-probe-regression-'))
        print('Evidence:',cls.root,flush=True)

    def setUp(self):
        self.directory=self.root/self._testMethodName
        self.directory.mkdir()

    def run_case(self,name,model=BASE,options=('--probe-steps','8','--probe-input','9=01'),code=0):
        folder=self.directory/name;folder.mkdir()
        source=folder/'model.btor2';source.write_text(model)
        before=hashlib.sha256(source.read_bytes()).hexdigest()
        output=folder/'output'
        command=[str(BINARY),'--model',str(source),'--output-dir',str(output),*options]
        (folder/'command.json').write_text(json.dumps(command,indent=2)+'\n')
        result=subprocess.run(command,text=True,capture_output=True,timeout=10)
        (folder/'stdout').write_text(result.stdout);(folder/'stderr').write_text(result.stderr)
        self.assertEqual(result.returncode,code,result.stderr)
        self.assertEqual(hashlib.sha256(source.read_bytes()).hexdigest(),before)
        if code:
            self.assertNotIn('Unsafe',result.stdout)
            self.assertFalse(list(output.glob('*.obs.jsonl')) if output.exists() else [])
            self.assertFalse(list(output.glob('*.cexb')) if output.exists() else [])
            if code==2:self.assertIn('Unknown',result.stdout)
            return
        self.assertIn('\nUnsafe\n','\n'+result.stdout)
        cex=output/'model.btor2.cexb';obs=output/'model.btor2.cexb.obs.jsonl'
        lines=obs.read_bytes().splitlines(keepends=True);records=[json.loads(x)for x in lines]
        self.assertEqual(records[0]['model_sha256'],before)
        self.assertEqual(records[0]['cex_sha256'],hashlib.sha256(cex.read_bytes()).hexdigest())
        self.assertTrue(records[-1]['confirmed'])
        self.assertEqual(records[-1]['observations_sha256'],hashlib.sha256(b''.join(lines[:-1])).hexdigest())
        self.assertEqual(records[-1]['badframe'],len(records)-3)
        # Reuse the unchanged strict replay parser/Verify, rather than trusting
        # the probe's in-memory simulation or its own serialized result.
        replay=folder/'replay'
        checked=subprocess.run([str(BINARY),'--model',str(source),'--cex',str(cex),'--output-dir',str(replay)],text=True,capture_output=True,timeout=10)
        self.assertEqual(checked.returncode,0,checked.stderr)
        self.assertEqual((replay/'model.btor2.cexb').read_bytes(),cex.read_bytes())
        original=obs.read_bytes();again=subprocess.run(command,text=True,capture_output=True,timeout=10)
        self.assertEqual(again.returncode,1)
        self.assertEqual(obs.read_bytes(),original)
        return records,cex.read_text()

    def test_first_bad_total_inputs_and_legacy_replay(self):
        records,cex=self.run_case('first')
        self.assertEqual(records[0]['frames'],2)
        self.assertEqual(records[-1]['bad_node'],13)
        self.assertEqual(records[2]['events'],[{'event':1,'exec':'1','value':'01','bad':'1'}])
        for frame in cex.split('@')[1:]:
            values=frame.splitlines()[1:3]
            self.assertEqual(values,['0 01','1 00'])

    def test_bad_requires_every_prefix_constraint(self):
        self.run_case('bound-before-bad',options=('--probe-steps','1','--probe-input','9=01'),code=2)
        self.run_case('different-input',options=('--probe-steps','8','--probe-input','9=00'),code=2)
        self.run_case('same-frame-false',model=BASE.replace('19 constraint 18','19 constraint -11'),code=2)
        # At frame1 the constraint becomes true and bad holds. Frame0 already
        # violated it, so this is still not an execution of the source model.
        self.run_case('earlier-false',model=BASE.replace('19 constraint 18','19 constraint 10'),code=2)

    def test_free_states_are_explicit_without_overriding_successors(self):
        model=BASE+'''21 state 2 initially_free
22 next 2 21 5
23 state 2 always_free
24 state 2 initialized_but_no_next
25 init 2 24 3
'''
        records,cex=self.run_case('states',model=model)
        # Ordinals: pc0 is derived; initially_free1 only needs a frame0 seed,
        # always_free2 every frame, initialized_but_no_next3 after frame0.
        self.assertIn('#0\n1 00\n2 00\n@0',cex)
        self.assertIn('#1\n2 00\n3 00\n@1',cex)
        self.assertEqual(records[1]['pc'],'00');self.assertEqual(records[2]['pc'],'01')

    def test_array_choices_are_total_and_wide_scalar_seeds_exact(self):
        model=BASE+'''21 sort bitvec 80
22 sort array 21 2
23 input 22 array_input
24 state 22 free_array
25 input 21 wide_input
'''
        records,cex=self.run_case('arrays-wide',model=model,options=('--probe-steps','3','--probe-input','9=01','--probe-input','25='+'1'*80))
        self.assertIn('2 [*] 00',cex)
        self.assertIn('1 [*] 00',cex)
        self.assertIn('3 '+'1'*80,cex)
        self.assertEqual(records[0]['frames'],2)

    def test_invalid_seed_cannot_override_states_or_expressions(self):
        cases=[('state','5=01'),('expression','7=01'),('width','9=1'),('nonbinary','9=0x'),('negative','-9=01'),('zero','0=01'),('unknown','9999=01')]
        for name,seed in cases:
            with self.subTest(name=name):self.run_case(name,options=('--probe-steps','2','--probe-input',seed),code=1)
        self.run_case('duplicate',options=('--probe-steps','2','--probe-input','9=01','--probe-input','9=01'),code=1)
        self.run_case('zero-frames',options=('--probe-steps','0'),code=1)
        self.run_case('negative-frames',options=('--probe-steps','-1'),code=1)
        self.run_case('array-seed',model=BASE+'21 sort array 2 2\n22 input 21 array\n',options=('--probe-steps','2','--probe-input','22=00'),code=1)
        self.run_case('mixed-modes',options=('--probe-steps','2','--cex','missing'),code=1)

    def test_timed_inputs_change_only_explicit_original_choices(self):
        records,cex=self.run_case('timed',options=('--probe-steps','8','--probe-input-at','0:9=00','--probe-input-at','1:9=01'))
        self.assertEqual(records[0]['frames'],2)
        self.assertIn('@0\n0 00\n1 00',cex)
        self.assertIn('@1\n0 01\n1 00',cex)
        # A frame-specific choice takes precedence over an all-frame seed. A
        # false final bad must still be Unknown and publish no witness.
        self.run_case('timed-disables-bad',options=('--probe-steps','2','--probe-input','9=01','--probe-input-at','1:9=00'),code=2)

    def test_timed_seed_validation_and_duplicate_keys(self):
        cases=[('horizon','8:9=01'),('negative-time','-1:9=01'),('empty-time',':9=01'),('nonnumeric-time','x:9=01'),('missing-time','9=01'),('state','1:5=01'),('expression','1:7=01'),('wrong-width','1:9=1'),('nonbinary','1:9=0x')]
        for name,seed in cases:
            with self.subTest(name=name):self.run_case(name,options=('--probe-steps','8','--probe-input-at',seed),code=1)
        self.run_case('same-frame-duplicate',options=('--probe-steps','8','--probe-input-at','1:9=01','--probe-input-at','01:9=00'),code=1)
        self.run_case('timed-array-rejected',model=BASE+'21 sort array 2 2\n22 input 21 array\n',options=('--probe-steps','8','--probe-input-at','1:22=00'),code=1)
        self.run_case('timed-legacy-mode-rejected',options=('--cex','missing','--probe-input-at','1:9=01'),code=1)

if __name__=='__main__':unittest.main()
