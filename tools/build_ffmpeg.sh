#!/bin/sh
# Builds the minimal FFmpeg (submodule client/external/ffmpeg, tag n7.1.1) into client/lib/ffmpeg:
# MP4/MKV/MPEG-TS demuxers and the audio decoders; the 3DS hardware decodes the video.
#   tools/build_ffmpeg.sh [work_dir]     (needs devkitARM; run by client/Makefile)
# No H.264 parser: the MP4/MKV demuxers would run it over H.264 headers and change playback.
# -Wno-error=incompatible-pointer-types: int32_t is `long` on devkitARM (same size as int).
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
SOURCE="$HERE/client/external/ffmpeg"
WORK=${1:-/tmp/tube3d-ffmpeg}
PREFIX="$HERE/client/lib/ffmpeg"
: "${DEVKITARM:=/opt/devkitpro/devkitARM}"
export PATH="$DEVKITARM/bin:$PATH"
[ -f "$SOURCE/configure" ] || { echo "FFmpeg's source is missing: run git submodule update --init"; exit 1; }

# Build from a copy, so patching leaves the submodule untouched
mkdir -p "$WORK"
cd "$WORK"
rm -rf src
mkdir src
git -C "$SOURCE" archive HEAD | tar -x -C src
cd src
cp RELEASE VERSION

# Drop libavutil/tx's static FFT tables above 8192 points (~17 MB of .bss; AAC needs 1024)
BIG_FFT_SIZES='16384\|32768\|65536\|131072\|262144\|524288\|1048576\|2097152'
sed -i "/SR_TABLE(\($BIG_FFT_SIZES\))/d; /DECL_SR_CODELET(\($BIG_FFT_SIZES\),/d; /ff_tx_fft\($BIG_FFT_SIZES\)_ns_def/d" libavutil/tx_template.c
sed -i 's/        int idx = factor_2 - 3;/        int idx = FFMIN(factor_2 - 3, (int)FF_ARRAY_ELEMS(sr_tabs_init_funcs) - 1);/' libavutil/tx_template.c
grep -q "FF_ARRAY_ELEMS(sr_tabs_init_funcs) - 1" libavutil/tx_template.c || { echo "tx patch did not apply"; exit 1; }

./configure \
	--prefix=/usr/local \
	--enable-cross-compile --cross-prefix=arm-none-eabi- --arch=arm --cpu=armv6k --target-os=linux \
	--extra-cflags="-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -D__3DS__ -ffunction-sections -fdata-sections -Wno-error=incompatible-pointer-types" \
	--extra-ldflags="-mfloat-abi=hard" \
	--disable-everything --disable-programs --disable-doc --disable-debug \
	--disable-avdevice --disable-avfilter --disable-swscale --disable-postproc \
	--disable-network --disable-pthreads --disable-w32threads --disable-os2threads \
	--disable-asm --disable-runtime-cpudetect \
	--disable-zlib --disable-bzlib --disable-lzma --disable-iconv \
	--enable-small \
	--enable-demuxer=mov,matroska,mpegts \
	--enable-parser=aac,ac3,dca,mpegaudio,opus,vorbis,flac \
	--enable-decoder=aac,ac3,eac3,dca,mp3,opus,vorbis,flac

make -j"$(nproc)"
# Staged install, so the library's stored configure line names /usr/local, not build folders
rm -rf "$PREFIX" "$WORK/stage"
make install DESTDIR="$WORK/stage"
mkdir -p "$PREFIX"
cp -r "$WORK/stage/usr/local/include" "$WORK/stage/usr/local/lib" "$PREFIX/"
rm -rf "$PREFIX/lib/pkgconfig"
echo "installed into $PREFIX"
ls -la "$PREFIX/lib"
