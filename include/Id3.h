#pragma once
// Leitura de tags ID3 (v1 e v2.3/v2.4) para o player de música do M5 RETRO TV.
//
// Funções puras, header-only, sem estado global. Assim como PlaybackIO.h, toda a
// API é templatada no tipo do stream: basta que ele exponha
//
//   size_t   read(uint8_t *dst, size_t n);  // bytes lidos (pode ser < n no EOF)
//   bool     seek(uint32_t pos);
//   uint32_t position() const;
//   uint32_t size() const;
//
// o que vale tanto para o `File` do Arduino (<SD.h>/<FS.h>) quanto para os
// stubs em memória usados em tests/. Nenhum trecho maior que algumas centenas de
// bytes é lido de uma vez; a imagem da capa (APIC) nunca é carregada em RAM —
// apenas seu offset/tamanho são registrados para decodificação sob demanda.
#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace id3 {

struct TrackMeta {
  char title[64];   // ASCII normalizado, vazio se ausente
  char artist[64];
  char album[64];
  char year[8];
  char track[8];
  bool hasCover;        // true se há capa embutida JPEG válida
  uint32_t coverOffset; // offset dos bytes da imagem APIC dentro do arquivo
  uint32_t coverSize;   // tamanho dos bytes da imagem
  char coverMime[24];   // ex.: "image/jpeg"

  TrackMeta() { reset(); }
  void reset() {
    title[0] = artist[0] = album[0] = year[0] = track[0] = coverMime[0] = 0;
    hasCover = false;
    coverOffset = coverSize = 0;
  }
};

