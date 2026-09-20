#include "PlaybackIO.h"
#include <JPEGDEC.h>
#include <cassert>
#include <fstream>
#include <iostream>
#include <vector>
#include <algorithm>
struct Input {
  std::ifstream file;
  explicit Input(const char *path) : file(path, std::ios::binary) { assert(file); }
  size_t read(uint8_t *p, size_t n) {
    file.read(reinterpret_cast<char *>(p), n);
    return file.gcount();
  }
};
size_t pixels = 0;
int receive(JPEGDRAW *draw) {
  assert(draw->x >= 0 && draw->y >= 0);
  pixels += draw->iWidth * draw->iHeight;
  return 1;
}
int main(int argc, char **argv) {
  assert(argc == 4);
  Input file(argv[1]);
  int width = atoi(argv[2]), height = atoi(argv[3]);
  playback::MjpegReader reader;
  JPEGDEC jpeg;
  std::vector<uint8_t> data(128 * 1024);
  size_t n = 0;
  int count = 0;
  playback::FrameResult result;
  while ((result = reader.next(file, data.data(), data.size(), n)) == playback::FrameResult::Ready) {
    assert(jpeg.openRAM(data.data(), n, receive));
    assert(jpeg.getWidth() == width && jpeg.getHeight() == height);
    jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
    pixels = 0;
    assert(jpeg.decode(0, 0, 0));
    jpeg.close();
    assert(pixels >= size_t(width * height));
    ++count;
  }
  assert(result == playback::FrameResult::End && count == 30);
  std::cout << "PASS: JPEGDEC decoded " << count << " real " << width << "x" << height << " frames\n";
}
