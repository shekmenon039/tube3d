#!/bin/sh
# Builds libssh2 (BSD-3-Clause, libssh2.org; submodule client/external/libssh2, tag
# libssh2-1.11.1) with the 3ds-mbedtls backend into client/lib/libssh2.
#   tools/build_libssh2.sh [work_dir]     (needs devkitARM, 3ds-mbedtls, cmake, ninja; run by client/Makefile)
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
SOURCE="$HERE/client/external/libssh2"
WORK=${1:-/tmp/tube3d-libssh2}
PREFIX="$HERE/client/lib/libssh2"
: "${DEVKITPRO:=/opt/devkitpro}"
export DEVKITPRO DEVKITARM="$DEVKITPRO/devkitARM" PATH="$DEVKITPRO/devkitARM/bin:$HOME/.local/bin:$PATH"
PORTLIBS="$DEVKITPRO/portlibs/3ds"
[ -f "$SOURCE/CMakeLists.txt" ] || { echo "libssh2's source is missing: run git submodule update --init"; exit 1; }

mkdir -p "$WORK"
cd "$WORK"
rm -rf src build shim
mkdir src
git -C "$SOURCE" archive HEAD | tar -x -C src
# The git tree says "1.11.1_DEV"; drop the suffix as libssh2's release script does
sed -i '/#define LIBSSH2_VERSION /s/_DEV"/"/' src/include/libssh2.h
grep -q '#define LIBSSH2_VERSION  *"[0-9.]*"' src/include/libssh2.h || { echo "libssh2 version fix did not apply"; exit 1; }
# newlib on the 3DS has no sys/uio.h; libssh2 only needs struct iovec
mkdir -p shim/sys
printf '#pragma once\n#include <stddef.h>\nstruct iovec { void* iov_base; size_t iov_len; };\n' > shim/sys/uio.h
CFLAGS="-I$WORK/shim -include sys/uio.h" cmake -S src -B build -G Ninja \
	"-DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake" -DPICKY_COMPILER=OFF \
	-DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF \
	-DCRYPTO_BACKEND=mbedTLS -DENABLE_ZLIB_COMPRESSION=OFF \
	"-DMBEDTLS_INCLUDE_DIR=$PORTLIBS/include" "-DMBEDCRYPTO_LIBRARY=$PORTLIBS/lib/libmbedcrypto.a" \
	"-DMBEDTLS_LIBRARY=$PORTLIBS/lib/libmbedtls.a" "-DMBEDX509_LIBRARY=$PORTLIBS/lib/libmbedx509.a" \
	-DCMAKE_INSTALL_PREFIX="$PREFIX"
cmake --build build
rm -rf "$PREFIX"
cmake --build build --target install
arm-none-eabi-strip -g "$PREFIX"/lib/*.a
rm -rf "$PREFIX/lib/pkgconfig" "$PREFIX/lib/cmake" "$PREFIX/share"
echo "installed into $PREFIX"
ls -la "$PREFIX/lib"
