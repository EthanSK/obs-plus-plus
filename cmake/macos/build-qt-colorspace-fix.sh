#!/bin/bash
set -euo pipefail
source_dir="$(cd "$(dirname "$0")/../.." && pwd)"

# Backport Qt's QTBUG-147602 fix without upgrading OBS's other dependencies or changing its renderer.
qt_prefix="${1:?Usage: bash cmake/macos/build-qt-colorspace-fix.sh QT_6_11_1_PREFIX WORK_DIR.noindex}"
work_dir="${2:?Supply a task-owned .noindex scratch directory}"
[[ "$work_dir" = /* && "$work_dir" == *'.noindex'* ]]
test -f "$qt_prefix/lib/cmake/Qt6/Qt6Config.cmake"
grep -q 'QT_VERSION_STR "6.11.1"' "$qt_prefix/lib/QtCore.framework/Headers/qconfig.h"
mkdir -p "$work_dir"
qt_prefix="$(cd "$qt_prefix" && pwd)"
work_dir="$(cd "$work_dir" && pwd)"
cd "$work_dir"

download() {
	local url="$1" filename="$2" checksum="$3"
	if [[ ! -f "$filename" ]]; then curl --fail --location --retry 2 "$url" -o "$filename"; fi
	printf '%s  %s\n' "$checksum" "$filename" | shasum -a 256 --check
}

download 'https://github.com/qt/qtbase/archive/refs/tags/v6.11.1.tar.gz' qtbase-6.11.1.tar.gz \
	e20852bd45cdef5da5175f3634e285e03e2be7ca437f3d2b7e1a2af7321bca7a
download 'https://github.com/qt/qtbase/commit/08e464f8559719662c5b2207d1d4c84c251eca07.patch' QTBUG-147602.patch \
	9d9a71b733f9105feb2c710c3c8cf73de96cf5ea685de15a2a125006e5c576b8
if [[ ! -d qtbase-6.11.1 ]]; then tar -xzf qtbase-6.11.1.tar.gz; fi
if ! grep -q 'struct Q_GUI_EXPORT QCGImageFormat' qtbase-6.11.1/src/gui/painting/qcoregraphics_p.h; then
	(cd qtbase-6.11.1 && patch --batch -p1 -i ../QTBUG-147602.patch)
fi

# Match obs-deps 2026-07-15's Qt build flags; build only Gui and its prerequisites, not a new Qt installation.
cmake -S qtbase-6.11.1 -B qt-build.noindex -G 'Unix Makefiles' \
	-DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="$(uname -m)" -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 \
	-DCMAKE_INSTALL_PREFIX="$work_dir/qt-install.noindex" -DBUILD_SHARED_LIBS=ON -DFEATURE_rpath=ON \
	-DFEATURE_brotli=OFF -DFEATURE_dbus=OFF -DFEATURE_doubleconversion=ON -DFEATURE_glib=OFF \
	-DFEATURE_jpeg=ON -DFEATURE_macdeployqt=OFF -DFEATURE_pcre2=ON -DFEATURE_pdf=OFF \
	-DFEATURE_png=ON -DFEATURE_printsupport=OFF -DFEATURE_qmake=OFF -DFEATURE_separate_debug_info=ON \
	-DFEATURE_sql=OFF -DFEATURE_system_doubleconversion=OFF -DFEATURE_system_jpeg=OFF \
	-DFEATURE_system_pcre2=OFF -DFEATURE_system_png=OFF -DFEATURE_system_zlib=ON \
	-DFEATURE_testlib=OFF -DINPUT_openssl=no -DQT_BUILD_BENCHMARKS=OFF -DQT_BUILD_EXAMPLES=OFF \
	-DQT_BUILD_MANUAL_TESTS=OFF -DQT_BUILD_TESTS=OFF -DQT_BUILD_TOOLS_BY_DEFAULT=OFF \
	-DQT_CREATE_VERSIONED_HARD_LINK=OFF -DQT_USE_VCPKG=OFF -DQT_FEATURE_force_debug_info=ON
cmake --build qt-build.noindex --target Gui --parallel 6
cmake --install qt-build.noindex/src/gui --strip

if [[ ! -d patched-qt.noindex ]]; then ditto "$qt_prefix" patched-qt.noindex; fi
ditto qt-install.noindex/lib/QtGui.framework patched-qt.noindex/lib/QtGui.framework
mkdir -p test-qt.noindex
bash "$source_dir/test/osx/run-qt-colorspace-test.sh" "$work_dir/patched-qt.noindex" "$work_dir/test-qt.noindex"
printf 'Patched Qt prefix: %s/patched-qt.noindex\n' "$work_dir"
