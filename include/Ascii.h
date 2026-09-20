#pragma once
#include <stddef.h>

// Normalização de texto para as fontes bitmap (ASCII-only) do M5 RETRO TV.
//
// As fontes (Font0/Font2/Font4) só desenham 0x20..0x7E. Nomes de arquivo, tags
// ID3 (ISO-8859-1/UTF-8/UTF-16) e títulos de vídeo com acentos ("música",
// "Não Me Deixes", "França") virariam glifos quebrados. Este módulo converte
// tudo para ASCII imprimível, mapeando acentos para equivalentes sem acento
// (á->a, é->e, ç->c, ã->a ...) e descartando o que não tem equivalente.
namespace ascii {

// Mapeia um byte Latin-1 (0x80..0xFF) para ASCII. Retorna 0 quando não há
// equivalente (byte de controle ou símbolo sem letra). Tabela estática: O(1).
inline char latin1(unsigned char c) {
  // Índice 0 = 0x80 .. 127 = 0xFF.
  static const char T[128] = {
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 0x80..0x8F (C1)
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 0x90..0x9F (C1)
      ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 'a', 0, 0, 0, 0, 0, // 0xA0 NBSP ... 0xAF
      0, 0, 0, 0, 0, 'u', 0, 0, 0, 0, 'o', 0, 0, 0, 0, 0, // 0xB0..0xBF
      'A', 'A', 'A', 'A', 'A', 'A', 'A', 'C', 'E', 'E', 'E', 'E', 'I', 'I', 'I', 'I', // 0xC0..0xCF
      'D', 'N', 'O', 'O', 'O', 'O', 'O', 0, 'O', 'U', 'U', 'U', 'U', 'Y', 0, 's', // 0xD0..0xDF
      'a', 'a', 'a', 'a', 'a', 'a', 'a', 'c', 'e', 'e', 'e', 'e', 'i', 'i', 'i', 'i', // 0xE0..0xEF
      'd', 'n', 'o', 'o', 'o', 'o', 'o', 0, 'o', 'u', 'u', 'u', 'u', 'y', 0, 'y'  // 0xF0..0xFF
  };
  return T[c - 0x80];
}

// Normaliza `src` para ASCII imprimível em `dst` (terminado em \0, `cap` bytes).
// Entende UTF-8 (sequências de 2/3 bytes cobrindo o Latin-1 Supplement) e
// também bytes Latin-1 crus (ID3v1). Retorna o comprimento escrito (sem o \0).
inline size_t normalize(char *dst, size_t cap, const char *src) {
  size_t j = 0;
  if (!dst || !src || !cap)
    return 0;
  for (size_t i = 0; src[i] && j + 1 < cap;) {
    const unsigned char c = (unsigned char)src[i];
    if (c < 0x80) {
      if (c >= 0x20 && c < 0x7F)
        dst[j++] = (char)c;
      ++i;
      continue;
    }
    // Tenta decodificar UTF-8 (2 ou 3 bytes) para o codepoint.
    unsigned int cp = 0;
    size_t n = 0;
    if (c >= 0xC2 && c <= 0xDF && ((unsigned char)src[i + 1] & 0xC0) == 0x80) {
      cp = ((c & 0x1Fu) << 6) | ((unsigned char)src[i + 1] & 0x3Fu);
      n = 2;
    } else if (c >= 0xE0 && c <= 0xEF && ((unsigned char)src[i + 1] & 0xC0) == 0x80 &&
               ((unsigned char)src[i + 2] & 0xC0) == 0x80) {
      cp = ((c & 0x0Fu) << 12) | (((unsigned char)src[i + 1] & 0x3Fu) << 6) |
           ((unsigned char)src[i + 2] & 0x3Fu);
      n = 3;
    }
    if (n) {
      i += n;
      if (cp >= 0xA0 && cp <= 0xFF) {
        const char m = latin1((unsigned char)cp);
        if (m)
          dst[j++] = m;
      } else if (cp >= 0x20 && cp < 0x7F) {
        dst[j++] = (char)cp;
      }
      // else: codepoint sem equivalente ASCII -> descarta.
    } else {
      // Byte alto solto (Latin-1 puro): usa a tabela diretamente.
      const char m = latin1(c);
      if (m)
        dst[j++] = m;
      ++i;
    }
  }
  dst[j] = 0;
  return j;
}

// Normaliza em maiúsculas (para rótulos), reaproveitando a mesma passada.
inline size_t normalizeUpper(char *dst, size_t cap, const char *src) {
  const size_t n = normalize(dst, cap, src);
  for (size_t i = 0; i < n; ++i)
    if (dst[i] >= 'a' && dst[i] <= 'z')
      dst[i] -= 'a' - 'A';
  return n;
}

} // namespace ascii
