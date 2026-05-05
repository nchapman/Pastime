#!/usr/bin/env bash
# Build and run the Downplay menu-driver unit tests.
#
# Standalone — does not go through ./configure + make.  Each test
# binary is built directly via the host C compiler against the
# real production source files (no carbon copies).  Production
# headers (boolean.h, retro_common_api.h) come from
# libretro-common/include/.  Production source files use
# DOWNPLAY_NAV_TEST_BUILD to swap RA's full verbosity macros for
# the test stubs defined in the test file.
#
# Usage:
#   bash downplay/tests/run.sh
#
# Exits non-zero if any test fails.

set -euo pipefail

cd "$(dirname "$0")"

CC="${CC:-cc}"
PROJECT_ROOT="$(cd ../.. && pwd)"
INC="-I${PROJECT_ROOT}/libretro-common/include"
DEFS="-DDOWNPLAY_NAV_TEST_BUILD"
CFLAGS="${CFLAGS:--std=c99 -Wall -Wextra -Wpedantic -Wno-unused-function -O0 -g}"

# --- nav stack ---
echo "== building test_nav"
$CC $CFLAGS $DEFS $INC \
    test_nav.c ../downplay_nav.c \
    -o test_nav

# --- metadata disambiguation table ---
echo "== building test_metadata_disambig"
$CC $CFLAGS $INC \
    test_metadata_disambig.c ../downplay_metadata_disambig.c \
    -o test_metadata_disambig

# --- display-name cleaner ---
echo "== building test_display_name"
$CC $CFLAGS $INC \
    test_display_name.c ../downplay_display_name.c \
    -o test_display_name

# --- thumbnail match cascade ---
echo "== building test_thumbs"
$CC $CFLAGS -DDOWNPLAY_THUMBS_TEST_BUILD $INC \
    test_thumbs.c ../downplay_thumbs.c ../downplay_display_name.c \
    "${PROJECT_ROOT}/libretro-common/formats/json/rjson.c" \
    -o test_thumbs

# --- run all built tests ---
fail=0
for bin in test_*; do
   # Skip non-files (clang's debug-symbol dirs, source files).
   [[ -f "$bin" && -x "$bin" && ! "$bin" == *.c ]] || continue
   echo "== running $bin"
   ./"$bin" || fail=1
   echo
done

exit $fail
