#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)}"

cd "$ROOT_DIR"

# Use the URLs and pinned commits recorded by this parent checkout. Never
# follow a submodule's remote branch or force away local source changes.
echo "[setup] updating git submodules to pinned commits"
git submodule sync --recursive
git submodule update --init --recursive --checkout

apply_dependency_patch() {
    local directory="$ROOT_DIR/$1" patch="$ROOT_DIR/$2"
    local expected actual
    expected="$(git -C "$ROOT_DIR" rev-parse "HEAD:$1")"
    actual="$(git -C "$directory" rev-parse HEAD)"
    if [[ "$actual" != "$expected" ]]; then
        echo "[setup] $1 is at $actual, expected $expected from the parent commit" >&2
        return 1
    fi
    if git -C "$directory" apply --check "$patch" >/dev/null 2>&1; then
        git -C "$directory" apply "$patch"
        echo "[setup] applied $2"
    elif git -C "$directory" apply --reverse --check "$patch" >/dev/null 2>&1; then
        echo "[setup] $2 already applied"
    else
        echo "[setup] $2 conflicts with dependency sources; refusing to overwrite local changes" >&2
        return 1
    fi
}

# Patch before compiling either dependency, including on repeated setup runs.
apply_dependency_patch src/sat/minisat src/sat/minisat.patch
apply_dependency_patch deps/btor2tools deps/btor2tools.patch

BTOR2TOOLS_DIR="$ROOT_DIR/deps/btor2tools"
BOOLECTOR_LIB="$BTOR2TOOLS_DIR/deps/install/lib/libboolector.a"
if [[ ! -f "$BOOLECTOR_LIB" ]]; then
    echo "[setup] building btor2tools dependencies"
    (
        cd "$BTOR2TOOLS_DIR"
        ./setup-deps.sh
    )
fi

echo "[setup] building btor2tools"
(
    cd "$BTOR2TOOLS_DIR"
    if [[ ! -f build/CMakeCache.txt ]]; then
        ./configure.sh
    fi
    # An existing executable may predate the patch. Always run the incremental
    # build so changes to patched sources are picked up.
    cmake --build build -j "$JOBS"
)

echo "[setup] building cadical"
(
    cd "$ROOT_DIR/src/sat/cadical"
    CADICAL_PREFIX_HEADER="../../cadical-kitten-symbol-prefix.h"
    CADICAL_FORCE_INCLUDE="-include $CADICAL_PREFIX_HEADER"
    CADICAL_CFLAGS="$CADICAL_FORCE_INCLUDE${CFLAGS:+ $CFLAGS}"
    CADICAL_CXXFLAGS="$CADICAL_FORCE_INCLUDE${CXXFLAGS:+ $CXXFLAGS}"
    # CaDiCaL's generated makefile does not track compiler flag changes.
    if [[ -f makefile ]]; then
        make clean
    fi
    CFLAGS="$CADICAL_CFLAGS" CXXFLAGS="$CADICAL_CXXFLAGS" \
        ./configure --competition
    make -j"$JOBS"
)

echo "[setup] building kissat"
(
    cd "$ROOT_DIR/src/sat/kissat"
    ./configure --competition
    make -j"$JOBS"
)

echo "[setup] done"
