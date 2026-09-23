#pragma once
// ============================================================================
//  PhotoShow — slideshow de fotos (.jpg/.jpeg) do cartão na saída composta.
//
//  Mostra uma foto por vez, centralizada no quadro de 320x240, avançando
//  sozinha num prazo configurável (3/5/10/30 s) e também pelos três botões.
//  A pasta é /M5RETRO/fotos.
//
//  ---------------------------------------------------------------------------
//  Regras deste aparelho que moldaram a API (ver AGENTS.md)
//  ---------------------------------------------------------------------------
//
//  1. A SRAM é o recurso escasso (o framebuffer CVBS já leva 150 KB e o heap
//     livre medido fica entre 28 e 38 KB). Por isso este header **não aloca
//     nada**, seguindo o mesmo contrato do ScreenFx.h:
//
//       * o buffer de decodificação é o `jpegBuffer` que o main.cpp já mantém
//         na PSRAM para o vídeo (128 KiB) — entra por `attachBuffer()`;
//       * a instância do JPEGDEC também é a do main.cpp (a JPEGIMAGE tem
//         dezenas de KB; duas não caberiam) — entra por `attachDecoder()`;
//       * o catálogo de nomes mora num "pool" fornecido pelo chamador
//         (`attachStorage()`), que pode ficar na PSRAM. Guardamos só o nome
//         curto de cada arquivo, nunca o caminho completo: o caminho é montado
//         num buffer de pilha, um de cada vez, na hora de abrir.
//
//     Nada de LGFX_Sprite aqui: um sprite de 5 KB já causou falta de memória
//     neste projeto, e um de tela cheia (76.800 B em RGB332) não cabe.
//
//  2. Transições são as do include/ScreenFx.h — este arquivo não tem dither
//     próprio. **Achado importante**: `crossfade()` e `fadeIn()` exigem um
//     LGFX_Sprite 320x240 RGB332 do chamador e, recebendo `nullptr`, caem em
//     `hardCut()` — corte seco, sem nenhuma animação, devolvendo false. Já o
//     `fadeThrough()` com `nullptr` continua **visível**: ele esmaece até o
//     preto com o dither de Bayer (que não precisa de buffer nenhum) e só o
//     segundo tempo, o reacender, vira corte seco. Como aqui não há SRAM para
//     o sprite, `beginTransition()` usa `fadeThrough`. É de propósito.
//
//  3. `millis()` dá a volta em ~49 dias. O prazo usa `timeReached()` da
//     UiLogic.h (aritmética com sinal sobre a diferença), nunca `now > prazo`.
//
//  4. O cartão é disputado com a tarefa de áudio e é protegido pelo `sdMutex`
//     do main.cpp. **Este header nunca toma o mutex.** As duas únicas funções
//     que tocam o cartão — `scan()` e `load()` — estão marcadas com
//     "PRÉ-CONDIÇÃO: o chamador segura o sdMutex" e são curtas de propósito,
//     para o mutex ser devolvido logo (segurar mata o áudio de fome). O
//     desenho, que é a parte demorada, **não toca no cartão**: ele
//     redecodifica os bytes que já estão no `jpegBuffer`, exatamente como o
//     `redrawCurrentFrame()` do player faz.
//
//  5. Texto em ASCII maiúsculo (`ascii::normalizeUpper`), porque as fontes
//     bitmap só têm ASCII e um nome acentuado viraria buraco duplo. O nome
//     guardado no catálogo é o **cru**, do jeito que veio do cartão — é ele
//     que abre o arquivo; a normalização acontece só na hora de desenhar.
//
//  6. Sem ciano saturado (0x07FF) na saída composta: dot crawl. O acento é o
//     mesmo 0x96BC do resto do firmware.
//
//  7. Texto e faixas dentro de crt::SAFE_* (SafeArea.h). A foto em si pode
//     usar o quadro inteiro — só ela sangra até a borda do raster.
//
//  8. O JPEGDEC deste projeto não decodifica tudo o que se chama JPEG. Um
//     arquivo maior que o jpegBuffer (MAX_JPEG = 128 KiB: uma foto de câmera
//     de 4000x3168 tem ~560 KB) é recusado pelo TAMANHO, antes de ler; um
//     progressivo e um 4:4:4 são recusados pelo CABEÇALHO, antes de decodificar.
//     Ver o bloco "O que ESTA versão do JPEGDEC realmente decodifica", abaixo.
//     Em todos esses casos a tela mostra o motivo e a apresentação segue no
//     próximo prazo — nada de travar nem de deixar meia foto na tela.
//
//  ---------------------------------------------------------------------------
//  Uso (integração no main.cpp)
//  ---------------------------------------------------------------------------
//
//    photo::Show photos;
//    // storage na PSRAM, uma vez no setup():
//    photos.attachStorage((char *)ps_malloc(photo::POOL_SUGGESTED), photo::POOL_SUGGESTED,
//                         (uint16_t *)ps_malloc(photo::MAX_ENTRIES * 2), photo::MAX_ENTRIES);
//    photos.attachBuffer(jpegBuffer, MAX_JPEG);
//    photos.attachDecoder(&jpeg);
//
//    // ao entrar na tela (zera todo o estado — armadilha 6 do AGENTS.md):
//    photos.reset(millis());
//    take(sdMutex); photos.scan(SD); give(sdMutex);
//    take(sdMutex); photos.load(SD, millis()); give(sdMutex);
//    photos.paint(&rca, 0, 0);   // primeiro quadro, sem transição
//
//    // no loop():
//    if (tx.busy())            tx.tick(millis());
//    else if (photos.dueForNext(millis())) {
//      photos.advance(1, millis());
//      take(sdMutex); photos.load(SD, millis()); give(sdMutex);
//      photos.beginTransition(tx, millis(), nullptr);
//    }
//
//  Compila em -std=gnu++11. Depende de M5GFX (pelo ScreenFx.h), SafeArea.h,
//  Ascii.h e UiLogic.h. SD e JPEGDEC são detectados por __has_include: sem
//  eles (simulador de desktop) o arquivo continua compilando, só sem as duas
//  funções de cartão e sem o decode — o que permite testar a lógica pura em
//  sim/probes/photo_show.cpp.
// ============================================================================

