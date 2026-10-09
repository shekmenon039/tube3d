#!/bin/sh
# Checks the client's C/C++ against .clang-format, or reformats it with "fix".
#   sh tools/format.sh [fix]      (needs clang-format 21: pip install clang-format==21.1.8)
cd "$(dirname "$0")/.." || exit 1
FORMAT=${CLANG_FORMAT:-clang-format}
if [ "${1:-}" = fix ]; then
  exec $FORMAT -i client/source/*.c client/source/*.h client/source/*.cpp
fi
exec $FORMAT --dry-run --Werror client/source/*.c client/source/*.h client/source/*.cpp
