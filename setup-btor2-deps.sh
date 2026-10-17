#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEPS_DIR="$ROOT_DIR/deps/btor2tools/deps"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 1)}"
if [[ "$(uname -s)" == Darwin ]]; then
    export CC="${CC:-/usr/bin/clang}"
    export CXX="${CXX:-/usr/bin/clang++}"
elif [[ "$(uname -s)" == Linux ]]; then
    export CC="${CC:-/usr/bin/gcc}"
    export CXX="${CXX:-/usr/bin/g++}"
fi

# These are the bitblast-api version and its upstream dependency revisions.
BOOLECTOR_COMMIT=0eafd0c0d98f5e14aca7cc105c7eab8e2ba29916
BTOR2PARSER_COMMIT=1df768d75adfb13a8f922f5ffdd1d58e80cb1cc2
LINGELING_COMMIT=03b4860d14016f42213ea271014f2f13d181f504

fetch_source() {
    local repo="$1" revision="$2" destination="$3"
    if [[ -d "$destination" ]]; then
        return
    fi
    local archive="$DEPS_DIR/$(basename "$destination")-$revision.tar.gz"
    curl --fail --location --retry 3 \
        "https://github.com/$repo/archive/$revision.tar.gz" -o "$archive"
    mkdir -p "$destination"
    tar -xzf "$archive" -C "$destination" --strip-components=1
}

mkdir -p "$DEPS_DIR"
BOOLECTOR_DIR="$DEPS_DIR/boolector"
fetch_source boolector/boolector "$BOOLECTOR_COMMIT" "$BOOLECTOR_DIR"
# An existing Git checkout must match the pinned bit-blast API version.
if [[ -e "$BOOLECTOR_DIR/.git" ]] && \
   [[ "$(git -C "$BOOLECTOR_DIR" rev-parse HEAD)" != "$BOOLECTOR_COMMIT" ]]; then
    echo "[setup] unexpected Boolector revision in $BOOLECTOR_DIR" >&2
    exit 1
fi

INSTALL_DIR="$BOOLECTOR_DIR/deps/install"
mkdir -p "$INSTALL_DIR/lib" "$INSTALL_DIR/include/btor2parser"
if [[ ! -f "$INSTALL_DIR/lib/libbtor2parser.a" ]]; then
    PARSER_DIR="$BOOLECTOR_DIR/deps/btor2tools"
    fetch_source boolector/btor2tools "$BTOR2PARSER_COMMIT" "$PARSER_DIR"
    (cd "$PARSER_DIR"; ./configure.sh -fPIC; make -j"$JOBS")
    cp "$PARSER_DIR/build/libbtor2parser.a" "$INSTALL_DIR/lib/"
    cp "$PARSER_DIR/src/btor2parser/btor2parser.h" "$INSTALL_DIR/include/btor2parser/"
fi
if [[ ! -f "$INSTALL_DIR/lib/liblgl.a" ]]; then
    LINGELING_DIR="$BOOLECTOR_DIR/deps/lingeling"
    fetch_source arminbiere/lingeling "$LINGELING_COMMIT" "$LINGELING_DIR"
    (cd "$LINGELING_DIR"; ./configure.sh -fPIC; make -j"$JOBS" liblgl.a)
    cp "$LINGELING_DIR/liblgl.a" "$INSTALL_DIR/lib/"
    cp "$LINGELING_DIR/lglib.h" "$INSTALL_DIR/include/"
fi

# Boolector declares CMake 3.3 compatibility; CMake 4 needs a policy floor.
cmake -S "$BOOLECTOR_DIR" -B "$BOOLECTOR_DIR/build" \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$DEPS_DIR/install" \
    -DBUILD_SHARED_LIBS=OFF -DONLY_LINGELING=ON -DUSE_GMP=OFF
cmake --build "$BOOLECTOR_DIR/build" -j "$JOBS"
cmake --install "$BOOLECTOR_DIR/build"