#include "Ascii.h"
#include "SafeArea.h"
#include "ScreenFx.h"
#include "UiLogic.h" // timeReached()

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if !defined(PHOTOSHOW_HAS_JPEGDEC) && defined(__has_include)
#if __has_include(<JPEGDEC.h>)
#define PHOTOSHOW_HAS_JPEGDEC 1
#endif
#endif
#if !defined(PHOTOSHOW_HAS_SD) && defined(__has_include)
#if __has_include(<FS.h>)
#define PHOTOSHOW_HAS_SD 1
#endif
#endif

#if PHOTOSHOW_HAS_JPEGDEC
#include <JPEGDEC.h>
#endif
#if PHOTOSHOW_HAS_SD
#include <FS.h>
#endif

namespace photo {

// ---------------------------------------------------------------------------
//  Limites e aparência
// ---------------------------------------------------------------------------

// Teto de fotos catalogadas. 256 já é mais do que alguém navega com três
// botões, e o custo é só o vetor de deslocamentos (2 B por entrada).
const int MAX_ENTRIES = 256;
// Maior nome de arquivo aceito. Acima disso a foto é ignorada: sem o nome
// inteiro não dá para reabrir o arquivo depois.
const int NAME_LIMIT = 63;
// Pool sugerido para os nomes. Com nome médio de ~16 caracteres cabem ~240
// fotos; quando acaba, a varredura para e `dropped()` conta o que ficou fora.
const int POOL_SUGGESTED = 4096;

constexpr const char *kDefaultDir = "/M5RETRO/fotos";
constexpr const char *kTitle = "FOTOS";
// Legendas dos três botões. O pintor desenha esta faixa ele mesmo: as
// transições repintam a tela inteira, e algo desenhado só uma vez pelo
// chamador sumiria no primeiro esmaecimento (armadilha 7 do AGENTS.md).
constexpr const char *kBtnPrev = "ANTERIOR";
constexpr const char *kBtnDwell = "TEMPO";
constexpr const char *kBtnNext = "PROXIMA";

// Mesmo acento seguro do resto do firmware. Ciano puro (0x07FF) faz dot crawl
// na composta; este azul claro tem a mesma luminância e croma baixo.
const uint16_t ACCENT = 0x96BC;

// Faixa da legenda, logo acima da barra de botões, dentro da área segura.
const int CAPTION_H = 16;
const int CAPTION_Y = crt::BAR_Y - CAPTION_H - 4; // 182 .. 198

// Prazos oferecidos pelo botão do meio, em milissegundos.
// C++11: corpo de constexpr é UM return só — nada de laço ou variável local.
constexpr uint32_t dwellMsAt(int slot) {
  return slot <= 0 ? 3000u : slot == 1 ? 5000u : slot == 2 ? 10000u : 30000u;
}
const int DWELL_SLOTS = 4;

// ---------------------------------------------------------------------------
//  Escolha do fator de redução do JPEGDEC
// ---------------------------------------------------------------------------
//
//  O JPEGDEC reduz na própria decodificação (1/2, 1/4, 1/8), sem buffer
//  intermediário: os blocos já saem menores para o callback. É o que permite
//  uma foto de câmera de 4000x3000 virar imagem na tela sem nada de 12
//  megapixels na memória.
//
//  Os valores de JPEG_SCALE_HALF/QUARTER/EIGHTH são 2, 4 e 8, e escala 1 é
//  opção 0 — por isso `option` sai daqui sem precisar do JPEGDEC.h, e esta
//  função pode ser testada no desktop.
struct Fit {
  uint8_t scale;  // 1, 2, 4 ou 8
  uint8_t option; // 0, 2, 4 ou 8 (o que vai em jpeg.decode(0,0,option))
  int16_t w, h;   // dimensões já reduzidas
  bool fits;      // false = nem 1/8 coube; desenha recortada no centro
};

// Regra: reduzir enquanto a imagem AINDA COBRIR o quadro, e parar antes do
// passo que a deixaria menor. O excesso e recortado no centro pelo desenho.
//
// A alternativa -- reduzir ate caber inteira -- parece mais segura e e pior de
// olhar. Uma foto de 1364x1080 cabe inteira so em 1/8, que da 171x135: pouco
// mais de um quarto do quadro, cercada de tarja preta. Em 1/4 ela da 341x270,
// estoura 6% e some so com as bordas. Na TV, 6% de recorte nao se nota; 171x135
// no meio de um quadro de 320x240 se nota de longe. E o CRT ja come ~7% por
// borda em overscan de qualquer jeito, entao parte desse recorte nem chega a
// ser visivel.
//
// Nao existe 1/16 no JPEGDEC, entao uma foto de camera de 4000 px para em 1/8 e
// sai recortada (500x396 -> fits=false), que e o melhor possivel sem
// redimensionar antes. tools/prepare_photos.py evita esse caso na origem.
inline Fit fitFor(int srcW, int srcH) {
  Fit f;
  int s = 1;
  while (s < 8) {
    const int nx = s * 2;
    // Para se o proximo passo derrubar qualquer um dos lados abaixo do quadro.
    if ((srcW + nx - 1) / nx < crt::W || (srcH + nx - 1) / nx < crt::H)
      break;
    s = nx;
  }
  f.scale = (uint8_t)s;
  f.option = (uint8_t)(s == 1 ? 0 : s);
  f.w = (int16_t)((srcW + s - 1) / s);
  f.h = (int16_t)((srcH + s - 1) / s);
  f.fits = (f.w <= crt::W && f.h <= crt::H);
  return f;
}

// ---------------------------------------------------------------------------
//  O que ESTA versão do JPEGDEC realmente decodifica
// ---------------------------------------------------------------------------
//
//  Medido na biblioteca do próprio projeto
//  (.pio/libdeps/m5stack-core2/JPEGDEC), não suposto:
//
//   * PROGRESSIVO FALHA EM SILÊNCIO — e é o caso perigoso. `decode()` devolve
//     sucesso e `getLastError()` devolve 0, mas só sai 1/8 da imagem: o
//     jpeg.inl, linha 4961, diz "progressive mode - we only decode the first
//     scan (DC values)". Uma foto de 320x240 vira um borrão de 40x30. Não há
//     código de erro para pegar isso DEPOIS, então a checagem é ANTES, pelo
//     `getJPEGType()` (0xC2 = SOF2 = progressivo). Exportadores web e o "Save
//     for Web" do Photoshop geram progressivo por padrão, então isto acontece
//     com foto de gente de verdade, não é caso de laboratório.
//
//   * 4:4:4 (yuvj444p) NÃO DECODIFICA: devolve JPEG_DECODE_ERROR, e pior,
//     depois de já ter chamado o callback para alguns blocos — sobra meia
//     imagem na tela. Por isso o pintor LIMPA o quadro quando o decode falha.
//     O Y do SOF sai em `getSubSample()`: 0x11 é 4:4:4 (recusado), 0x21/0x12/
//     0x22 são os 4:2:2/4:2:0 que funcionam, e 0 é escala de cinza (1 só
//     componente), que funciona e não pode ser confundida com 4:4:4.
//
//  Formato recomendado para o cartão: 320x240, baseline, yuvj420p, -q:v 5
//  (~8 KB por foto). O resto é tratado com educação, não com travamento.
enum Support : uint8_t { SUP_OK, SUP_PROGRESSIVE, SUP_SUBSAMPLE };

// `jpegType` é o que o getJPEGType() devolve (0 = baseline, 1 = progressivo) e
// `subSample` o que o getSubSample() devolve. Fora do JPEGDEC.h de propósito,
// para poder ser testado no desktop.
inline Support supportFor(int jpegType, int subSample) {
  return jpegType == 1 ? SUP_PROGRESSIVE : (subSample == 0x11 ? SUP_SUBSAMPLE : SUP_OK);
}

// ---------------------------------------------------------------------------
//  Resultado da última tentativa de carregar uma foto
// ---------------------------------------------------------------------------
enum Result : uint8_t {
  RES_NONE,        // ainda não se tentou carregar nada
  RES_OK,          // bytes no buffer, dimensões conhecidas
  RES_EMPTY,       // a pasta não tem foto nenhuma
  RES_NO_BUFFER,   // faltou attachBuffer()/attachDecoder()
  RES_UNREADABLE,  // não abriu, ou a leitura veio curta
  RES_TOO_BIG,     // arquivo maior que o jpegBuffer (MAX_JPEG = 128 KiB)
  RES_BAD_JPEG,    // cabeçalho inválido, ou o decode falhou no meio
  RES_PROGRESSIVE, // JPEG progressivo: sairia 1/8 da imagem, sem erro nenhum
  RES_SUBSAMPLE    // 4:4:4: o decodificador desiste no meio do quadro
};

// Mensagem ASCII maiúscula correspondente, para a tela.
inline const char *message(Result r) {
  return r == RES_OK            ? ""
         : r == RES_EMPTY       ? "SEM FOTOS NO CARTAO"
         : r == RES_NO_BUFFER   ? "SEM MEMORIA PARA DECODIFICAR"
         : r == RES_UNREADABLE  ? "NAO FOI POSSIVEL LER A FOTO"
         : r == RES_TOO_BIG     ? "FOTO MUITO GRANDE - PULANDO"
         : r == RES_BAD_JPEG    ? "FOTO INVALIDA - PULANDO"
         : r == RES_PROGRESSIVE ? "JPEG PROGRESSIVO - USE BASELINE"
         : r == RES_SUBSAMPLE   ? "JPEG 4:4:4 - USE 4:2:0"
                                : "CARREGANDO";
}

// ---------------------------------------------------------------------------
//  Catálogo: nomes curtos num pool do chamador, ordenados por nome
// ---------------------------------------------------------------------------
class Catalog {
public:
  Catalog()
      : pool_(nullptr), off_(nullptr), poolCap_(0), poolUsed_(0), maxEntries_(0), count_(0),
        dropped_(0) {}

