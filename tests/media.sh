#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p work
media_dir=$(mktemp -d "$PWD/work/media.XXXXXX")
trap 'rm -rf "$media_dir"' EXIT HUP INT TERM
ffmpeg -nostdin -v error -f lavfi -i testsrc2=size=640x360:rate=30 \
  -f lavfi -i sine=frequency=440:sample_rate=44100 -t 2 -c:v mpeg4 -c:a aac "$media_dir/source.mp4"
# Tamanho explícito: o padrão do prepare_video.py sobe para 320x240 no port
# (PORTING.md 3.5), e o teste abaixo confere 240x160.
python3 tools/prepare_video.py "$media_dir/source.mp4" "$media_dir/240" --size 240x160
ffmpeg -nostdin -v error -i "$media_dir/source.mp4" -an -c:v copy "$media_dir/silent.mp4"
python3 tools/prepare_video.py "$media_dir/silent.mp4" "$media_dir/320" --size 320x240
./tests/run.sh "$media_dir/240/video.mjpeg" "$media_dir/240/audio.wav"
./work/test_core "$media_dir/320/video.mjpeg" "$media_dir/320/audio.wav"
# JPEGDEC baixado pelo PlatformIO para o ambiente fruitjam; JPEGDEC_DIR aponta
# para outro clone (por exemplo, sem acesso ao registro do PlatformIO).
jpeg_dir=${JPEGDEC_DIR:-.pio/libdeps/fruitjam/JPEGDEC/src}
c++ -std=c++17 -D__LINUX__ -fsanitize=address -g -Iinclude -I"$jpeg_dir" \
  tests/test_decode.cpp "$jpeg_dir/JPEGDEC.cpp" -o work/test_decode
./work/test_decode "$media_dir/240/video.mjpeg" 240 160
./work/test_decode "$media_dir/320/video.mjpeg" 320 240
