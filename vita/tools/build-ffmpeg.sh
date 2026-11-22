#!/bin/sh
# Builds the app's own FFmpeg into vita/deps/ffmpeg: VitaSDK's package (the same version, 9.0.2, and
# the same options, from vitasdk/packages ffmpeg/VITABUILD) with the MP4/MOV reader added. That
# package asks for "mp4,m4a" demuxers, names FFmpeg doesn't have (its reader is "mov"), so MP4s
# couldn't be read; with it, MP4 videos play through FFmpeg and the Vita's own H.264 hardware
# (sceAvcdec) as MKVs do, frame by frame on the app's clock, the way VitaMediaDeck plays them,
# instead of through the system player. Needs VITASDK. Run once; the CMake build uses it when present.
set -eu
here="$(cd "$(dirname "$0")/.." && pwd)"
prefix="$here/deps/ffmpeg"
ver=9.0.2
sha=8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e
[ -f "$prefix/lib/libavformat.a" ] && [ "$(cat "$prefix/.ver" 2>/dev/null)" = "$ver-mov1" ] && { echo "FFmpeg $ver (with mov) already built"; exit 0; }
: "${VITASDK:?VITASDK is not set}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
curl -fsSL -o "$work/ff.tar.xz" "https://ffmpeg.org/releases/ffmpeg-$ver.tar.xz"
echo "$sha  $work/ff.tar.xz" | sha256sum -c -
tar -C "$work" -xJf "$work/ff.tar.xz"
cd "$work/ffmpeg-$ver"
./configure --prefix="$prefix" \
  --enable-cross-compile --cross-prefix="$VITASDK/bin/arm-vita-eabi-" \
  --disable-shared --disable-runtime-cpudetect --disable-armv5te --disable-programs --disable-doc --disable-network \
  --enable-libmp3lame --disable-everything \
  --enable-decoder=aac,adpcm_ima_qt,alac,ape,ac3,cinepak,flac,mp3,mpeg4,opus,pcm_s16le,pcm_s24le,pcm_s32le,pcm_s8,pcm_u8,svq1,tta,vorbis,wmalossless,wavpack \
  --enable-demuxer=aac,ac3,aiff,ape,asf,avi,caf,cine,flac,matroska,mov,mp3,pcm_s16le,pcm_s24le,pcm_s32le,pcm_s8,pcm_u8,ogg,tta,wav,wv \
  --enable-muxer=aac,ac3,aiff,ape,asf,avi,caf,flac,matroska,mp3,mp4,m4a,pcm_s16le,pcm_s24le,pcm_s32le,pcm_s8,pcm_u8,ogg,opus,tta,wav,webm,wv \
  --enable-encoder=aac,alac,ape,ac3,flac,libmp3lame,mpeg4,opus,pcm_s16le,pcm_s24le,pcm_s32le,pcm_s8,pcm_u8,tta,vorbis,wmalossless,wavpack \
  --enable-parser=aac,ac3,flac,h263,h264,mpeg4video,mpegaudio,opus,vorbis \
  --enable-protocol=file --enable-static --enable-small --disable-debug \
  --arch=armv7-a --cpu=cortex-a9 --disable-armv6t2 --target-os=none \
  --extra-cflags=" -std=gnu11 -Wno-error=implicit-function-declaration -Wno-error=int-conversion -Wno-error=incompatible-pointer-types -Wl,-q -O2 -ftree-vectorize -fomit-frame-pointer -ffast-math -D_BSD_SOURCE" \
  --extra-ldflags=" -L$VITASDK/lib " --extra-libs=-lmpg123 \
  --disable-bzlib --disable-iconv --disable-lzma --disable-sdl2 --disable-securetransport --disable-xlib --enable-pthreads >/dev/null
make -j"$(nproc)" >/dev/null
make install >/dev/null
echo "$ver-mov1" > "$prefix/.ver"
echo "FFmpeg $ver with the mov reader: $prefix"
