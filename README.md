# Carat for C2Btor Witness

This version of Carat is developed specifically for the **C2Btor witness
pipeline**. It checks concrete counterexamples against the original input BTOR2
model and exports counterexamples plus execution observations for Python to
recover C executions and generate SV-COMP YAML 2.1 witnesses. The normal recovery
path does not require a separate BtorSim/VCD replay.

## Build

Requires Git, CMake, Make, Clang/LLVM with LLD, and wget. The C++ sources use C++17.
The build commands are unchanged:

```bash
./setup.sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
```

`setup.sh` initializes submodules using the URLs and exact commits recorded by
the current parent checkout, applies both dependency patches, then builds them:

| Patch | Submodule base commit |
| --- | --- |
| `src/sat/minisat.patch` | `37dc6c67e2af26379d88ce349eb9c4c6160e8543` |
| `deps/btor2tools.patch` | `d33c73ff1d173f1bfac8ba6b1c6d68ba62c55f8e` |

Already applied patches are skipped. Conflicts stop setup without overwriting
local edits. BtorSim is incrementally rebuilt even if its executable exists.
Submodule branches need not share the parent branch's name; setup does not follow
remote branch tips. A shared checkout also shares its submodule working
directories. `cmake --build` only compiles; run `./setup.sh` first.

## Four solver configurations

These are the four configurations used in the C2Btor experiment results. Choose
one command for each run, using a fresh output directory:

```bash
mkdir -p witness-out

# CARAT ic3: IC3 with CTG (default SAT solver: minicore).
./build/carat -a ic3 -w witness-out/ model.btor2

# CARAT ic3_noctg: IC3 with CTG disabled.
./build/carat -a ic3 --ctg_max_lvl 0 -w witness-out/ model.btor2

# CARAT ic3_all: IC3 with active lemma learning, CTG disabled.
./build/carat -a ic3 --all --ctg_max_lvl 0 -w witness-out/ model.btor2

# CARAT wlbmc: word-level bounded model checking with Kissat, bound 100.
./build/carat -a wlbmc -s kissat -k 100 -w witness-out/ model.btor2
```

The three IC3 commands use the default `minicore` SAT solver. WLBMC finding no
counterexample within its bound is **Unknown**, not Safe. C2Btor should merge
source assertions into one BTOR2 bad property for these runs.

For an Unsafe result with a confirmed counterexample, the output is:

```text
witness-out/model.btor2.cexb
witness-out/model.btor2.cexb.obs.jsonl
```

The `.cexb` contains concrete bit-vector and sparse-array assignments. Its
`.obs.jsonl` companion contains observations from the internal confirmation,
bound to the model and counterexample by hashes. Keep both files together.
Python uses them with the C2Btor model and source map to generate `witness.yml`;
Carat itself does not write the C-level YAML witness. Safe results use the
existing Python dummy correctness-witness path.

Run `./build/carat -h` to see all available options.

## Portfolio runs (Linux, Python 3.8+)

Run `python3 scripts/portfolio.py --type <safety|array|liveness> <model-file>` for
parallel portfolio verification, or use `--help` for options.
