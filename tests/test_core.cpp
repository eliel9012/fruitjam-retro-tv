#include "PlaybackIO.h"
#include "SafeStorage.h"
#include "UiLogic.h"
#include "InputManager.h"
#include "NetworkManager.h"
#include <M5Unified.h>
#include <cassert>
#include <algorithm>
#include <iostream>
#include <map>
#include <vector>
#include <fstream>
#include <iterator>
uint32_t fakeMillis = 0;
FakeM5 M5;
FakeWiFi WiFi;
struct MemoryStream {
  std::vector<uint8_t> bytes;
  size_t offset = 0, reads = 0;
  size_t read(uint8_t *dst, size_t n) {
    ++reads;
    n = std::min(n, bytes.size() - offset);
    std::copy_n(bytes.data() + offset, n, dst);
    offset += n;
    return n;
  }
  bool seek(uint32_t p) {
    if (p > bytes.size())
      return false;
    offset = p;
    return true;
  }
  uint32_t position() const { return offset; }
  uint32_t size() const { return bytes.size(); }
};
void u32(std::vector<uint8_t> &b, uint32_t v) {
  for (int i = 0; i < 4; ++i)
    b.push_back(v >> (i * 8));
}
void patch32(std::vector<uint8_t> &b, size_t p, uint32_t v) {
  for (int i = 0; i < 4; ++i)
    b[p + i] = v >> (i * 8);
}
MemoryStream wav(bool junk = false) {
  MemoryStream f;
  auto &b = f.bytes;
  b = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E'};
  if (junk) {
    b.insert(b.end(), {'J', 'U', 'N', 'K'});
    u32(b, 3);
    b.insert(b.end(), {1, 2, 3, 0});
  }
  b.insert(b.end(), {'f', 'm', 't', ' '});
  u32(b, 16);
  b.insert(b.end(), {1, 0, 2, 0});
  u32(b, 22050);
  u32(b, 88200);
  b.insert(b.end(), {4, 0, 16, 0});
  b.insert(b.end(), {'d', 'a', 't', 'a'});
  u32(b, 8);
  b.insert(b.end(), 8, 0);
  patch32(b, 4, b.size() - 8);
  return f;
}
struct FakeFS {
  std::map<std::string, std::string> files;
  std::string failRenameFrom;
  bool shortWrite = false;
  struct File {
    FakeFS *fs;
    std::string path;
    explicit operator bool() const { return true; }
    size_t print(const std::string &data) {
      size_t n = fs->shortWrite ? data.size() / 2 : data.size();
      fs->files[path] = data.substr(0, n);
      return n;
    }
    void flush() {}
    void close() {}
  };
  bool exists(const std::string &p) const { return files.count(p); }
  bool remove(const std::string &p) { return files.erase(p); }
  File open(const std::string &p, const char *) {
    files[p] = "";
    return {this, p};
  }
  bool rename(const std::string &a, const std::string &b) {
    if (a == failRenameFrom || !exists(a) || exists(b))
      return false;
    files[b] = files[a];
    files.erase(a);
    return true;
  }
};
void testMjpeg() {
  using playback::FrameResult;
  playback::MjpegReader reader;
  MemoryStream f;
  f.bytes = {0xff, 0xd8};
  f.bytes.insert(f.bytes.end(), 4093, 0x32);
  f.bytes.insert(f.bytes.end(), {0xff, 0xd9, 0xff, 0xd8, 0x10, 0xff, 0xd9});
  std::vector<uint8_t> output(5000);
  size_t n = 0;
  assert(reader.next(f, output.data(), output.size(), n) == FrameResult::Ready && n == 4097);
  assert(output[4096] == 0xd9);
  assert(reader.next(f, nullptr, output.size(), n) == FrameResult::Ready && n == 5);
  assert(reader.next(f, output.data(), output.size(), n) == FrameResult::End);
  assert(f.reads == 3); // two buffered reads plus EOF, never one read per byte
  reader.reset();
  f = {};
  f.bytes = {0xff, 0xd8, 3};
  assert(reader.next(f, output.data(), output.size(), n) == FrameResult::Truncated);
  reader.reset();
  f = {};
  f.bytes = {0xff, 0xd8, 1, 2, 0xff, 0xd9};
  assert(reader.next(f, output.data(), 4, n) == FrameResult::TooLarge);
  reader.reset();
  f = {};
  f.bytes.resize(10000, 0x23);
  assert(reader.next(f, output.data(), output.size(), n) == FrameResult::Invalid);
  assert(f.reads <= 2);
  // Repeated streams and every marker boundary in a block.
  for (int prefix = 0; prefix < 4096; ++prefix) {
    reader.reset();
    f = {};
    f.bytes.resize(prefix, 0);
    f.bytes.insert(f.bytes.end(), {0xff, 0xd8, 2, 0xff, 0xd9});
    assert(reader.next(f, output.data(), output.size(), n) == FrameResult::Ready);
  }
}
void testWav() {
  playback::WavInfo info;
  for (bool junk : {false, true}) {
    auto f = wav(junk);
    assert(playback::readWav(f, info));
    assert(info.rate == 22050 && info.end - info.start == 8 && f.position() == info.start);
  }
  auto f = wav();
  f.bytes.pop_back();
  assert(!playback::readWav(f, info));
  f = wav();
  patch32(f.bytes, 40, 0xfffffffe);
  assert(!playback::readWav(f, info));
  f = wav();
  f.bytes[22] = 1;
  assert(!playback::readWav(f, info));
  f = wav();
  f.bytes[34] = 8;
  assert(!playback::readWav(f, info));
  f = wav();
  patch32(f.bytes, 24, 44100);
  assert(!playback::readWav(f, info));
  f = wav();
  patch32(f.bytes, 40, 7);
  assert(!playback::readWav(f, info));
  f = wav();
  patch32(f.bytes, 16, 8);
  assert(!playback::readWav(f, info));
  int16_t pcm[] = {-32768, 32767, 1000, -1000};
  playback::scalePcm(pcm, 4, 50);
  assert(pcm[0] == -16384 && pcm[1] == 16383 && pcm[2] == 500 && pcm[3] == -500);
  playback::scalePcm(pcm, 4, 0);
  for (int16_t sample : pcm)
    assert(sample == 0);
}
void testStorage() {
  std::string path = "settings", value = "new configuration";
  FakeFS fs;
  fs.files[path] = "old";
  fs.shortWrite = true;
  assert(!storage::replace(fs, path, value));
  assert(fs.files[path] == "old");
  fs.shortWrite = false;
  fs.failRenameFrom = "settings.tmp";
  assert(!storage::replace(fs, path, value));
  assert(fs.files[path] == "old");
  fs.failRenameFrom = path;
  assert(!storage::replace(fs, path, value));
  assert(fs.files[path] == "old");
  fs.failRenameFrom = "";
  assert(storage::replace(fs, path, value));
  assert(fs.files[path] == value && !fs.exists(path + ".bak"));
  fs.files[path + ".bak"] = "recovered";
  fs.remove(path);
  assert(storage::recover(fs, path));
  assert(fs.files[path] == "recovered");
  fs.files[path + ".bak"] = "stale";
  assert(storage::recover(fs, path));
  assert(fs.files[path] == "recovered");
}
void testNavigation() {
  assert(isBackButton(280, 4) && isBackButton(315, 33));
  assert(!isBackButton(279, 10) && !isBackButton(300, 34));
  assert(isPlayerAudioButton(232, 160) && isPlayerAudioButton(315, 183));
  assert(!isPlayerAudioButton(231, 170) && !isPlayerAudioButton(316, 170) && !isPlayerAudioButton(250, 184));
  assert(homeTarget(0) == VIDEO_LIBRARY && homeTarget(1) == AIRCRAFT_RADAR && homeTarget(2) == SETTINGS &&
         homeTarget(3) == SYSTEM_INFO);
  assert(touchButton(12, 240) == -1 && touchButton(12, 183) == -1 && touchButton(12, 184) == 0 &&
         touchButton(160, 210) == 1 && touchButton(300, 210) == 2);
  InputManager input;
  input.begin();
  fakeMillis = 0;
  M5.BtnB.down = true;
  input.update();
  fakeMillis = 100;
  M5.BtnB.down = false;
  input.update();
  assert(input.getAction() == NavAction::SELECT);
  assert(input.getAction() == NavAction::NONE);
  M5.BtnB.down = true;
  input.update();
  fakeMillis = 900;
  input.update();
  M5.BtnB.down = false;
  input.update();
  assert(input.getAction() == NavAction::BACK);
  fakeMillis = 1000;
  M5.BtnB.down = true;
  input.update();
  fakeMillis = 2700;
  input.update();
  M5.BtnB.down = false;
  input.update();
  assert(input.getAction() == NavAction::HOME);
  input.setAutoRepeat(true);
  M5.BtnA.down = true;
  input.update();
  fakeMillis += 401;
  input.update();
  assert(input.getAction() == NavAction::LEFT);
  fakeMillis += 400;
  input.update();
  assert(input.getAction() == NavAction::LEFT);
  M5.BtnA.down = false;
  input.update();
  assert(input.getAction() == NavAction::NONE);
  input.setAutoRepeat(false);
  M5.BtnC.down = true;
  input.update();
  fakeMillis += 701;
  input.update();
  assert(input.getAction() == NavAction::NEXT);
  M5.BtnC.down = false;
  input.update();
  assert(input.getAction() == NavAction::NONE);
  assert(!timeReached(0xfffffff0, 0x10));
  assert(timeReached(0x11, 0x10));
}
void testNetwork() {
  NetworkManager manager;
  SecretsConfig config;
  config.ssid = "test";
  fakeMillis = 0;
  WiFi = {};
  manager.begin(config);
  manager.update();
  assert(WiFi.attempts == 1);
  fakeMillis = 10001;
  manager.update();
  assert(manager.state() == NetworkState::FAILED);
  fakeMillis = 15002;
  manager.update();
  assert(WiFi.attempts == 2);
  WiFi.statusValue = WL_CONNECTED;
  manager.update();
  assert(manager.connected());
  WiFi.statusValue = 0;
  manager.update();
  assert(WiFi.attempts == 3); // reconnect after connection loss
  manager.disconnect();
  fakeMillis += 100000;
  manager.update();
  assert(WiFi.attempts == 3);
  manager.reconnectNow();
  manager.update();
  assert(WiFi.attempts == 4);
  for (int i = 0; i < 300; ++i) {
    fakeMillis += 10001;
    manager.update();
    fakeMillis += 40001;
    manager.update();
  }
  assert(manager.failures() == 255);
}
int main(int argc, char **argv) {
  testMjpeg();
  testWav();
  testStorage();
  testNavigation();
  testNetwork();
  if (argc == 3) {
    MemoryStream video, audio;
    std::ifstream v(argv[1], std::ios::binary), a(argv[2], std::ios::binary);
    assert(v && a);
    video.bytes.assign(std::istreambuf_iterator<char>(v), {});
    audio.bytes.assign(std::istreambuf_iterator<char>(a), {});
    playback::WavInfo info;
    assert(playback::readWav(audio, info));
    playback::MjpegReader reader;
    std::vector<uint8_t> frame(128 * 1024);
    size_t size = 0;
    int count = 0;
    playback::FrameResult result;
    while ((result = reader.next(video, frame.data(), frame.size(), size)) == playback::FrameResult::Ready)
      ++count;
    assert(result == playback::FrameResult::End && count == 30);
    assert(info.end - info.start == 2 * 88200);
    std::cout << "Real media: " << count << " frames, 2 seconds stereo PCM, " << video.reads
              << " SD block reads\n";
  }
  std::cout << "PASS: MJPEG, WAV, PCM volume, storage recovery, navigation, input, network\n";
}