// ---------------------------------------------------------------------------
// Helpers de byte/endianness
// ---------------------------------------------------------------------------
inline uint32_t syncsafe32(const uint8_t *b) {
  return (uint32_t(b[0] & 0x7F) << 21) | (uint32_t(b[1] & 0x7F) << 14) |
         (uint32_t(b[2] & 0x7F) << 7) | uint32_t(b[3] & 0x7F);
}
inline uint32_t be32(const uint8_t *b) {
  return (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | uint32_t(b[3]);
}
inline uint16_t be16(const uint8_t *b) {
  return uint16_t(uint16_t(b[0]) << 8 | uint16_t(b[1]));
}

// ---------------------------------------------------------------------------
// Normalização para ASCII imprimível (fontes bitmap são ASCII-only).
// ---------------------------------------------------------------------------
// Mapeia um byte Latin-1 (ou code point < 0x100) para ASCII; retorna 0 quando o
// caractere deve ser descartado. Acertos (0xC0..0xFF) viram o equivalente sem
// acento; pontuação comum do Windows-1252 (0x80..0x9F) também é mapeada.
inline char latin1ToAscii(uint8_t c) {
  if (c >= 0x20 && c < 0x7F)
    return char(c);
  switch (c) {
    case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xC5: return 'A';
    case 0xC6: return 'A'; // Æ
    case 0xC7: return 'C';
    case 0xC8: case 0xC9: case 0xCA: case 0xCB: return 'E';
    case 0xCC: case 0xCD: case 0xCE: case 0xCF: return 'I';
    case 0xD0: return 'D';
    case 0xD1: return 'N';
    case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6: case 0xD8: return 'O';
    case 0xD9: case 0xDA: case 0xDB: case 0xDC: return 'U';
    case 0xDD: return 'Y';
    case 0xDE: return 'T'; // Þ
    case 0xDF: return 's'; // ß
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5: return 'a';
    case 0xE6: return 'a'; // æ
    case 0xE7: return 'c';
    case 0xE8: case 0xE9: case 0xEA: case 0xEB: return 'e';
    case 0xEC: case 0xED: case 0xEE: case 0xEF: return 'i';
    case 0xF0: return 'd';
    case 0xF1: return 'n';
    case 0xF2: case 0xF3: case 0xF4: case 0xF5: case 0xF6: case 0xF8: return 'o';
    case 0xF9: case 0xFA: case 0xFB: case 0xFC: return 'u';
    case 0xFD: case 0xFF: return 'y';
    case 0xFE: return 't'; // þ
    // Pontuação comum do cp1252 (presente em tags mal-etiquetadas como latin-1)
    case 0x91: case 0x92: return '\'';
    case 0x93: case 0x94: return '"';
    case 0x96: case 0x97: return '-';
    case 0xA0: return ' ';
    default: return 0;
  }
}
inline char unicodeAscii(uint32_t cp) {
  return cp < 0x100 ? latin1ToAscii(uint8_t(cp)) : 0;
}

inline void appendAscii(char *dst, size_t cap, size_t &j, char c) {
  if (c && j + 1 < cap)
    dst[j++] = c;
}
inline void finishAscii(char *dst, size_t &j) {
  while (j && dst[j - 1] == ' ')
    --j;
  dst[j] = 0;
}

// Copia `len` bytes brutos (Latin-1) para ASCII normalizado. Para em 0x00,
// remove espaços à direita e garante terminação.
inline void copyAscii(char *dst, size_t cap, const uint8_t *src, size_t len) {
  size_t j = 0;
  for (size_t i = 0; i < len; ++i) {
    if (src[i] == 0)
      break;
    appendAscii(dst, cap, j, latin1ToAscii(src[i]));
  }
  finishAscii(dst, j);
}

inline void decodeUtf8(char *dst, size_t cap, const uint8_t *p, size_t n) {
  size_t j = 0;
  for (size_t i = 0; i < n;) {
    uint8_t c = p[i];
    if (c == 0)
      break;
    uint32_t cp;
    size_t len;
    if (c < 0x80) {
      cp = c;
      len = 1;
    } else if ((c & 0xE0) == 0xC0) {
      cp = c & 0x1F;
      len = 2;
    } else if ((c & 0xF0) == 0xE0) {
      cp = c & 0x0F;
      len = 3;
    } else if ((c & 0xF8) == 0xF0) {
      cp = c & 0x07;
      len = 4;
    } else {
      ++i; // byte de continuação/lead inválido
      continue;
    }
    if (i + len > n)
      break;
    bool ok = true;
    for (size_t k = 1; k < len; ++k) {
      uint8_t cc = p[i + k];
      if ((cc & 0xC0) != 0x80) {
        ok = false;
        break;
      }
      cp = (cp << 6) | (cc & 0x3F);
    }
    if (ok)
      appendAscii(dst, cap, j, unicodeAscii(cp));
    i += ok ? len : 1;
  }
  finishAscii(dst, j);
}

inline void decodeUtf16(char *dst, size_t cap, const uint8_t *p, size_t n, bool littleEndian) {
  size_t j = 0;
  for (size_t i = 0; i + 1 < n; i += 2) {
    uint16_t u = littleEndian ? uint16_t(uint16_t(p[i]) | (uint16_t(p[i + 1]) << 8))
                              : uint16_t(uint16_t(p[i]) << 8 | uint16_t(p[i + 1]));
    if (u == 0)
      break;
    appendAscii(dst, cap, j, unicodeAscii(u));
  }
  finishAscii(dst, j);
}

// Decodifica o payload de um frame de texto ID3v2 (encoding byte 0..3) para
// ASCII normalizado em `dst`.
inline void decodeText(char *dst, size_t cap, const uint8_t *payload, size_t size) {
  if (size < 1) {
    dst[0] = 0;
    return;
  }
  const uint8_t *p = payload + 1;
  const size_t n = size - 1;
  switch (payload[0]) {
    case 0: // ISO-8859-1
      copyAscii(dst, cap, p, n);
      break;
    case 1: { // UTF-16 com BOM
      bool le = false;
      size_t off = 0;
      if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) {
        le = true;
        off = 2;
      } else if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) {
        le = false;
        off = 2;
      }
      decodeUtf16(dst, cap, p + off, n - off, le);
      break;
    }
    case 2: // UTF-16BE (sem BOM)
      decodeUtf16(dst, cap, p, n, false);
      break;
    case 3: // UTF-8
      decodeUtf8(dst, cap, p, n);
      break;
    default:
      dst[0] = 0;
  }
}

// Reduz o texto decodificado de um frame de ano (TYER/TDRC) aos 4 dígitos
// iniciais — ex.: "2001-01-01" -> "2001".
inline void extractYear(char *dst, size_t cap) {
  char tmp[8];
  size_t j = 0;
  for (size_t i = 0; dst[i] && j < 4; ++i)
    if (dst[i] >= '0' && dst[i] <= '9')
      tmp[j++] = dst[i];
  tmp[j] = 0;
  size_t k = 0;
  while (k + 1 < cap && tmp[k])
    dst[k++] = tmp[k];
  dst[k] = 0;
}

inline bool isJpegMime(const char *mime) {
  char lower[24];
  size_t i = 0;
  for (; mime[i] && i + 1 < sizeof(lower); ++i) {
    char c = mime[i];
    if (c >= 'A' && c <= 'Z')
      c += 'a' - 'A';
    lower[i] = c;
  }
  lower[i] = 0;
  return strstr(lower, "jpeg") || strstr(lower, "jpg");
}

