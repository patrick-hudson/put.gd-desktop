#!/usr/bin/env bash
# Sanitizer job: build an AddressSanitizer-instrumented QtCore that matches the
# distro Qt exactly. Distro QtCore is not instrumented, so ASan cannot check a
# load that happens inside it: the QJsonValueRef into a destroyed temporary
# that #76 fixed was read only by exported QtCore functions and passed both
# GCC and clang ASan. The application still compiles against the distro
# headers; the tests load this library in place of the distro one through
# LD_LIBRARY_PATH, so distro QtGui, QtSql and QtTest bind to it.
set -euo pipefail
version=6.10.2
sha256=aeb78d29291a2b5fd53cb55950f8f5065b4978c25fb1d77f627d695ab9adf21e
prefix="${1:-build/qt-asan}"
work=build/qt-asan-source
distro="$(dpkg-query -W -f '${Version}' libqt6core6t64)"
case "$distro" in
  "$version"+*|"$version"-*) ;;
  *) echo "Distro QtCore is $distro, but this recipe pins $version; update both together." >&2; exit 1 ;;
esac
archive="$work/qtbase-everywhere-src-$version.tar.xz"
mkdir -p "$work"
curl --fail --silent --show-error --location --output "$archive" \
  "https://download.qt.io/archive/qt/${version%.*}/$version/submodules/qtbase-everywhere-src-$version.tar.xz"
echo "$sha256  $archive" | sha256sum --check --strict
tar -xf "$archive" -C "$work"
# QLibraryInfo must report the distro's absolute paths, so the distro plugins
# load (QSQLITE, image formats, offscreen) in every child process without
# QT_PLUGIN_PATH. Core is the only module rebuilt; the other features are off.
query() { /usr/lib/qt6/bin/qtpaths6 --query "$1"; }
qt_prefix="$(query QT_INSTALL_PREFIX)"
libdir="$(query QT_INSTALL_LIBS | sed "s|^$qt_prefix/||")"
dirs=()
for pair in LIBDIR:QT_INSTALL_LIBS ARCHDATADIR:QT_INSTALL_ARCHDATA DATADIR:QT_INSTALL_DATA \
    PLUGINSDIR:QT_INSTALL_PLUGINS LIBEXECDIR:QT_INSTALL_LIBEXECS BINDIR:QT_INSTALL_BINS \
    TRANSLATIONSDIR:QT_INSTALL_TRANSLATIONS QMLDIR:QT_INSTALL_QML INCLUDEDIR:QT_INSTALL_HEADERS; do
  dirs+=("-DINSTALL_${pair%%:*}=$(query "${pair#*:}" | sed "s|^$qt_prefix/||")")
done
cmake -G Ninja -S "$work/qtbase-everywhere-src-$version" -B "$work/build" \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_INSTALL_PREFIX="$qt_prefix" -DFEATURE_relocatable=OFF "${dirs[@]}" \
  -DFEATURE_sanitize_address=ON -DBUILD_WITH_PCH=OFF -DQT_BUILD_EXAMPLES=OFF -DQT_BUILD_TESTS=OFF \
  -DFEATURE_gui=OFF -DFEATURE_widgets=OFF -DFEATURE_network=OFF -DFEATURE_sql=OFF \
  -DFEATURE_dbus=OFF -DFEATURE_testlib=OFF -DFEATURE_xml=OFF -DFEATURE_concurrent=OFF
ninja -C "$work/build" -j "${PUTGD_QT_JOBS:-$(nproc)}" Core
rm -rf "$prefix"
mkdir -p "$prefix/lib"
cp -a "$work/build/$libdir/"libQt6Core.so.6* "$prefix/lib/"
printf 'qtbase %s %s\n%s\n' "$version" "$sha256" "$(clang++ --version | head -n 1)" > "$prefix/BUILD"
rm -rf "$work"