  // `pool` guarda os nomes (com o \0); `offsets` tem um uint16_t por entrada.
  // Os dois podem vir da PSRAM — este header nunca aloca.
  void attachStorage(char *pool, uint16_t poolCap, uint16_t *offsets, uint16_t maxEntries) {
    pool_ = pool;
    poolCap_ = poolCap;
    off_ = offsets;
    maxEntries_ = maxEntries > (uint16_t)MAX_ENTRIES ? (uint16_t)MAX_ENTRIES : maxEntries;
    clear();
  }
  bool attached() const { return pool_ && off_ && poolCap_ > 0 && maxEntries_ > 0; }
  void clear() {
    poolUsed_ = 0;
    count_ = 0;
    dropped_ = 0;
  }

  int count() const { return count_; }
  int dropped() const { return dropped_; } // quantas ficaram de fora do pool
  int capacity() const { return maxEntries_; }
  const char *name(int i) const { return (i >= 0 && i < count_) ? pool_ + off_[i] : ""; }

  // O iterador de diretório do ESP32 às vezes devolve o caminho inteiro
  // ("/M5RETRO/fotos/a.jpg") e às vezes só o nome. Fica só o nome.
  static const char *baseName(const char *p) {
    if (!p)
      return "";
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
  }

  // Extensão .jpg/.jpeg, sem diferenciar caixa. Ignora o que começa com '.'
  // (arquivos de recurso do macOS, "._IMG_0001.JPG", são lixo no cartão).
  static bool isJpeg(const char *name) {
    if (!name || !*name || name[0] == '.')
      return false;
    const size_t n = strlen(name);
    if (n > 4 && lowerEq(name + n - 4, ".jpg", 4))
      return true;
    return n > 5 && lowerEq(name + n - 5, ".jpeg", 5);
  }