// ---------------------------------------------------------------------------
// ID3v1 — últimos 128 bytes
// ---------------------------------------------------------------------------
// Preenche apenas os campos ainda vazios de `out` (para não sobrescrever o que
// o ID3v2 já forneceu). Retorna true se um bloco "TAG" válido foi encontrado.
template <class Stream> bool readV1(Stream &f, TrackMeta &out) {
  const uint64_t sz = f.size();
  if (sz < 128)
    return false;
  uint8_t tag[128];
  if (!f.seek(uint32_t(sz - 128)))
    return false;
  if (f.read(tag, 128) != 128)
    return false;
  if (memcmp(tag, "TAG", 3))
    return false;
  if (out.title[0] == 0)
    copyAscii(out.title, sizeof(out.title), tag + 3, 30);
  if (out.artist[0] == 0)
    copyAscii(out.artist, sizeof(out.artist), tag + 33, 30);
  if (out.album[0] == 0)
    copyAscii(out.album, sizeof(out.album), tag + 63, 30);
  if (out.year[0] == 0) {
    char y[8] = {0};
    copyAscii(y, sizeof(y), tag + 93, 4);
    size_t k = 0;
    while (k + 1 < sizeof(out.year) && y[k])
      out.year[k++] = y[k];
    out.year[k] = 0;
  }
  // ID3v1.1: faixa em comment[28]==0 && comment[29]!=0
  if (out.track[0] == 0 && tag[125] == 0 && tag[126] != 0) {
    const uint8_t n = tag[126];
    char t[8];
    size_t k = 0;
    if (n >= 100)
      t[k++] = char('0' + n / 100);
    if (n >= 10)
      t[k++] = char('0' + (n / 10) % 10);
    t[k++] = char('0' + n % 10);
    t[k] = 0;
    memcpy(out.track, t, k + 1);
  }
  return true;
}

// ---------------------------------------------------------------------------
// ID3v2 — cabeçalho + frames (v2.3 e v2.4)
// ---------------------------------------------------------------------------

// Lê o payload de um frame de texto (TIT2/TPE1/...). Nunca lê mais que 256
// bytes, suficiente para os campos de até 64 chars.
template <class Stream>
void readTextField(Stream &f, uint64_t payloadPos, uint32_t size, char *dst, size_t cap) {
  uint8_t buf[256];
  const size_t want = size < sizeof(buf) ? size : sizeof(buf);
  if (want < 1 || !f.seek(uint32_t(payloadPos)) || f.read(buf, want) != want) {
    dst[0] = 0;
    return;
  }
  decodeText(dst, cap, buf, want);
}

// Analisa o payload de um frame APIC (sem carregar a imagem): localiza offset e
// tamanho dos bytes da imagem e valida que é um JPEG (mime + SOI FF D8). Retorna
// true e preenche picType/off/sz/mime apenas para capas JPEG válidas.
template <class Stream>
bool readApic(Stream &f, uint64_t payloadPos, uint32_t size, uint8_t &picType, uint32_t &off,
              uint32_t &sz, char *mime, size_t mimeCap) {
  const uint32_t cap = 512;
  uint8_t buf[cap];
  const size_t want = size < cap ? size : cap;
  if (want < 5 || !f.seek(uint32_t(payloadPos)) || f.read(buf, want) != want)
    return false;

  const uint8_t enc = buf[0];
  size_t i = 1;

  // MIME (sempre Latin-1, zero-terminado)
  const size_t mimeStart = i;
  while (i < want && buf[i] != 0)
    ++i;
  if (i >= want)
    return false;
  const size_t mimeLen = i - mimeStart;
  if (mimeLen >= mimeCap)
    return false;
  memcpy(mime, buf + mimeStart, mimeLen);
  mime[mimeLen] = 0;
  ++i; // pula o terminador

  if (i >= want)
    return false;
  picType = buf[i++];

  // Descrição (zero-terminada no encoding do frame)
  if (enc == 1 || enc == 2) { // UTF-16
    bool terminated = false;
    for (; i + 1 < want; i += 2) {
      if (buf[i] == 0 && buf[i + 1] == 0) {
        terminated = true;
        i += 2;
        break;
      }
    }
    if (!terminated)
      return false;
  } else { // ISO-8859-1 / UTF-8
    while (i < want && buf[i] != 0)
      ++i;
    if (i >= want)
      return false;
    ++i;
  }

  if (!isJpegMime(mime))
    return false;

  const uint64_t imgOff = payloadPos + i;
  const uint64_t imgSize = uint64_t(size) - i;
  if (imgSize < 2 || imgOff + imgSize > uint64_t(f.size()))
    return false;

  // Valida o marcador SOI do JPEG sem carregar a imagem.
  uint8_t soi[2];
  if (!f.seek(uint32_t(imgOff)) || f.read(soi, 2) != 2)
    return false;
  if (soi[0] != 0xFF || soi[1] != 0xD8)
    return false;

  off = uint32_t(imgOff);
  sz = uint32_t(imgSize);
  return true;
}

