#!/bin/bash
set -euo pipefail

# Supply an existing configured OBS build and a temporary output directory. These tests do not launch OBS.
build_dir="${1:?Usage: bash test/osx/run-resource-lifetime-tests.sh BUILD_DIR TEMP_OUTPUT_DIR}"
output_dir="${2:?Supply a temporary output directory}"
test_dir="$(cd "$(dirname "$0")" && pwd)"
source_dir="$(cd "$test_dir/../.." && pwd)"
build_dir="$(cd "$build_dir" && pwd)"
test -d "$output_dir"
simde_include="$(sed -n 's/^SIMDe_INCLUDE_DIR:PATH=//p' "$build_dir/CMakeCache.txt")"
test -f "$simde_include/simde/x86/sse2.h"
framework_dir="$build_dir/libobs/RelWithDebInfo"
runtime_dir="$build_dir/frontend/RelWithDebInfo/OBS++.app/Contents/Frameworks"
test -f "$framework_dir/libobs.framework/Versions/A/libobs"
test -d "$runtime_dir"

common_flags=(-mmacosx-version-min=13.0 -Wno-deprecated-declarations
    -I"$source_dir/libobs" -I"$build_dir/config" -isystem "$simde_include" -F"$framework_dir"
    -framework libobs -framework CoreMedia -framework CoreVideo -framework CoreFoundation
    -Wl,-dead_strip -Wl,-rpath,"$framework_dir" -Wl,-rpath,"$runtime_dir")

xcrun clang -std=c11 "${common_flags[@]}" "$test_dir/test-videotoolbox-lifetime.c" \
    -framework VideoToolbox -o "$output_dir/test-videotoolbox-lifetime"
"$output_dir/test-videotoolbox-lifetime"

for mode in sck legacy; do
    extra_flags=(-UTEST_LEGACY_CAPTURE)
    if [[ "$mode" == legacy ]]; then extra_flags=(-DTEST_LEGACY_CAPTURE); fi
    xcrun clang "${common_flags[@]}" "${extra_flags[@]}" "$test_dir/test-capture-surface-lifetime.m" \
        -framework Cocoa -framework IOSurface -framework ScreenCaptureKit \
        -o "$output_dir/test-$mode-surface-lifetime"
    "$output_dir/test-$mode-surface-lifetime"
done