  // Guarda o nome CRU (é ele que abre o arquivo). Devolve false quando não é
  // foto ou quando não coube. Não ordena: chame sort() no fim da varredura.
  bool add(const char *rawName) {
    if (!attached())
      return false;
    const char *n = baseName(rawName);
    if (!isJpeg(n))
      return false;
    const size_t len = strlen(n);
    if (len > (size_t)NAME_LIMIT || count_ >= maxEntries_ ||
        poolUsed_ + (uint32_t)len + 1 > (uint32_t)poolCap_) {
      ++dropped_;
      return false;
    }
    memcpy(pool_ + poolUsed_, n, len + 1);
    off_[count_++] = poolUsed_;
    poolUsed_ = (uint16_t)(poolUsed_ + len + 1);
    return true;
  }

  // Ordena por nome, sem diferenciar caixa. Inserção sobre o vetor de
  // deslocamentos: os nomes não se movem no pool, só os índices. Com 256
  // entradas o pior caso são ~32 mil comparações curtas — milissegundos, e
  // roda uma vez por visita à tela.
  void sort() {
    for (int i = 1; i < count_; ++i) {
      const uint16_t key = off_[i];
      int j = i - 1;
      while (j >= 0 && casecmp(pool_ + off_[j], pool_ + key) > 0) {
        off_[j + 1] = off_[j];
        --j;
      }
      off_[j + 1] = key;
    }
  }

  // Monta "dir/nome" num buffer do chamador. É o único lugar onde um caminho
  // completo existe, e ele vive só durante o open().
  bool path(int i, const char *dir, char *dst, size_t cap) const {
    if (!dst || cap == 0)
      return false;
    dst[0] = 0;
    const char *n = name(i);
    if (!dir || !n[0])
      return false;
    const int need = snprintf(dst, cap, "%s/%s", dir, n);
    return need > 0 && (size_t)need < cap;
  }