// Índice do campo de texto (0 título, 1 artista, 2 álbum, 3 faixa, 4 ano) ou -1.
inline int textField(const uint8_t *id) {
  if (!memcmp(id, "TIT2", 4))
    return 0;
  if (!memcmp(id, "TPE1", 4))
    return 1;
  if (!memcmp(id, "TALB", 4))
    return 2;
  if (!memcmp(id, "TRCK", 4))
    return 3;
  if (!memcmp(id, "TYER", 4) || !memcmp(id, "TDRC", 4))
    return 4;
  return -1;
}

// Varre os frames de um ID3v2. Lê o cabeçalho do início do arquivo; assume que
// `out` já foi resetado. Retorna true se um cabeçalho ID3v2.3/v2.4 válido existe.
template <class Stream> bool readV2(Stream &f, TrackMeta &out) {
  if (f.size() < 10)
    return false;
  uint8_t hdr[10];
  if (!f.seek(0) || f.read(hdr, 10) != 10)
    return false;
  if (memcmp(hdr, "ID3", 3))
    return false;
  const uint8_t major = hdr[3];
  const uint8_t flags = hdr[5];
  if (major != 3 && major != 4)
    return false;

  const uint64_t end = uint64_t(10) + syncsafe32(hdr + 6);
  if (end > uint64_t(f.size()))
    return false;
  uint64_t framesEnd = end;
  if (major == 4 && (flags & 0x10)) // footer de 10 bytes no v2.4
    framesEnd -= 10;

  uint64_t pos = 10;

  // Extended header (0x40)
  if (flags & 0x40) {
    uint8_t eh[4];
    if (pos + 4 > framesEnd || !f.seek(uint32_t(pos)) || f.read(eh, 4) != 4)
      return false;
    if (major == 4) {
      const uint32_t ehSize = syncsafe32(eh);
      if (ehSize < 4)
        return false;
      pos += ehSize;
    } else {
      pos += 4 + be32(eh); // v2.3: tamanho exclui os 4 bytes do próprio campo
    }
    if (pos > framesEnd)
      return false;
  }

  uint8_t coverType = 0xFF; // preferência: 3 (front) > 0 (other)
  while (pos + 10 <= framesEnd) {
    uint8_t fh[10];
    if (!f.seek(uint32_t(pos)) || f.read(fh, 10) != 10)
      break;
    if (fh[0] == 0) // padding
      break;

    const uint32_t fsize = (major == 4) ? syncsafe32(fh + 4) : be32(fh + 4);
    if (fsize == 0)
      break;
    const uint16_t fflags = be16(fh + 8);
    const uint64_t payloadPos = pos + 10;
    const uint64_t next = payloadPos + fsize;
    if (next > framesEnd)
      break; // frame malformado

    // Flags de compressão/encryption (v2.3 0x00C0, v2.4 0x000C) -> ignora o frame.
    const uint16_t badFlags = (major == 4) ? 0x000C : 0x00C0;
    if (!(fflags & badFlags)) {
      const int fi = textField(fh);
      if (fi >= 0) {
        struct Field {
          char *dst;
          size_t cap;
        };
        const Field fields[5] = {
            {out.title, sizeof(out.title)}, {out.artist, sizeof(out.artist)},
            {out.album, sizeof(out.album)}, {out.track, sizeof(out.track)},
            {out.year, sizeof(out.year)},
        };
        readTextField(f, payloadPos, fsize, fields[fi].dst, fields[fi].cap);
        if (fi == 4)
          extractYear(fields[fi].dst, fields[fi].cap);
      } else if (!memcmp(fh, "APIC", 4)) {
        uint8_t pt;
        uint32_t off, sz;
        char mime[24];
        if (readApic(f, payloadPos, fsize, pt, off, sz, mime, sizeof(mime))) {
          if (!out.hasCover || (pt == 3 && coverType != 3)) {
            out.hasCover = true;
            out.coverOffset = off;
            out.coverSize = sz;
            memcpy(out.coverMime, mime, sizeof(out.coverMime));
            out.coverMime[sizeof(out.coverMime) - 1] = 0;
            coverType = pt;
          }
        }
      }
    }
    pos = next;
  }
  return true;
}

// ---------------------------------------------------------------------------
// API principal
// ---------------------------------------------------------------------------
// Lê tags de um arquivo já aberto. Tenta ID3v2 no início; se título/artista/
// álbum ficarem incompletos, complementa com ID3v1 nos últimos 128 bytes.
// Salva e restaura a posição do stream. Retorna true se algum tag foi lido.
template <class Stream> bool readTags(Stream &f, TrackMeta &out) {
  out.reset();
  const uint32_t saved = f.position();

  bool found = readV2(f, out);
  if (out.title[0] == 0 || out.artist[0] == 0 || out.album[0] == 0) {
    if (readV1(f, out))
      found = true;
  }

  f.seek(saved); // restaura a posição original
  return found;
}

} // namespace id3
