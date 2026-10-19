#!/bin/bash
set -euo pipefail
qt_prefix="${1:?Usage: bash test/osx/run-qt-colorspace-test.sh QT_PREFIX OUTPUT_DIR [RUNTIME_FRAMEWORK_DIR]}"
output_dir="${2:?Supply a task-owned output directory}"
runtime_dir="${3:-$qt_prefix/lib}"
test_dir="$(cd "$(dirname "$0")" && pwd)"
test -d "$output_dir"
xcrun clang++ -std=c++17 -mmacosx-version-min=13.0 -dynamiclib -DTEST_QT_INTERPOSE \
	"$test_dir/test-qt-colorspace-lifetime.mm" -framework CoreGraphics -framework CoreFoundation \
	-o "$output_dir/qt-colorspace-hooks.dylib"
xcrun clang++ -std=c++17 -mmacosx-version-min=13.0 "$test_dir/test-qt-colorspace-lifetime.mm" \
	-F"$qt_prefix/lib" -framework QtGui -framework QtCore -framework CoreGraphics \
	-Wl,-rpath,"$runtime_dir" -o "$output_dir/test-qt-colorspace-lifetime"
# These hooks run only in this isolated test process, never in Ethan's OBS session.
DYLD_INSERT_LIBRARIES="$output_dir/qt-colorspace-hooks.dylib" "$output_dir/test-qt-colorspace-lifetime"
