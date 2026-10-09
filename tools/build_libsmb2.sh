#!/bin/sh
# Builds libsmb2 (SMB2/3 client, LGPL-2.1, github.com/sahlberg/libsmb2; submodule
# client/external/libsmb2) into client/lib/libsmb2.
#   tools/build_libsmb2.sh [work_dir]     (needs devkitARM, cmake, ninja; run by client/Makefile)
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
SOURCE="$HERE/client/external/libsmb2"
WORK=${1:-/tmp/tube3d-libsmb2}
PREFIX="$HERE/client/lib/libsmb2"
: "${DEVKITPRO:=/opt/devkitpro}"
export DEVKITPRO DEVKITARM="$DEVKITPRO/devkitARM" PATH="$DEVKITPRO/devkitARM/bin:$HOME/.local/bin:$PATH"
[ -f "$SOURCE/CMakeLists.txt" ] || { echo "libsmb2's source is missing: run git submodule update --init"; exit 1; }

mkdir -p "$WORK"
cd "$WORK"
rm -rf src build
mkdir src
git -C "$SOURCE" archive HEAD | tar -x -C src
# The 3DS's SO_ERROR says "in progress" after poll() reports writable; unpatched, every connect fails
(cd src && git apply "$HERE/tools/libsmb2-3ds-connect.patch") || { echo "libsmb2 patch failed"; exit 1; }
cmake -S src -B build -G Ninja "-DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake" \
	-DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DENABLE_EXAMPLES=OFF -DCMAKE_INSTALL_PREFIX="$PREFIX"
cmake --build build
rm -rf "$PREFIX"
cmake --build build --target install
arm-none-eabi-strip -g "$PREFIX"/lib/*.a
rm -rf "$PREFIX/lib/pkgconfig" "$PREFIX/lib/cmake"
echo "installed into $PREFIX"
ls -la "$PREFIX/lib"
