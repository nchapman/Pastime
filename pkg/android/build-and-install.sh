#!/usr/bin/env bash
# Build the aarch64 debug APK and install it on the connected device.
#
# Requires: mise (provides JDK 11 — see phoenix/.mise.toml), adb, and an
# Android device attached (`adb devices` shows it).
#
# Usage: pkg/android/build-and-install.sh [release|debug]   # default: debug

set -euo pipefail

VARIANT="${1:-debug}"
case "$VARIANT" in
   debug)   GRADLE_TASK="assembleAarch64Debug"   ; APK_DIR="aarch64/debug"   ; APK_NAME="phoenix-aarch64-debug.apk"   ;;
   release) GRADLE_TASK="assembleAarch64Release" ; APK_DIR="aarch64/release" ; APK_NAME="phoenix-aarch64-release.apk" ;;
   *) echo "usage: $0 [debug|release]" >&2; exit 2 ;;
esac

cd "$(dirname "$0")/phoenix"

# mise activates JDK 11 from .mise.toml in this directory.
mise exec -- ./gradlew "$GRADLE_TASK"

APK="build/outputs/apk/$APK_DIR/$APK_NAME"
[[ -f "$APK" ]] || { echo "APK not found at $APK" >&2; exit 1; }

# Refuse to guess if multiple devices are attached — adb would silently
# pick one or fail opaquely.  Set ANDROID_SERIAL to disambiguate.
DEVICES=$(adb devices | awk 'NR>1 && $2=="device" {print $1}')
COUNT=$(printf '%s\n' "$DEVICES" | grep -c .)
if [[ -n "${ANDROID_SERIAL:-}" ]]; then
   ADB_TARGET=(-s "$ANDROID_SERIAL")
elif [[ "$COUNT" -eq 1 ]]; then
   ADB_TARGET=()
elif [[ "$COUNT" -eq 0 ]]; then
   echo "No Android device attached." >&2; exit 1
else
   echo "Multiple devices attached; set ANDROID_SERIAL to one of:" >&2
   printf '  %s\n' $DEVICES >&2; exit 1
fi

echo "Installing $APK"
adb ${ADB_TARGET[@]+"${ADB_TARGET[@]}"} install -r "$APK"

# Force-stop any running instance — `install -r` leaves the existing
# process untouched, which means re-foregrounding the app keeps the OLD
# binary mapped and the freshly installed code is silently ignored.
# Bit us when iterating on debug logging.
adb ${ADB_TARGET[@]+"${ADB_TARGET[@]}"} shell am force-stop gg.pastime.aarch64