  static int casecmp(const char *a, const char *b) {
    while (*a && *b) {
      const int ca = up(*a), cb = up(*b);
      if (ca != cb)
        return ca - cb;
      ++a;
      ++b;
    }
    return up(*a) - up(*b);
  }

private:
  static int up(char c) { return (c >= 'a' && c <= 'z') ? c - ('a' - 'A') : (unsigned char)c; }
  static bool lowerEq(const char *s, const char *lower, size_t n) {
    for (size_t i = 0; i < n; ++i)
      if (up(s[i]) != up(lower[i]))
        return false;
    return true;
  }

  char *pool_;
  uint16_t *off_;
  uint16_t poolCap_, poolUsed_, maxEntries_;
  int count_, dropped_;
};

// ---------------------------------------------------------------------------
//  Alvo do callback do JPEGDEC
// ---------------------------------------------------------------------------
//  O JPEGDEC só aceita ponteiro de função (nada de lambda com captura), então
//  o destino do bloco vai por um contexto de arquivo — mesmo padrão do
//  `posterSprite` do main.cpp. Fica num static de função para o header
//  continuar sem .cpp (C++11 não tem variável `inline`).
struct Blit {
  lgfx::LovyanGFX *dst;
  int x0, y0;
  int32_t clipW, clipH; // tamanho do destino, para descartar bloco fora dele
};
inline Blit &blitCtx() {
  static Blit b = {nullptr, 0, 0, 0, 0};
  return b;
}

#if PHOTOSHOW_HAS_JPEGDEC
inline int blitCallback(JPEGDRAW *draw) {
  Blit &b = blitCtx();
  if (!b.dst)
    return 1;
  const int x = b.x0 + draw->x, y = b.y0 + draw->y;
  // Uma foto de 4000x3168 ainda sai 500x396 em 1/8 — o JPEGDEC não tem 1/16 —
  // então há blocos fora da tela em toda apresentação com foto de câmera.
  // Descartar o bloco inteiramente fora poupa a chamada; o parcialmente fora
  // vai para o pushImage, que recorta contra o clipRect corrente nos quatro
  // lados (LGFXBase::pushImage, ajuste de _clip_l/_clip_t + _adjust_width) e
  // corrige o ponteiro de origem. Nada é escrito fora do painel.
  if (x >= b.clipW || y >= b.clipH || x + draw->iWidth <= 0 || y + draw->iHeight <= 0)
    return 1;
  b.dst->pushImage(x, y, draw->iWidth, draw->iHeight,
                   reinterpret_cast<const lgfx::rgb565_t *>(draw->pPixels));
  return 1;
}
#endif

// ---------------------------------------------------------------------------
//  A tela
// ---------------------------------------------------------------------------
class Show {
public:
  // O que o chamador precisa fazer depois de um botão.
  enum Action : uint8_t {
    ACT_NONE,    // nada mudou
    ACT_REPAINT, // só redesenhar (mudou o prazo)
    ACT_LOAD     // trocou de foto: load() com o mutex e depois a transição
  };

  Show()
      : dir_(kDefaultDir), buf_(nullptr), bufCap_(0), index_(0), dwell_(1), deadline_(0),
        result_(RES_NONE), bytes_(0), srcW_(0), srcH_(0) {
#if PHOTOSHOW_HAS_JPEGDEC
    jpeg_ = nullptr;
#endif
    fit_ = fitFor(1, 1);
  }

  // -- ligações (uma vez, no setup) -----------------------------------------

  void attachStorage(char *pool, uint16_t poolCap, uint16_t *offsets, uint16_t maxEntries) {
    cat_.attachStorage(pool, poolCap, offsets, maxEntries);
  }
  // REUSA o jpegBuffer do vídeo (PSRAM, 128 KiB). Não alocar outro.
  void attachBuffer(uint8_t *jpegBuffer, size_t cap) {
    buf_ = jpegBuffer;
    bufCap_ = cap;
  }
#if PHOTOSHOW_HAS_JPEGDEC
  // REUSA a instância global do JPEGDEC: a JPEGIMAGE é grande demais para duas.
  void attachDecoder(JPEGDEC *dec) { jpeg_ = dec; }
#endif
  void setFolder(const char *dir) { dir_ = dir ? dir : kDefaultDir; }
  const char *folder() const { return dir_; }

  // Zera TUDO ao entrar na tela. Estado static/membro herdado da visita
  // anterior é a armadilha 6 do AGENTS.md.
  void reset(uint32_t now) {
    cat_.clear();
    index_ = 0;
    result_ = RES_NONE;
    bytes_ = 0;
    srcW_ = srcH_ = 0;
    fit_ = fitFor(1, 1);
    resetTimer(now);
  }

  // -- catálogo --------------------------------------------------------------

  Catalog &catalog() { return cat_; } // exposto para teste nativo e diagnóstico
  const Catalog &catalog() const { return cat_; }
  int count() const { return cat_.count(); }
  int index() const { return index_; }
  const char *rawName() const { return cat_.name(index_); }

