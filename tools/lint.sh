#!/bin/sh
# Lints the client's C/C++ (strict gcc warnings, clang-tidy, clang-format) and
# the Python tools (ruff, mypy; settings in pyproject.toml), and scans for leaked API keys.
#   sh tools/lint.sh      (needs devkitARM; clang-tidy, ruff, mypy are skipped when missing)
# Exits non-zero if anything is reported. Deliberately off:
#   -Wcast-align: byte buffers are aligned by construction (ring.h, linearMemAlign)
#   -Wstrict-/-Wmissing-prototypes: hundreds of hits in libctru and FFmpeg headers
#   clang-tidy EnumCastOutOfRange: UA_MODE_0 + mode is a reserved range (ui.h)
#   clang-tidy bugprone-integer-division: ui.c's whole-pixel layout truncates on purpose
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DKP=${DEVKITPRO:-/opt/devkitpro}
CLIENT="$ROOT/client"
SRC="$CLIENT/source"
status=0

INCLUDES="-I$CLIENT/external/rapidjson/include -I$CLIENT/build -D__3DS__"   # build/: generated headers (make prepare first)
SYSTEM_INCLUDES="-isystem $CLIENT/lib/ffmpeg/include -isystem $CLIENT/lib/libsmb2/include -isystem $CLIENT/lib/libssh2/include -isystem $DKP/portlibs/3ds/include -isystem $DKP/libctru/include"
ARCH="-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -O2"
WARN="-Wall -Wextra -Wno-unused-parameter -Wshadow -Wformat=2 -Wduplicated-cond -Wduplicated-branches \
-Wlogical-op -Wnull-dereference -Wpointer-arith -Wundef -Wmissing-declarations -Wunused-macros \
-Wredundant-decls -Wmissing-field-initializers"

report() {   # $1 = findings
  if [ -n "$1" ]; then echo "$1"; status=1; else echo "clean"; fi
}

gcc_check() {   # $1 = extra flags, then the folders
  flags=$1
  shift
  for dir in "$@"; do
    for file in "$dir"/*.c; do
      "$DKP/devkitARM/bin/arm-none-eabi-gcc" -std=gnu11 -fsyntax-only $ARCH $WARN -Wold-style-definition \
        $INCLUDES $flags $SYSTEM_INCLUDES "$file" 2>&1
    done
    for file in "$dir"/*.cpp; do
      [ -f "$file" ] || continue
      "$DKP/devkitARM/bin/arm-none-eabi-g++" -std=gnu++17 -fno-rtti -fno-exceptions -fsyntax-only $ARCH $WARN \
        $INCLUDES $flags $SYSTEM_INCLUDES "$file" 2>&1
    done
  done
}

echo "== compiler warnings, release build (arm-none-eabi-gcc/g++ -fsyntax-only)"
report "$(gcc_check "" "$SRC")"

if command -v clang-tidy >/dev/null 2>&1; then
  echo "== clang-tidy (bugprone, static analyzer)"
  GCC_INCLUDE=$(ls -d "$DKP"/devkitARM/lib/gcc/arm-none-eabi/*/include | head -n 1)
  CXX_INCLUDE=$(ls -d "$DKP"/devkitARM/arm-none-eabi/include/c++/* | head -n 1)
  TARGET="--target=arm-none-eabi -march=armv6k -mfloat-abi=hard -D__arm__ \
-isystem $DKP/devkitARM/arm-none-eabi/include -isystem $GCC_INCLUDE $SYSTEM_INCLUDES $INCLUDES"
  CHECKS='-*,bugprone-*,clang-analyzer-*,-bugprone-easily-swappable-parameters,-bugprone-reserved-identifier,'\
'-bugprone-narrowing-conversions,-bugprone-implicit-widening-of-multiplication-result,'\
'-bugprone-assignment-in-if-condition,-bugprone-multi-level-implicit-pointer-conversion,-bugprone-integer-division,'\
'-clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling,-clang-analyzer-optin.core.EnumCastOutOfRange'
  FINDINGS="^$CLIENT/source/.*(warning|error):"
  report "$(
    for file in "$SRC"/*.c; do
      clang-tidy --quiet -checks="$CHECKS" "$file" -- -x c -std=gnu11 $TARGET 2>/dev/null | grep -E "$FINDINGS"
    done
    for file in "$SRC"/*.cpp; do
      clang-tidy --quiet -checks="$CHECKS" "$file" -- -x c++ -std=gnu++17 -fno-exceptions \
        -isystem "$CXX_INCLUDE" -isystem "$CXX_INCLUDE/arm-none-eabi" $TARGET 2>/dev/null | grep -E "$FINDINGS"
    done
  )"
else
  echo "== clang-tidy not installed: skipped"
fi

cd "$ROOT" || exit 1

echo "== clang-format (.clang-format)"
if command -v "${CLANG_FORMAT:-clang-format}" >/dev/null 2>&1; then
  sh tools/format.sh && echo "clean" || status=1
else
  echo "clang-format not installed: skipped"
fi

# Google API keys belong in git-ignored client/secrets.mk; only YouTube's public web key (yt.cpp) may be tracked
echo "== secrets in tracked files"
report "$(git grep -nE 'AIza[0-9A-Za-z_-]{35}' -- . | grep -v AIzaSyAO_FJ2SlqU8Q4STEHLGCilw_Y9_11qcW8)"

if command -v ruff >/dev/null 2>&1; then
  echo "== ruff"
  ruff check . && ruff format --check . || status=1
else
  echo "== ruff not installed: skipped"
fi
if command -v mypy >/dev/null 2>&1; then
  echo "== mypy"
  mypy || status=1
else
  echo "== mypy not installed: skipped"
fi
exit $status
