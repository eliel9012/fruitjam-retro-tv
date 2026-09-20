#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace playback {
enum class FrameResult { Ready, End, Truncated, TooLarge, Invalid };

// Read the card in sectors, preserving unused bytes between frames. Passing
// nullptr skips a compressed frame without copying or invoking the decoder.
class MjpegReader {
  uint8_t block[4096];
  size_t cursor = 0, count = 0;

public:
  void reset() { cursor = count = 0; }
  template <class Stream> FrameResult next(Stream &stream, uint8_t *dest, size_t capacity, size_t &length) {
    length = 0;
    int previous = -1;
    bool started = false;
    size_t searched = 0;
    while (true) {
      if (cursor == count) {
        count = stream.read(block, sizeof(block));
        cursor = 0;
        if (!count)
          return started ? FrameResult::Truncated : searched ? FrameResult::Invalid : FrameResult::End;
      }
      const uint8_t value = block[cursor++];
      if (!started) {
        if (previous == 0xff && value == 0xd8) {
          if (capacity < 2)
            return FrameResult::TooLarge;
          started = true;
          length = 2;
          if (dest) {
            dest[0] = 0xff;
            dest[1] = 0xd8;
          }
        } else if (++searched > 4096)
          return FrameResult::Invalid;
      } else {
        if (length >= capacity)
          return FrameResult::TooLarge;
        if (dest)
          dest[length] = value;
        ++length;
        if (previous == 0xff && value == 0xd9)
          return FrameResult::Ready;
      }
      previous = value;
    }
  }
};

struct WavInfo {
  uint32_t rate = 0, start = 0, end = 0;
  uint16_t channels = 0, align = 0;
};
inline uint32_t le32(const uint8_t *b) {
  return uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
}
inline uint16_t le16(const uint8_t *b) {
  return uint16_t(b[0]) | (uint16_t(b[1]) << 8);
}
template <class Stream> bool readWav(Stream &file, WavInfo &info) {
  uint8_t riff[12];
  if (!file.seek(0) || file.read(riff, 12) != 12 || memcmp(riff, "RIFF", 4) || memcmp(riff + 8, "WAVE", 4))
    return false;
  const uint64_t end = uint64_t(le32(riff + 4)) + 8;
  if (end < 12 || end > file.size())
    return false;
  bool formatted = false;
  while (uint64_t(file.position()) + 8 <= end) {
    uint8_t chunk[8];
    if (file.read(chunk, 8) != 8)
      return false;
    const uint32_t size = le32(chunk + 4), start = file.position();
    const uint64_t next = uint64_t(start) + size + (size & 1U);
    if (next > end)
      return false;
    if (!memcmp(chunk, "fmt ", 4)) {
      uint8_t fmt[16];
      if (size < 16 || file.read(fmt, 16) != 16)
        return false;
      info.channels = le16(fmt + 2);
      info.rate = le32(fmt + 4);
      info.align = le16(fmt + 12);
      if (le16(fmt) != 1 || info.channels != 2 || info.rate != 22050 || info.align != 4 ||
          le16(fmt + 14) != 16 || le32(fmt + 8) != 88200)
        return false;
      formatted = true;
    } else if (!memcmp(chunk, "data", 4)) {
      if (!formatted || !size || size % info.align)
        return false;
      info.start = start;
      info.end = start + size;
      return true;
    }
    if (!file.seek(uint32_t(next)))
      return false;
  }
  return false;
}
inline void scalePcm(int16_t *samples, size_t count, int volume) {
  if (volume < 0)
    volume = 0;
  if (volume > 100)
    volume = 100;
  for (size_t i = 0; i < count; ++i)
    samples[i] = int32_t(samples[i]) * volume / 100;
}
} // namespace playback