  // Nome da foto corrente em ASCII maiúsculo, para desenhar.
  size_t displayName(char *dst, size_t cap) const {
    return ascii::normalizeUpper(dst, cap, cat_.name(index_));
  }

  // Fecha a varredura: ordena, prende o índice na faixa e reinicia o prazo.
  // Chamado pelo scan(); público para o probe nativo poder alimentar o
  // catálogo à mão, sem cartão.
  void afterScan(uint32_t now) {
    cat_.sort();
    if (index_ >= cat_.count())
      index_ = 0;
    result_ = cat_.count() ? RES_NONE : RES_EMPTY;
    bytes_ = 0;
    resetTimer(now);
  }

  // -- navegação e prazo -----------------------------------------------------

  int dwellSlot() const { return dwell_; }
  uint32_t dwellMs() const { return dwellMsAt(dwell_); }
  uint32_t deadline() const { return deadline_; }

  void resetTimer(uint32_t now) { deadline_ = now + dwellMs(); }

  // Prazo vencido? `timeReached` compara a DIFERENÇA com sinal, então o wrap
  // de millis() (~49 dias) não faz a foto congelar nem correr.
  bool dueForNext(uint32_t now) const { return cat_.count() > 0 && timeReached(now, deadline_); }

  // Avança `delta` fotos, dando a volta nas duas pontas. Reinicia o prazo.
  void advance(int delta, uint32_t now) {
    const int n = cat_.count();
    if (n <= 0) {
      index_ = 0;
      resetTimer(now);
      return;
    }
    int i = (index_ + delta) % n;
    if (i < 0)
      i += n; // % em C++ pode devolver negativo
    index_ = i;
    bytes_ = 0;
    result_ = RES_NONE;
    resetTimer(now);
  }

  void setIndex(int i, uint32_t now) { advance(i - index_, now); }

  void cycleDwell(uint32_t now) {
    dwell_ = (dwell_ + 1) % DWELL_SLOTS;
    resetTimer(now); // o prazo novo vale a partir de agora
  }

  // Botões: 0 = anterior, 1 = tempo, 2 = próxima.
  Action onButton(int button, uint32_t now) {
    if (button == 0) {
      advance(-1, now);
      return ACT_LOAD;
    }
    if (button == 2) {
      advance(1, now);
      return ACT_LOAD;
    }
    if (button == 1) {
      cycleDwell(now);
      return ACT_REPAINT;
    }
    return ACT_NONE;
  }

  // -- estado da foto --------------------------------------------------------

  Result result() const { return result_; }
  const char *statusText() const { return message(result_); }
  bool hasImage() const { return result_ == RES_OK && bytes_ > 0; }
  size_t bytes() const { return bytes_; }
  int srcWidth() const { return srcW_; }
  int srcHeight() const { return srcH_; }
  const Fit &fit() const { return fit_; }

  // -- cartão (PRÉ-CONDIÇÃO: o chamador segura o sdMutex) --------------------
#if PHOTOSHOW_HAS_SD
  // Lista a pasta e ordena. Curto de propósito: só lê nomes de diretório.
  //
  // PRÉ-CONDIÇÃO: o chamador já tomou o sdMutex e o devolve logo depois.
  int scan(fs::FS &fs, uint32_t now = 0) {
    cat_.clear();
    fs::File dir = fs.open(dir_);
    if (!dir || !dir.isDirectory()) {
      if (dir)
        dir.close();
      afterScan(now);
      return 0;
    }
    for (fs::File e = dir.openNextFile(); e; e = dir.openNextFile()) {
      if (!e.isDirectory())
        cat_.add(e.name());
      e.close();
    }
    dir.close();
    afterScan(now);
    return cat_.count();
  }

  // Lê a foto corrente inteira para o jpegBuffer e mede as dimensões. O
  // decode de verdade acontece no pintor, sem cartão — assim o mutex fica
  // preso só pelo tempo da leitura.
  //
  // PRÉ-CONDIÇÃO: o chamador já tomou o sdMutex e o devolve logo depois.
  Result load(fs::FS &fs, uint32_t now) {
    bytes_ = 0;
    srcW_ = srcH_ = 0;
    if (!cat_.count())
      return finish(RES_EMPTY, now);
    if (!buf_ || bufCap_ == 0)
      return finish(RES_NO_BUFFER, now);
    char p[160];
    if (!cat_.path(index_, dir_, p, sizeof(p)))
      return finish(RES_UNREADABLE, now);
    fs::File f = fs.open(p);
    if (!f || f.isDirectory()) {
      if (f)
        f.close();
      return finish(RES_UNREADABLE, now);
    }
    const size_t size = (size_t)f.size();
    if (size == 0) {
      f.close();
      return finish(RES_BAD_JPEG, now);
    }
    // Maior que o buffer: pula com mensagem em vez de arriscar a memória.
    if (size > bufCap_) {
      f.close();
      return finish(RES_TOO_BIG, now);
    }
    const size_t got = f.read(buf_, size);
    f.close();
    if (got != size)
      return finish(RES_UNREADABLE, now);
    bytes_ = size;
    return measure(now);
  }
#endif // PHOTOSHOW_HAS_SD

