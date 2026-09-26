#include "PlaybackIO.h"
#include "SafeStorage.h"
#include "UiLogic.h"
#include "InputManager.h"
#include "NetworkManager.h"
#include "fj/Board.h" // o stub de tests/stubs, não o do aparelho
#include "fj/Net.h"   // idem
#include "fj/UsbHidMap.h"
#include <cassert>
#include <algorithm>
#include <iostream>
#include <map>
#include <vector>
#include <fstream>
#include <iterator>
uint32_t fakeMillis = 0;
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
  File open(const std::string &p, const char *mode) {
    // SafeStorage tem de abrir com "w" (trunca). No arduino-pico FILE_WRITE é
    // O_APPEND: o JSON novo sairia colado no velho (PORTING.md 3.7).
    assert(std::string(mode) == "w");
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
  assert(isBackButton(274, 0) && isBackButton(300, 35));
  assert(!isBackButton(273, 10) && !isBackButton(300, 40));
  assert(isPlayerAudioButton(232, 160) && isPlayerAudioButton(315, 183));
  assert(!isPlayerAudioButton(231, 170) && !isPlayerAudioButton(316, 170) && !isPlayerAudioButton(250, 184));
  // Ordem do menu inicial, em duas colunas de 6 e 6. A lista de rotulos em
  // drawHome() tem de andar junto -- la um static_assert prende o tamanho, e
  // aqui prende o destino de cada posicao.
  assert(homeTarget(0) == VIDEO_LIBRARY && homeTarget(1) == MUSIC_BROWSER &&
         homeTarget(2) == PHOTO_SHOW && homeTarget(3) == RADIO && homeTarget(4) == WEATHER &&
         homeTarget(5) == AIRCRAFT_RADAR && homeTarget(6) == TEST_PATTERN &&
         homeTarget(7) == FILE_TRANSFER && homeTarget(8) == SETTINGS &&
         homeTarget(9) == SYSTEM_INFO && homeTarget(10) == EMULATORS);
  // A ultima posicao e DESLIGAR: nao tem tela, e quem navega trata antes.
  assert(homeTarget(HOME_ITEM_COUNT - 1) == HOME && homeTarget(-1) == HOME &&
         homeTarget(HOME_ITEM_COUNT) == HOME);
  // Toque: duas colunas, 6 linhas cada agora (EMULADORES completou a coluna
  // direita). Fora da grade devolve -1.
  assert(homeHit(12, 40, 12, 40, 148, 18) == 0);       // canto do primeiro item
  assert(homeHit(12, 40 + 5 * 18, 12, 40, 148, 18) == 5);   // fim da coluna esquerda
  assert(homeHit(160, 40, 12, 40, 148, 18) == 6);      // topo da coluna direita
  assert(homeHit(160, 40 + 4 * 18, 12, 40, 148, 18) == 10); // EMULADORES
  assert(homeHit(160, 40 + 5 * 18, 12, 40, 148, 18) == 11); // DESLIGAR
  assert(homeHit(160, 40 + 6 * 18, 12, 40, 148, 18) == -1); // fora da grade (HOME_ROWS)
  assert(homeHit(12, 10, 12, 40, 148, 18) == -1);      // acima da grade
  assert(homeHit(0, 40, 12, 40, 148, 18) == -1);       // a esquerda da grade
  assert(touchButton(12, 240) == -1 && touchButton(12, 183) == -1 && touchButton(12, 184) == 0 &&
         touchButton(160, 210) == 1 && touchButton(300, 210) == 2);
  InputManager input;
  input.begin();
  fakeMillis = 0;
  board::fakeButtons[1] = true;
  input.update();
  fakeMillis = 100;
  board::fakeButtons[1] = false;
  input.update();
  assert(input.getAction() == NavAction::SELECT);
  assert(input.getAction() == NavAction::NONE);
  board::fakeButtons[1] = true;
  input.update();
  fakeMillis = 900;
  input.update();
  board::fakeButtons[1] = false;
  input.update();
  assert(input.getAction() == NavAction::BACK);
  fakeMillis = 1000;
  board::fakeButtons[1] = true;
  input.update();
  fakeMillis = 2700;
  input.update();
  board::fakeButtons[1] = false;
  input.update();
  assert(input.getAction() == NavAction::HOME);
  input.setAutoRepeat(true);
  board::fakeButtons[0] = true;
  input.update();
  fakeMillis += 401;
  input.update();
  assert(input.getAction() == NavAction::LEFT);
  fakeMillis += 400;
  input.update();
  assert(input.getAction() == NavAction::LEFT);
  board::fakeButtons[0] = false;
  input.update();
  assert(input.getAction() == NavAction::NONE);
  input.setAutoRepeat(false);
  board::fakeButtons[2] = true;
  input.update();
  fakeMillis += 701;
  input.update();
  assert(input.getAction() == NavAction::NEXT);
  board::fakeButtons[2] = false;
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
void testUsbHidMap() {
  using namespace usbhidmap;
  // Descritor de um gamepad generico: X/Y de 8 bits, hat switch de 4 bits
  // (0..7, 8=centro), 4 bits de preenchimento e 8 botoes -- 4 bytes de
  // relatorio ao todo. Bytes conferidos a mao contra a tabela de itens curtos
  // do HID 1.11 (secao 6.2.2).
  const uint8_t desc[] = {
      0x05, 0x01,       // Usage Page (Generic Desktop)
      0x09, 0x05,       // Usage (Gamepad)
      0xA1, 0x01,       // Collection (Application)
      0x09, 0x30,       //   Usage (X)
      0x09, 0x31,       //   Usage (Y)
      0x15, 0x00,       //   Logical Minimum (0)
      0x26, 0xFF, 0x00, //   Logical Maximum (255)
      0x75, 0x08,       //   Report Size (8)
      0x95, 0x02,       //   Report Count (2)
      0x81, 0x02,       //   Input (Data,Var,Abs)
      0x09, 0x39,       //   Usage (Hat switch)
      0x15, 0x00,       //   Logical Minimum (0)
      0x25, 0x07,       //   Logical Maximum (7)
      0x75, 0x04,       //   Report Size (4)
      0x95, 0x01,       //   Report Count (1)
      0x81, 0x42,       //   Input (Data,Var,Abs,Null)
      0x75, 0x04,       //   Report Size (4) -- preenchimento
      0x95, 0x01,       //   Report Count (1)
      0x81, 0x01,       //   Input (Constant)
      0x05, 0x09,       //   Usage Page (Button)
      0x19, 0x01,       //   Usage Minimum (1)
      0x29, 0x08,       //   Usage Maximum (8)
      0x15, 0x00,       //   Logical Minimum (0)
      0x25, 0x01,       //   Logical Maximum (1)
      0x75, 0x01,       //   Report Size (1)
      0x95, 0x08,       //   Report Count (8)
      0x81, 0x02,       //   Input (Data,Var,Abs)
      0xC0,             // End Collection
  };
  GamepadFieldLayout layout = parseGamepadFieldLayout(desc, sizeof(desc));
  assert(layout.valid);
  assert(layout.reportId == 0);
  assert(layout.hasAxisX && layout.axisXBitOffset == 0 && layout.axisXBitSize == 8);
  assert(layout.hasAxisY && layout.axisYBitOffset == 8 && layout.axisYBitSize == 8);
  assert(layout.hasHat && layout.hatBitOffset == 16 && layout.hatBitSize == 4);
  assert(layout.hatLogicalMin == 0 && layout.hatLogicalMax == 7);
  assert(layout.axisLogicalMin == 0 && layout.axisLogicalMax == 255);
  assert(layout.buttonBitOffset == 24 && layout.buttonCount == 8);

  // Hat para cima (dir=0), analogico centrado, botoes 0 (select) e 6 (home).
  {
    const uint8_t report[4] = {128, 128, 0x00, 0x41};
    LogicalButtons b = fromGeneric(layout, report, sizeof(report));
    assert(b.up && !b.down && !b.left && !b.right);
    assert(b.select && b.home);
    // Navegacao tem prioridade sobre os atalhos no mesmo relatorio.
    assert(primaryAction(b) == NavAction::UP);
  }
  // Hat centrado (valor 8 = null): cai para o analogico. X puxado para a
  // direita, Y no meio; botao 1 (back) sozinho.
  {
    const uint8_t report[4] = {250, 128, 0x08, 0x02};
    LogicalButtons b = fromGeneric(layout, report, sizeof(report));
    assert(!b.up && !b.down && !b.left && b.right);
    assert(b.back && !b.select);
    assert(primaryAction(b) == NavAction::RIGHT);
  }
  // So o botao de voltar, sem direcao nenhuma.
  {
    const uint8_t report[4] = {128, 128, 0x08, 0x02};
    LogicalButtons b = fromGeneric(layout, report, sizeof(report));
    assert(!b.up && !b.down && !b.left && !b.right && b.back);
    assert(primaryAction(b) == NavAction::BACK);
  }

  // Teclado boot HID: seta e Enter/Esc/Espaco/PgUp/PgDn.
  {
    const uint8_t left[8] = {0, 0, hidkey::LEFT, 0, 0, 0, 0, 0};
    assert(primaryAction(fromKeyboardBootReport(left)) == NavAction::LEFT);
    const uint8_t enter[8] = {0, 0, hidkey::ENTER, 0, 0, 0, 0, 0};
    assert(primaryAction(fromKeyboardBootReport(enter)) == NavAction::SELECT);
    const uint8_t esc[8] = {0, 0, hidkey::ESC, 0, 0, 0, 0, 0};
    assert(primaryAction(fromKeyboardBootReport(esc)) == NavAction::BACK);
    const uint8_t space[8] = {0, 0, hidkey::SPACE, 0, 0, 0, 0, 0};
    assert(primaryAction(fromKeyboardBootReport(space)) == NavAction::PLAY_PAUSE);
    const uint8_t pgup[8] = {0, 0, hidkey::PAGE_UP, 0, 0, 0, 0, 0};
    assert(primaryAction(fromKeyboardBootReport(pgup)) == NavAction::PREVIOUS);
    // Enter e Esc juntos (repique/tecla presa): BACK vence, mesma prioridade
    // fixa de primaryAction().
    const uint8_t both[8] = {0, 0, hidkey::ENTER, hidkey::ESC, 0, 0, 0, 0};
    assert(primaryAction(fromKeyboardBootReport(both)) == NavAction::BACK);
  }

  // DualShock4: hat=5 (sudoeste -> baixo e esquerda juntos), Cruz (select),
  // L1 (previous) e o botao PS (home).
  {
    const uint8_t ds4[8] = {0, 0, 0, 0, 0x25, 0x01, 0x01, 0};
    LogicalButtons b = fromDualShock4(ds4, sizeof(ds4));
    assert(b.down && b.left && !b.up && !b.right);
    assert(b.select && b.previous && b.home);
    // Diagonal: left vence na prioridade fixa sobre down/home/select.
    assert(primaryAction(b) == NavAction::LEFT);
    // DualSense usa o mesmo layout de eixos/d-pad/botoes.
    assert(primaryAction(fromDualSense(ds4, sizeof(ds4))) == NavAction::LEFT);
  }
  assert(isDualShock4(VID_SONY, PID_DUALSHOCK4_V1));
  assert(isDualShock4(VID_SONY, PID_DUALSHOCK4_V2));
  assert(isDualSense(VID_SONY, PID_DUALSENSE));
  assert(!isDualShock4(0x046D, 0xC216)); // Logitech generico -- nao e Sony
}
int main(int argc, char **argv) {
  testMjpeg();
  testWav();
  testStorage();
  testNavigation();
  testNetwork();
  testUsbHidMap();
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
  std::cout << "PASS: MJPEG, WAV, PCM volume, storage recovery, navigation, input, network, USB HID\n";
}
