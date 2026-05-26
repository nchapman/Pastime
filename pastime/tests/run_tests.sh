#!/usr/bin/env bash
# Build and run the Pastime menu-driver unit tests.
#
# Standalone — does not go through ./configure + make.  Each test
# binary is built directly via the host C compiler against the
# real production source files (no carbon copies).  Production
# headers (boolean.h, retro_common_api.h) come from
# libretro-common/include/.  Production source files use
# PASTIME_NAV_TEST_BUILD to swap RA's full verbosity macros for
# the test stubs defined in the test file.
#
# Usage:
#   bash pastime/tests/run.sh
#
# Exits non-zero if any test fails.

set -euo pipefail

cd "$(dirname "$0")"

CC="${CC:-cc}"
PROJECT_ROOT="$(cd ../.. && pwd)"
INC="-I${PROJECT_ROOT}/libretro-common/include"
DEFS="-DPASTIME_NAV_TEST_BUILD"
CFLAGS="${CFLAGS:--std=c99 -Wall -Wextra -Wpedantic -Wno-unused-function -O0 -g}"

# On glibc/Linux under strict -std=c99, strdup needs a feature macro
# to be declared.  macOS is BSD-derived and exposes both strdup and
# strlcpy unconditionally — adding _POSIX_C_SOURCE there *removes*
# strlcpy (which compat/strl.h then expects to be in libc), breaking
# the build.  Apply the define only on non-Darwin hosts.
if [[ "$(uname -s)" != "Darwin" ]]; then
   CFLAGS="$CFLAGS -D_DEFAULT_SOURCE"
fi

# Track binaries we build this run; only those get executed below.
# Globbing test_* would otherwise re-run stale binaries left behind
# by a previous failed build.
BUILT=()

# --- nav stack ---
echo "== building test_nav"
$CC $CFLAGS $DEFS $INC \
    test_nav.c ../pastime_nav.c \
    -o test_nav
BUILT+=(test_nav)

# --- metadata disambiguation table ---
echo "== building test_metadata_disambig"
$CC $CFLAGS $INC \
    test_metadata_disambig.c ../pastime_metadata_disambig.c \
    -o test_metadata_disambig
BUILT+=(test_metadata_disambig)

# --- display-name cleaner ---
echo "== building test_display_name"
$CC $CFLAGS $INC \
    test_display_name.c ../pastime_display_name.c \
    -o test_display_name
BUILT+=(test_display_name)

# --- ROM-row label disambiguation ---
echo "== building test_disambig"
$CC $CFLAGS $INC \
    test_disambig.c ../pastime_disambig.c \
    -o test_disambig
BUILT+=(test_disambig)

# --- cores extras table ---
echo "== building test_cores_extras"
$CC $CFLAGS $INC \
    test_cores_extras.c ../pastime_cores_extras.c \
    -o test_cores_extras
BUILT+=(test_cores_extras)

# --- external-emulator preset table + marker parser ---
echo "== building test_external"
$CC $CFLAGS -DPASTIME_EXTERNAL_TEST_BUILD $INC \
    test_external.c ../pastime_external.c \
    -o test_external
BUILT+=(test_external)

# --- thumbnail match cascade ---
# Pure side only: pastime_thumbs_index.c carries the parse + match
# cascade with no HTTP/IO/log dependencies, so we link it directly
# (no PASTIME_THUMBS_TEST_BUILD stub dance — the manager file isn't
# linked in).
echo "== building test_thumbs"
# compat_strl.c provides strlcpy_retro__/strlcat_retro__ on platforms
# where strlcpy isn't in libc (Linux/glibc).  macOS auto-defines
# HAVE_STRL in compat/strl.h, so linking the shim there would conflict.
COMPAT_STRL=()
if [[ "$(uname -s)" != "Darwin" ]]; then
   COMPAT_STRL=("${PROJECT_ROOT}/libretro-common/compat/compat_strl.c")
fi
$CC $CFLAGS $INC \
    test_thumbs.c ../pastime_thumbs_index.c \
    "${PROJECT_ROOT}/libretro-common/formats/json/rjson.c" \
    ${COMPAT_STRL[@]+"${COMPAT_STRL[@]}"} \
    -o test_thumbs
BUILT+=(test_thumbs)

echo "== building test_rommap"
$CC $CFLAGS $INC \
    test_rommap.c \
    -o test_rommap
BUILT+=(test_rommap)

# --- scan primitives (baked cache, resolve, .disabled) ---
echo "== building test_scan"
$CC $CFLAGS $INC \
    test_scan.c \
    -o test_scan
BUILT+=(test_scan)

# --- run only what we just built ---
fail=0
for bin in "${BUILT[@]}"; do
   echo "== running $bin"
   ./"$bin" || fail=1
   echo
done

exit $fail
