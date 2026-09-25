#include "Id3.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

// Mesma técnica de stubs in-memory de tests/test_core.cpp: um stream mínimo que
// expõe read/seek/position/size, compatível com a API templatada de id3::*.
struct MemoryStream {
  std::vector<uint8_t> bytes;
  size_t offset = 0;
  size_t read(uint8_t *dst, size_t n) {
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
  uint32_t position() const { return uint32_t(offset); }
  uint32_t size() const { return uint32_t(bytes.size()); }
};

static void u32be(std::vector<uint8_t> &b, uint32_t v) {
  b.push_back((v >> 24) & 0xFF);
  b.push_back((v >> 16) & 0xFF);
  b.push_back((v >> 8) & 0xFF);
  b.push_back(v & 0xFF);
}
static void u32syncsafe(std::vector<uint8_t> &b, uint32_t v) {
  b.push_back((v >> 21) & 0x7F);
  b.push_back((v >> 14) & 0x7F);
  b.push_back((v >> 7) & 0x7F);
  b.push_back(v & 0x7F);
}
static void append(std::vector<uint8_t> &dst, const std::vector<uint8_t> &src) {
  dst.insert(dst.end(), src.begin(), src.end());
}
static void magicId3(std::vector<uint8_t> &b) {
  b.push_back('I');
  b.push_back('D');
  b.push_back('3');
}

static std::vector<uint8_t> makeFrame(const char id[4], const std::vector<uint8_t> &payload,
                                      bool syncsafeSize) {
  std::vector<uint8_t> f;
  f.insert(f.end(), id, id + 4);
  if (syncsafeSize)
    u32syncsafe(f, uint32_t(payload.size()));
  else
    u32be(f, uint32_t(payload.size()));
  f.push_back(0);
  f.push_back(0); // flags
  append(f, payload);
  return f;
}
static std::vector<uint8_t> textPayload(uint8_t enc, const std::string &s) {
  std::vector<uint8_t> p;
  p.push_back(enc);
  p.insert(p.end(), s.begin(), s.end());
  return p;
}
static std::vector<uint8_t> apicPayload(uint8_t picType, const std::string &mime,
                                        const std::vector<uint8_t> &img) {
  std::vector<uint8_t> p;
  p.push_back(0); // encoding
  p.insert(p.end(), mime.begin(), mime.end());
  p.push_back(0); // mime terminator
  p.push_back(picType);
  p.push_back(0); // empty description terminator
  p.insert(p.end(), img.begin(), img.end());
  return p;
}
// Escreve `s` em `b[off..off+len)` completando com espaços (como os rippers
// clássicos de ID3v1 fazem), para exercitar o trim de espaços à direita.
static void putField(std::vector<uint8_t> &b, size_t off, size_t len, const char *s) {
  size_t i = 0;
  for (; i < len && s[i]; ++i)
    b[off + i] = uint8_t(s[i]);
  for (; i < len; ++i)
    b[off + i] = ' ';
}

static void testId3v1Full() {
  std::vector<uint8_t> b(128, 0);
  b[0] = 'T';
  b[1] = 'A';
  b[2] = 'G';
  putField(b, 3, 30, "My Title");
  putField(b, 33, 30, "An Artist");
  putField(b, 63, 30, "The Album");
  putField(b, 93, 4, "1999");
  putField(b, 97, 30, "a comment");
  MemoryStream f;
  f.bytes = b;
  id3::TrackMeta m;
  assert(id3::readTags(f, m));
  assert(!strcmp(m.title, "My Title"));
  assert(!strcmp(m.artist, "An Artist"));
  assert(!strcmp(m.album, "The Album"));
  assert(!strcmp(m.year, "1999"));
  assert(m.track[0] == 0); // v1 puro: sem faixa
  assert(!m.hasCover);
}

static void testId3v1Track() {
  std::vector<uint8_t> b(128, 0);
  b[0] = 'T';
  b[1] = 'A';
  b[2] = 'G';
  putField(b, 3, 30, "Track Title");
  putField(b, 33, 30, "Artist");
  putField(b, 63, 30, "Album");
  putField(b, 93, 4, "2003");
  putField(b, 97, 30, "comment");
  b[125] = 0; // comment[28] == 0 => ID3v1.1
  b[126] = 7; // comment[29] == faixa
  MemoryStream f;
  f.bytes = b;
  id3::TrackMeta m;
  assert(id3::readV1(f, m));
  assert(!strcmp(m.track, "7"));
}

static void testId3v23() {
  std::vector<uint8_t> img = {0xFF, 0xD8, 1, 2, 3, 4, 0xFF, 0xD9};
  std::vector<uint8_t> frames;
  append(frames, makeFrame("TIT2", textPayload(0, "Cool Song"), false));
  append(frames, makeFrame("TPE1", textPayload(0, "Artist Name"), false));
  append(frames, makeFrame("TALB", textPayload(0, "Great Album"), false));
  append(frames, makeFrame("APIC", apicPayload(3, "image/jpeg", img), false));

  std::vector<uint8_t> file;
  magicId3(file);
  file.push_back(3); // major
  file.push_back(0); // revision
  file.push_back(0); // flags
  u32syncsafe(file, uint32_t(frames.size()));
  append(file, frames);

  MemoryStream f;
  f.bytes = file;
  f.offset = 7; // posição arbitrária que deve ser restaurada
  id3::TrackMeta m;
  assert(id3::readTags(f, m));
  assert(!strcmp(m.title, "Cool Song"));
  assert(!strcmp(m.artist, "Artist Name"));
  assert(!strcmp(m.album, "Great Album"));
  assert(m.hasCover);
  assert(m.coverSize == img.size());
  assert(!strcmp(m.coverMime, "image/jpeg"));
  assert(f.position() == 7); // posição restaurada
  // A capa aponta para os bytes reais da imagem dentro do arquivo.
  uint8_t soi[2];
  assert(f.seek(m.coverOffset) && f.read(soi, 2) == 2);
  assert(soi[0] == 0xFF && soi[1] == 0xD8);
}

static void testId3v24() {
  std::vector<uint8_t> img = {0xFF, 0xD8, 9, 9, 0xFF, 0xD9};
  std::vector<uint8_t> frames;
  append(frames, makeFrame("TIT2", textPayload(3, "Utf8 Title"), true));
  append(frames, makeFrame("TPE1", textPayload(3, "Utf8 Artist"), true));
  append(frames, makeFrame("TALB", textPayload(3, "Utf8 Album"), true));
  append(frames, makeFrame("TDRC", textPayload(0, "2001-05-01"), true));
  append(frames, makeFrame("TRCK", textPayload(0, "7/12"), true));
  append(frames, makeFrame("APIC", apicPayload(3, "image/jpeg", img), true));

  std::vector<uint8_t> file;
  magicId3(file);
  file.push_back(4); // major v2.4
  file.push_back(0);
  file.push_back(0);
  u32syncsafe(file, uint32_t(frames.size()));
  append(file, frames);

  MemoryStream f;
  f.bytes = file;
  id3::TrackMeta m;
  assert(id3::readV2(f, m));
  assert(!strcmp(m.title, "Utf8 Title"));
  assert(!strcmp(m.artist, "Utf8 Artist"));
  assert(!strcmp(m.album, "Utf8 Album"));
  assert(!strcmp(m.year, "2001")); // TDRC "2001-05-01" -> "2001"
  assert(!strcmp(m.track, "7/12"));
  assert(m.hasCover && m.coverSize == img.size());
}

static void testNormalization() {
  // Latin-1 acentuado -> ASCII sem acento; UTF-8 multibyte -> ASCII.
  {
    std::vector<uint8_t> frames;
    std::string latin1 = "Caf";
    latin1.push_back(char(0xE9)); // 'é' em Latin-1
    append(frames, makeFrame("TIT2", textPayload(0, latin1), false));
    std::vector<uint8_t> file;
    magicId3(file);
    file.push_back(3);
    file.push_back(0);
    file.push_back(0);
    u32syncsafe(file, uint32_t(frames.size()));
    append(file, frames);
    MemoryStream f;
    f.bytes = file;
    id3::TrackMeta m;
    assert(id3::readV2(f, m));
    assert(!strcmp(m.title, "Cafe"));
  }
  {
    std::vector<uint8_t> frames;
    std::string utf8 = "Caf";
    utf8.push_back(char(0xC3));
    utf8.push_back(char(0xA9)); // 'é' em UTF-8
    append(frames, makeFrame("TIT2", textPayload(3, utf8), false));
    std::vector<uint8_t> file;
    magicId3(file);
    file.push_back(3);
    file.push_back(0);
    file.push_back(0);
    u32syncsafe(file, uint32_t(frames.size()));
    append(file, frames);
    MemoryStream f;
    f.bytes = file;
    id3::TrackMeta m;
    assert(id3::readV2(f, m));
    assert(!strcmp(m.title, "Cafe"));
  }
  // UTF-16LE com BOM -> ASCII.
  {
    std::vector<uint8_t> payload;
    payload.push_back(1); // enc = UTF-16 com BOM
    payload.push_back(0xFF);
    payload.push_back(0xFE); // BOM LE
    payload.push_back('H');
    payload.push_back(0);
    payload.push_back('i');
    payload.push_back(0);
    std::vector<uint8_t> frames;
    append(frames, makeFrame("TIT2", payload, false));
    std::vector<uint8_t> file;
    magicId3(file);
    file.push_back(3);
    file.push_back(0);
    file.push_back(0);
    u32syncsafe(file, uint32_t(frames.size()));
    append(file, frames);
    MemoryStream f;
    f.bytes = file;
    id3::TrackMeta m;
    assert(id3::readV2(f, m));
    assert(!strcmp(m.title, "Hi"));
  }
}

static void testFallbackV1() {
  // ID3v2 com apenas título; artista/álbum vêm do ID3v1 no fim do arquivo.
  std::vector<uint8_t> frames;
  append(frames, makeFrame("TIT2", textPayload(0, "V2 Only Title"), false));
  std::vector<uint8_t> file;
  magicId3(file);
  file.push_back(3);
  file.push_back(0);
  file.push_back(0);
  u32syncsafe(file, uint32_t(frames.size()));
  append(file, frames);

  std::vector<uint8_t> tag(128, 0);
  tag[0] = 'T';
  tag[1] = 'A';
  tag[2] = 'G';
  putField(tag, 3, 30, "V1 Title");
  putField(tag, 33, 30, "V1 Artist");
  putField(tag, 63, 30, "V1 Album");
  putField(tag, 93, 4, "1988");
  append(file, tag);

  MemoryStream f;
  f.bytes = file;
  id3::TrackMeta m;
  assert(id3::readTags(f, m));
  assert(!strcmp(m.title, "V2 Only Title")); // v2 tem precedência
  assert(!strcmp(m.artist, "V1 Artist"));    // preenchido pelo v1
  assert(!strcmp(m.album, "V1 Album"));
  assert(!strcmp(m.year, "1988"));
}

static void testNoTag() {
  MemoryStream f;
  f.bytes.assign(256, 0xAB); // nem ID3v2 no início, nem "TAG" no fim
  id3::TrackMeta m;
  assert(!id3::readTags(f, m));
  assert(m.title[0] == 0 && m.artist[0] == 0 && m.album[0] == 0 && !m.hasCover);
}

static void testAudioStart() {
  // Sem tag: offset 0.
  {
    MemoryStream f;
    f.bytes.assign(64, 0xAB);
    assert(id3::audioStart(f) == 0);
  }
  // ID3v2.3: áudio começa logo após o cabeçalho + frames (10 + tamanho).
  {
    std::vector<uint8_t> frames;
    append(frames, makeFrame("TIT2", textPayload(0, "x"), false));
    std::vector<uint8_t> file;
    magicId3(file);
    file.push_back(3);
    file.push_back(0);
    file.push_back(0); // flags (sem footer)
    u32syncsafe(file, uint32_t(frames.size()));
    append(file, frames);
    file.push_back(0xFF); // "áudio" fictício (sync word de MP3)
    file.push_back(0xFB);
    MemoryStream f;
    f.bytes = file;
    assert(id3::audioStart(f) == 10 + frames.size());
  }
  // ID3v2.4 com footer (flag 0x10): +10 bytes do footer.
  {
    std::vector<uint8_t> frames;
    append(frames, makeFrame("TIT2", textPayload(0, "x"), true));
    std::vector<uint8_t> file;
    magicId3(file);
    file.push_back(4);
    file.push_back(0);
    file.push_back(0x10); // footer presente
    u32syncsafe(file, uint32_t(frames.size()));
    append(file, frames);
    for (int i = 0; i < 10; ++i)
      file.push_back(0); // footer
    file.push_back(0xFF);
    file.push_back(0xFB);
    MemoryStream f;
    f.bytes = file;
    assert(id3::audioStart(f) == 10 + frames.size() + 10);
  }
}

static void testMalformed() {
  // Cabeçalho ID3v2 válido, mas tamanho syncsafe maior que o arquivo.
  std::vector<uint8_t> file;
  magicId3(file);
  file.push_back(3);
  file.push_back(0);
  file.push_back(0);
  u32syncsafe(file, 0x0FFFFFFF);
  MemoryStream f;
  f.bytes = file;
  id3::TrackMeta m;
  assert(!id3::readTags(f, m));
}

int main() {
  testId3v1Full();
  testId3v1Track();
  testId3v23();
  testId3v24();
  testNormalization();
  testFallbackV1();
  testNoTag();
  testAudioStart();
  testMalformed();
  std::cout << "PASS: ID3v1, ID3v1.1, ID3v2.3 (texto+APIC), ID3v2.4, normalização, fallback, sem tag, audioStart\n";
  return 0;
}