  // Mede o cabeçalho do que já está no buffer e escolhe a redução. Separado do
  // load() para o chamador poder alimentar o buffer por outro caminho.
  Result measure(uint32_t now) {
#if PHOTOSHOW_HAS_JPEGDEC
    if (!jpeg_ || !buf_ || !bytes_)
      return finish(RES_NO_BUFFER, now);
    if (!jpeg_->openRAM(buf_, (int)bytes_, blitCallback)) {
      bytes_ = 0;
      return finish(RES_BAD_JPEG, now);
    }
    srcW_ = jpeg_->getWidth();
    srcH_ = jpeg_->getHeight();
    // Formato do arquivo, ainda com o cabeçalho aberto. É a ÚNICA chance de
    // pegar o progressivo: depois do decode ele mente que deu certo.
    const Support sup = supportFor(jpeg_->getJPEGType(), jpeg_->getSubSample());
    jpeg_->close();
    if (srcW_ < 1 || srcH_ < 1) {
      bytes_ = 0;
      return finish(RES_BAD_JPEG, now);
    }
    if (sup != SUP_OK) {
      bytes_ = 0; // não chega a desenhar nada: a tela mostra o motivo
      return finish(sup == SUP_PROGRESSIVE ? RES_PROGRESSIVE : RES_SUBSAMPLE, now);
    }
    fit_ = fitFor(srcW_, srcH_);
    return finish(RES_OK, now);
#else
    (void)now;
    return finish(RES_NO_BUFFER, now);
#endif
  }

  // -- desenho ---------------------------------------------------------------

  // Pintor no formato dos outros: soma (ox, oy) a TUDO, não mexe no recorte
  // (é por ele que o ScreenFx limita o repinte) e pode ser chamado mais de uma
  // vez no mesmo quadro. Serve para o painel CVBS e para o LCD.
  void paint(lgfx::LovyanGFX *dst, int ox, int oy) {
    if (!dst)
      return;
    dst->fillScreen(TFT_BLACK); // respeita o recorte corrente
    if (!decodeInto(dst, ox, oy))
      drawMessage(dst, ox, oy);
    if (cat_.count() > 0)
      drawCaption(dst, ox, oy);
    drawLegend(dst, ox, oy);
  }

  // Ponte para o crt::fx::Paint (que leva um void* em vez do this).
  static void paintFn(lgfx::LovyanGFX *dst, int ox, int oy, void *user) {
    if (user)
      static_cast<Show *>(user)->paint(dst, ox, oy);
  }
  crt::fx::Screen screen() { return crt::fx::screen(&Show::paintFn, this); }

  // Começa a transição para a foto corrente.
  //
  // `scratch` é o LGFX_Sprite 320x240 RGB332 que o crossfade exigiria. NESTE
  // APARELHO ELE É SEMPRE nullptr: não há SRAM para 76.800 bytes e a PSRAM
  // está descartada para o caminho de vídeo. Por isso aqui se usa
  // `fadeThrough`, e não `crossfade`: sem sprite o crossfade vira corte seco
  // puro (hardCut), enquanto o fadeThrough ainda esmaece de verdade até o
  // preto com o dither de Bayer — que não precisa de buffer nenhum — e só o
  // reacender é que vira corte. Devolve o que o ScreenFx devolveu (false =
  // degradou, que é o esperado com nullptr).
  bool beginTransition(crt::fx::Transition &tx, uint32_t now, lgfx::LGFX_Sprite *scratch = nullptr,
                       uint32_t msOut = 260, uint32_t msIn = 200) {
    return tx.fadeThrough(now, screen(), scratch, msOut, msIn, TFT_BLACK);
  }

private:
  Result finish(Result r, uint32_t now) {
    result_ = r;
    // Mesmo com erro o prazo recomeça: a foto ruim mostra a mensagem e some
    // sozinha no próximo avanço, em vez de travar a apresentação.
    resetTimer(now);
    return r;
  }

  bool decodeInto(lgfx::LovyanGFX *dst, int ox, int oy) {
#if PHOTOSHOW_HAS_JPEGDEC
    if (!hasImage() || !jpeg_)
      return false;
    if (!jpeg_->openRAM(buf_, (int)bytes_, blitCallback))
      return false;
    jpeg_->setPixelType(RGB565_LITTLE_ENDIAN);
    Blit &b = blitCtx();
    b.dst = dst;
    // Centralizado. Quando nem 1/8 coube, a origem fica negativa e o
    // pushImage recorta: aparece o centro da foto, que é o menos ruim.
    b.x0 = ox + (crt::W - fit_.w) / 2;
    b.y0 = oy + (crt::H - fit_.h) / 2;
    b.clipW = dst->width();
    b.clipH = dst->height();
    const bool ok = jpeg_->decode(0, 0, (int)fit_.option) != 0;
    b.dst = nullptr;
    jpeg_->close();
    if (!ok) {
      // O decode falha DEPOIS de ter desenhado alguns blocos (foi assim que o
      // 4:4:4 deixou uma tira de 320x80 na tela). Limpar é obrigatório, senão
      // fica meia foto sob a mensagem de erro.
      dst->fillScreen(TFT_BLACK);
      bytes_ = 0;
      result_ = RES_BAD_JPEG; // a mensagem logo abaixo já sai com o motivo certo
    }
    return ok;
#else
    (void)dst;
    (void)ox;
    (void)oy;
    return false;
#endif
  }

  // Tela de recado: título + motivo, centralizados na área segura.
  void drawMessage(lgfx::LovyanGFX *dst, int ox, int oy) {
    dst->setFont(&fonts::Font2);
    dst->setTextSize(1);
    dst->setTextDatum(top_left);
    dst->setTextColor(TFT_WHITE, TFT_BLACK);
    dst->drawString(kTitle, ox + crt::SAFE_L, oy + crt::HEAD_Y);
    dst->drawFastHLine(ox + crt::SAFE_L, oy + crt::HEAD_RULE_Y, crt::SAFE_W, ACCENT);
    dst->setTextDatum(middle_center);
    dst->setTextColor(ACCENT, TFT_BLACK);
    dst->drawString(statusText(), ox + crt::W / 2, oy + crt::H / 2 - 8);
    if (cat_.count() > 0) {
      char nm[40];
      displayName(nm, sizeof(nm));
      dst->setFont(&fonts::Font0);
      dst->setTextColor(TFT_WHITE, TFT_BLACK);
      dst->drawString(nm, ox + crt::W / 2, oy + crt::H / 2 + 14);
    }
  }

  // Faixa da legenda: nome à esquerda, "n / total" e o prazo à direita.
  void drawCaption(lgfx::LovyanGFX *dst, int ox, int oy) {
    char nm[40], right[24], shown[40];
    displayName(nm, sizeof(nm));
    snprintf(right, sizeof(right), "%d / %d  %luS", index_ + 1, cat_.count(),
             (unsigned long)(dwellMs() / 1000UL));
    // Font0 tem 6 px por caractere: o lado direito come ~14 caracteres da
    // largura segura (272 px = 45), então o nome fica com 26 e reticências.
    fitText(shown, sizeof(shown), nm, 26);
    dst->fillRect(ox + crt::SAFE_L, oy + CAPTION_Y, crt::SAFE_W, CAPTION_H, TFT_BLACK);
    dst->drawFastHLine(ox + crt::SAFE_L, oy + CAPTION_Y, crt::SAFE_W, ACCENT);
    dst->setFont(&fonts::Font0);
    dst->setTextSize(1);
    dst->setTextDatum(top_left);
    dst->setTextColor(TFT_WHITE, TFT_BLACK);
    dst->drawString(shown, ox + crt::SAFE_L + 4, oy + CAPTION_Y + 4);
    dst->setTextDatum(top_right);
    dst->setTextColor(ACCENT, TFT_BLACK);
    dst->drawString(right, ox + crt::SAFE_R - 4, oy + CAPTION_Y + 4);
  }

  // Mesma faixa de botões das outras telas. Desenhada AQUI porque a transição
  // repinta a tela toda e o que o chamador desenhasse por fora sumiria.
  void drawLegend(lgfx::LovyanGFX *dst, int ox, int oy) {
    char line[64];
    snprintf(line, sizeof(line), "[ %s ]  [ %s ]  [ %s ]", kBtnPrev, kBtnDwell, kBtnNext);
    dst->fillRect(ox, oy + crt::BAR_Y, crt::W, crt::BAR_H, TFT_NAVY);
    dst->setFont(&fonts::Font0);
    dst->setTextSize(1);
    dst->setTextDatum(middle_center);
    dst->setTextColor(ACCENT, TFT_NAVY);
    dst->drawString(line, ox + crt::W / 2, oy + crt::BAR_Y + crt::BAR_H / 2);
  }

  // Corta em `maxChars` com ".." no fim, como o truncateText do main.cpp.
  static void fitText(char *dst, size_t cap, const char *src, int maxChars) {
    if (!dst || cap == 0)
      return;
    const size_t len = strlen(src);
    if ((int)len <= maxChars || maxChars < 3) {
      snprintf(dst, cap, "%s", src);
      return;
    }
    const int keep = maxChars - 2;
    size_t n = (size_t)keep;
    if (n > cap - 3)
      n = cap - 3;
    memcpy(dst, src, n);
    dst[n] = '.';
    dst[n + 1] = '.';
    dst[n + 2] = 0;
  }

  Catalog cat_;
  const char *dir_;
  uint8_t *buf_;
  size_t bufCap_;
#if PHOTOSHOW_HAS_JPEGDEC
  JPEGDEC *jpeg_;
#endif
  int index_, dwell_;
  uint32_t deadline_;
  Result result_;
  size_t bytes_;
  int srcW_, srcH_;
  Fit fit_;
};

} // namespace photo
