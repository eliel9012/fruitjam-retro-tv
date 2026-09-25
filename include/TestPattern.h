#pragma once
// ============================================================================
// TestPattern.h — carta de ajuste SMPTE, tom de referencia de 1 kHz e as telas
// de "sem sinal" / fim de transmissao.
//
// Serve a duas coisas que nenhuma outra tela deste firmware faz:
//
//   1. CALIBRAR O TUBO. As barras SMPTE sao a unica tela do aparelho que usa o
//      quadro INTEIRO de 320x240, de proposito. Todas as outras respeitam a
//      area segura (SafeArea.h) porque um tubo esconde ~7% de cada borda; aqui
//      o objetivo e justamente MEDIR quanto ele esconde. Quem olha a TV ve onde
//      as barras somem e descobre o overscan do aparelho dele. O unico elemento
//      que continua dentro da caixa segura e o TEXTO: um rotulo que a TV corta
//      nao informa nada.
//   2. ENCERRAR A TRANSMISSAO. Barras + tom de 1 kHz e o par classico de
//      sign-off; e o "sem sinal" (chuvisco ou cartaz) e o que a tela mostra
//      quando nao ha cartao SD.
//
// ---------------------------------------------------------------------------
// O que este arquivo assume sobre o codificador NTSC (medido, nao chutado)
// ---------------------------------------------------------------------------
// Os niveis abaixo saem da tabela NTSC do M5GFX 0.2.29
// (lgfx/v1/platforms/esp32/Panel_CVBS.cpp, struct signal_spec_t):
//
//     blanking = 286 mV  -> 0 IRE
//     black    = 340 mV  -> 7,5 IRE   (setup americano, o mesmo do NTSC-M)
//     white    = 960 mV  -> 94,4 IRE  (abaixo de 100 IRE de PROPOSITO: assim o
//                                      pico de croma do amarelo cai perto de
//                                      100 IRE em vez de estourar)
//
// Consequencias que mudam o desenho:
//
//   * **0x0000 NAO e o nivel de apagamento (blanking).** O codificador ja
//     insere o pedestal de 7,5 IRE, entao o preto do framebuffer sai no nivel
//     de PRETO correto do NTSC. Isso e bom (nao precisamos compensar nada) e
//     tem um preco: **nao existe "mais preto que preto"**. A barra de -4 IRE
//     do PLUGE e impossivel de gerar a partir de pixel; ela e desenhada
//     identica ao preto de referencia e o comentario do PLUGE explica como
//     calibrar mesmo assim.
//   * A escala util e 86,8 IRE para 256 codigos -> **0,3405 IRE por codigo**.
//   * Como o branco esta em 94,4 IRE e nao em 100, uma barra de 75% medida num
//     osciloscopio da ~71,9 IRE em vez dos 76,9 IRE do papel. Nao e erro de
//     conta nossa: e o ganho escolhido pelo M5GFX, e o controle de contraste da
//     TV absorve a diferenca. As PROPORCOES entre as barras, que e o que a
//     carta mede, continuam certas.
//   * O caminho RGB565 -> RGB888 do codificador e `(v5 * 0x21) >> 2` e
//     `(v6 * 0x41) >> 4`, ou seja replicacao de bits. Cada constante de cor
//     abaixo traz, no comentario, o RGB888 que o codificador realmente vai ver.
//
// ---------------------------------------------------------------------------
// Restricoes do projeto respeitadas aqui (ver AGENTS.md)
// ---------------------------------------------------------------------------
//   * **C++11** (`-std=gnu++11`): todo `constexpr` daqui e um unico `return`,
//     sem laco e sem variavel local. Conferir com `make -C sim cxx11`.
//   * **Nada de sprite nem de framebuffer proprio.** O desenho e so `fillRect`;
//     a SRAM e o recurso escasso (heap livre medido: 28-38 KB).
//   * A tabela do tom de 1 kHz tem 882 bytes e vive num `static const` dentro
//     de funcao, ou seja **em flash (.rodata), nao em SRAM**. O estado do
//     gerador sao 4 bytes.
//   * **Este header NAO toca no I2S.** I2S0 e do video composto e o I2S1 tem
//     dono (o `audioTask` do main.cpp). Aqui so se PRODUZ PCM; quem escreve e
//     o dono do barramento.
//   * Texto ASCII, sem acento: as fontes bitmap so tem ASCII.
//
// Dependencias: LovyanGFX (fj/Gfx.h, vem pela VcrFont.h), SafeArea.h e
// Ascii.h. Nada de Arduino, FreeRTOS, SD ou I2S — o destino e um
// `lgfx::LovyanGFX*`, entao o mesmo header serve ao canvas `tv` do aparelho e
// ao painel SDL do simulador.
// ============================================================================

#include "Ascii.h"
#include "SafeArea.h"
#include "VcrFont.h" // traz fj/Gfx.h (LovyanGFX) e a fonte grossa 12x16 do cartaz

#include <stddef.h>
#include <stdint.h>

namespace testpattern {

// ===========================================================================
//  1. Cores
// ===========================================================================

// RGB888 -> RGB565 por truncamento, que e exatamente o que o M5GFX faz em
// color565(). Um unico `return`, para continuar valendo em C++11.
constexpr uint16_t rgb565(int r, int g, int b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xF8) >> 3));
}

// Niveis de referencia da carta, em codigo de 8 bits.
constexpr int L75 = 191;  // 0,75 * 255 = 191,25 -> 191
constexpr int L100 = 255; // branco de referencia

// --- as sete barras de 75% -------------------------------------------------
// Valor 565 e, entre parenteses, o RGB888 que o codificador reconstroi.
// ATENCAO ao passar qualquer uma destas para o LovyanGFX: o tipo do argumento
// e que decide como a cor e lida. Em misc/colortype.hpp:861-866, uint8_t vira
// RGB332, uint16_t/int16_t/int32_t viram RGB565 e **uint32_t vira RGB888**.
// Um (uint32_t)GRAY75 faz 0xBDF7 ser lido como 0x00BDF7, ou seja R=0, G=189,
// B=247, e a barra cinza sai esverdeada. Era esse o defeito aqui. Passe
// uint16_t, ou lgfx::rgb565_t, nunca uint32_t.
constexpr uint16_t GRAY75 = rgb565(L75, L75, L75);  // 0xBDF7  (189,190,189)
constexpr uint16_t YELLOW75 = rgb565(L75, L75, 0);  // 0xBDE0  (189,190,  0)
constexpr uint16_t CYAN75 = rgb565(0, L75, L75);    // 0x05F7  (  0,190,189)
constexpr uint16_t GREEN75 = rgb565(0, L75, 0);     // 0x05E0  (  0,190,  0)
constexpr uint16_t MAGENTA75 = rgb565(L75, 0, L75); // 0xB817  (189,  0,189)
constexpr uint16_t RED75 = rgb565(L75, 0, 0);       // 0xB800  (189,  0,  0)
constexpr uint16_t BLUE75 = rgb565(0, 0, L75);      // 0x0017  (  0,  0,189)

// --- linha de baixo --------------------------------------------------------
constexpr uint16_t WHITE100 = rgb565(L100, L100, L100); // 0xFFFF  (255,255,255)
constexpr uint16_t BLACK = 0x0000;                      // 7,5 IRE: ver cabecalho

// -I e +Q sao os eixos de croma do NTSC. Os equivalentes RGB usuais da carta
// (croma em amplitude cheia, luma baixa) sao #00214C e #32006A.
constexpr uint16_t MINUS_I = rgb565(0, 33, 76);  // 0x0109  (  0, 32, 72)
constexpr uint16_t PLUS_Q = rgb565(50, 0, 106);  // 0x300D  ( 49,  0,107)

// PLUGE (Picture Line-Up Generating Equipment) — ajuste de brilho.
//
// O papel pede tres barras: -4 / 0 / +4 IRE em torno do preto. Aqui:
//
//   PLUGE_MINUS  impossivel. O pedestal de 7,5 IRE ja esta no codificador e o
//                codigo 0 e o piso; nao da para descer abaixo do preto a partir
//                de pixel. Fica identica ao preto de referencia, DE PROPOSITO.
//   BLACK        preto de referencia, codigo 0 = 7,5 IRE.
//   PLUGE_PLUS   codigo 8 = +2,72 IRE. Em RGB565 os cinzas neutros andam de 8
//                em 8 codigos (R5/B5 tem passo 8), entao +4 IRE (codigo ~12)
//                simplesmente nao existe nesta profundidade: 8 e o degrau
//                imediatamente acima do preto.
//
// Como calibrar com o que da para gerar: abaixe o BRILHO ate as tres barras
// virarem um bloco preto so; suba ate a TERCEIRA (a da direita) aparecer por
// pouco. Nesse ponto as duas primeiras ainda estao fundidas com o fundo — que e
// o mesmo criterio do PLUGE original, so que lido da direita para a esquerda.
constexpr uint16_t PLUGE_MINUS = BLACK;               // recortado no piso
constexpr uint16_t PLUGE_PLUS = rgb565(8, 8, 8);      // 0x0841  (8,8,8)  +2,72 IRE
constexpr uint16_t PLUGE_PLUS_HI = rgb565(16, 16, 16);// 0x1082  (16,16,16) +5,45 IRE
                                                      // degrau alternativo, para
                                                      // tubo muito surrado

// ===========================================================================
//  2. Geometria da carta (SMPTE ECR 1-1978, campo dividido)
// ===========================================================================
//
//   topo   67%  ->  161 px   sete barras de 75%
//   meio    8%  ->   19 px   barras azuis invertidas (castellation)
//   base   25%  ->   60 px   -I / branco 100% / +Q / preto / PLUGE / preto
//
// 240 * 0,67 = 160,8 ; 240 * 0,08 = 19,2 ; 240 * 0,25 = 60. Arredondado para
// 161 + 19 + 60 = 240 exatos, sem linha sobrando.
constexpr int BARS_Y = 0;
constexpr int BARS_H = 161;
constexpr int MID_Y = BARS_Y + BARS_H;  // 161
constexpr int MID_H = 19;
constexpr int BOT_Y = MID_Y + MID_H;    // 180
constexpr int BOT_H = crt::H - BOT_Y;   // 60

// Borda esquerda da barra `i` (0..7). 320/7 = 45,71, entao as barras saem com
// 45 ou 46 px — a diferenca de 1 px some no borrao horizontal do NTSC, e o
// importante (somar 320 exatos, sem coluna orfa na direita) fica garantido.
//   0, 45, 91, 137, 182, 228, 274, 320
constexpr int barX(int i) { return (i * crt::W) / 7; }

static_assert(BARS_H + MID_H + BOT_H == crt::H, "as tres faixas precisam somar 240");
static_assert(barX(0) == 0 && barX(7) == crt::W, "as barras precisam cobrir 0..320");

// O rotulo cai no bloco preto da linha de baixo, entre as barras 3 e 5. Esse
// retangulo e [137,228) x [180,240), e as duas linhas de texto ficam em
// [194,214) — dentro de crt::SAFE_* nos dois eixos, que e a regra para texto.
constexpr int LABEL_X = barX(3);
constexpr int LABEL_W = barX(5) - barX(3); // 91 px
constexpr int LABEL_Y1 = BOT_Y + 14;       // 194
constexpr int LABEL_Y2 = BOT_Y + 26;       // 206

static_assert(LABEL_X >= crt::SAFE_L && LABEL_X + LABEL_W <= crt::SAFE_R,
              "rotulo fora da area segura no eixo X");
static_assert(LABEL_Y1 >= crt::SAFE_T && LABEL_Y2 + 8 <= crt::SAFE_B,
              "rotulo fora da area segura no eixo Y");

// ===========================================================================
//  3. Desenho
// ===========================================================================

namespace detail {

inline void seg(lgfx::LovyanGFX *dst, int ox, int oy, int x0, int x1, int y, int h, uint16_t color) {
  dst->fillRect(ox + x0, oy + y, x1 - x0, h, (uint16_t)color);
}

// Normaliza texto de fora para ASCII antes de desenhar (armadilha 9 do
// AGENTS.md: acento em UTF-8 sao dois bytes sem glifo, viram dois buracos).
inline const char *norm(char *dst, size_t cap, const char *src) {
  if (!cap)
    return "";
  if (!src) {
    dst[0] = 0;
    return dst;
  }
  ascii::normalizeUpper(dst, cap, src);
  return dst;
}

} // namespace detail

// Barras SMPTE no quadro inteiro, com rotulo de emissora.
//
// `l1` / `l2` podem ser nulos (carta limpa, sem texto). Passam por
// ascii::normalizeUpper, entao aceitam texto vindo de fora.
inline void drawBarsLabeled(lgfx::LovyanGFX *dst, int ox, int oy, const char *l1, const char *l2) {
  if (!dst)
    return;

  // Em flash, nao em SRAM: 14 bytes cada.
  static const uint16_t kTop[7] = {GRAY75, YELLOW75, CYAN75, GREEN75, MAGENTA75, RED75, BLUE75};
  // Faixa do meio: as barras azuis invertidas. A ordem existe para que, com o
  // croma da TV desligado (ou so o canal azul ligado), as colunas fiquem com
  // luminancia igual e as bordas sumam — e assim que se ajusta cor/matiz.
  static const uint16_t kMid[7] = {BLUE75, BLACK, MAGENTA75, BLACK, CYAN75, BLACK, GRAY75};

  dst->startWrite();

  for (int i = 0; i < 7; ++i) {
    detail::seg(dst, ox, oy, barX(i), barX(i + 1), BARS_Y, BARS_H, kTop[i]);
    detail::seg(dst, ox, oy, barX(i), barX(i + 1), MID_Y, MID_H, kMid[i]);
  }

  // Linha de baixo. Larguras em unidades de barra: -I 1, branco 1, +Q 1,
  // preto 2, PLUGE 1, preto 1 = 7.
  detail::seg(dst, ox, oy, barX(0), barX(1), BOT_Y, BOT_H, MINUS_I);
  detail::seg(dst, ox, oy, barX(1), barX(2), BOT_Y, BOT_H, WHITE100);
  detail::seg(dst, ox, oy, barX(2), barX(3), BOT_Y, BOT_H, PLUS_Q);
  detail::seg(dst, ox, oy, barX(3), barX(5), BOT_Y, BOT_H, BLACK);

  const int p0 = barX(5), p3 = barX(6);
  const int p1 = p0 + (p3 - p0) / 3, p2 = p0 + ((p3 - p0) * 2) / 3;
  detail::seg(dst, ox, oy, p0, p1, BOT_Y, BOT_H, PLUGE_MINUS);
  detail::seg(dst, ox, oy, p1, p2, BOT_Y, BOT_H, BLACK);
  detail::seg(dst, ox, oy, p2, p3, BOT_Y, BOT_H, PLUGE_PLUS);

  detail::seg(dst, ox, oy, barX(6), barX(7), BOT_Y, BOT_H, BLACK);

  if (l1 || l2) {
    // Font0 tamanho 1 (6x8) de proposito: o rotulo e assinatura, nao conteudo.
    // Grande demais ele roubaria area preta que o PLUGE precisa ter ao lado.
    char b1[16], b2[16];
    dst->setFont(&fonts::Font0);
    dst->setTextSize(1);
    dst->setTextDatum(lgfx::textdatum::top_center);
    dst->setTextColor((uint16_t)GRAY75); // fore == back no LovyanGFX = fundo transparente
    const int cx = ox + LABEL_X + LABEL_W / 2;
    if (l1)
      dst->drawString(detail::norm(b1, sizeof(b1), l1), cx, oy + LABEL_Y1);
    if (l2)
      dst->drawString(detail::norm(b2, sizeof(b2), l2), cx, oy + LABEL_Y2);
    dst->setTextDatum(lgfx::textdatum::top_left);
  }

  dst->endWrite();
}

// Ponto de entrada padrao: mesma assinatura dos outros pintores do projeto,
// entao serve ao painel CVBS e ao LCD sem adaptacao.
inline void drawBars(lgfx::LovyanGFX *dst, int ox, int oy) {
  drawBarsLabeled(dst, ox, oy, "M5 RETRO TV", "SMPTE 75% 1KHZ");
}

// ---------------------------------------------------------------------------
//  Chuvisco ("snow") — o "sem sinal" classico
// ---------------------------------------------------------------------------
//
// Por celula de 8x8, nao por pixel: 40x30 = 1200 fillRect por quadro, contra
// 76.800 escritas de um laco pixel a pixel. O ruido de um tubo sem sinal e
// grosso mesmo, entao a celula nao custa realismo.
//
// O estado e do objeto, nao um `static` escondido (armadilha 6 do AGENTS.md:
// tela que herda o estado da visita anterior). Quem entra na tela chama
// reset().
class Snow {
public:
  static constexpr int CELL = 8;

  constexpr explicit Snow(uint32_t seed = 0x2545F491u) : s_(seed | 1u) {}

  void reset(uint32_t seed = 0x2545F491u) { s_ = seed | 1u; }

  // Um quadro de chuvisco no quadro inteiro. Chame de novo para o proximo.
  void draw(lgfx::LovyanGFX *dst, int ox, int oy, int cell = CELL) {
    if (!dst || cell < 1)
      return;
    dst->startWrite();
    for (int y = 0; y < crt::H; y += cell) {
      const int h = (y + cell > crt::H) ? crt::H - y : cell;
      for (int x = 0; x < crt::W; x += cell) {
        const int w = (x + cell > crt::W) ? crt::W - x : cell;
        const int v = (int)(next() >> 24); // byte alto: o de melhor distribuicao
        dst->fillRect(ox + x, oy + y, w, h, (uint16_t)rgb565(v, v, v));
      }
    }
    dst->endWrite();
  }

private:
  // xorshift32: 3 shifts e 3 XORs por celula. Um rand() da libc aqui custaria
  // chamada de funcao e estado global compartilhado com o resto do firmware.
  uint32_t next() {
    s_ ^= s_ << 13;
    s_ ^= s_ >> 17;
    s_ ^= s_ << 5;
    return s_;
  }
  uint32_t s_;
};

// Instancia de conveniencia para quem so quer um pintor (dst, ox, oy).
// Constante-inicializada (construtor constexpr), entao nao gera guard variable
// nem inicializacao em tempo de boot.
inline Snow &sharedSnow() {
  static Snow s;
  return s;
}

inline void drawSnow(lgfx::LovyanGFX *dst, int ox, int oy) {
  sharedSnow().draw(dst, ox, oy);
}

// ---------------------------------------------------------------------------
//  Cartaz (slate) — "NO SIGNAL" / fim de transmissao
// ---------------------------------------------------------------------------
//
// Barato: fundo preto, moldura e duas linhas de texto. Nada de por a data ou a
// hora aqui — isso obrigaria a repintar o quadro inteiro a cada segundo.
//
// `boxOnly` desenha so a caixa, sem apagar o fundo: e assim que o cartaz entra
// por cima do chuvisco em drawNoSignal().
inline void drawSlateText(lgfx::LovyanGFX *dst, int ox, int oy, const char *title, const char *sub,
                          bool boxOnly = false) {
  if (!dst)
    return;

  // Caixa centralizada e inteiramente dentro da area segura.
  const int bx = crt::SAFE_L;
  const int bw = crt::SAFE_W;
  const int bh = sub ? 84 : 56;
  const int by = (crt::H - bh) / 2;

  char t[24], s[32];
  const char *tt = detail::norm(t, sizeof(t), title ? title : "NO SIGNAL");
  const char *ss = sub ? detail::norm(s, sizeof(s), sub) : (const char *)0;

  dst->startWrite();
  if (!boxOnly)
    dst->fillRect(ox, oy, crt::W, crt::H, (uint16_t)BLACK);
  dst->fillRect(ox + bx, oy + by, bw, bh, (uint16_t)BLACK);
  dst->drawRect(ox + bx, oy + by, bw, bh, (uint16_t)GRAY75);
  dst->drawRect(ox + bx + 1, oy + by + 1, bw - 2, bh - 2, (uint16_t)GRAY75);

  // VcrFont escala 2 = celula 24x32, traco de 4 px: e a unica fonte do projeto
  // que aguenta o borrao horizontal do NTSC em tamanho grande.
  vcrfont::drawTextCentered(dst, tt, ox + bx, oy + by + 12, bw, (int32_t)WHITE100, 2);
  if (ss)
    vcrfont::drawTextCentered(dst, ss, ox + bx, oy + by + 52, bw, (int32_t)GRAY75, 1);
  dst->endWrite();
}

// Pintor (dst, ox, oy) do cartaz limpo.
inline void drawSlate(lgfx::LovyanGFX *dst, int ox, int oy) {
  drawSlateText(dst, ox, oy, "NO SIGNAL", (const char *)0);
}

// Chuvisco com o cartaz por cima — o "sem sinal" completo.
inline void drawNoSignal(lgfx::LovyanGFX *dst, int ox, int oy) {
  drawSnow(dst, ox, oy);
  drawSlateText(dst, ox, oy, "NO SIGNAL", (const char *)0, true);
}

// Adaptadores para o ScreenFx::Paint, que e (dst, ox, oy, void *user).
inline void paintBars(lgfx::LovyanGFX *dst, int ox, int oy, void *) { drawBars(dst, ox, oy); }
inline void paintSnow(lgfx::LovyanGFX *dst, int ox, int oy, void *) { drawSnow(dst, ox, oy); }
inline void paintSlate(lgfx::LovyanGFX *dst, int ox, int oy, void *) { drawSlate(dst, ox, oy); }

// ===========================================================================
//  4. Tom de referencia de 1 kHz
// ===========================================================================
//
// O outro metade do par "bars and tone". Este header NAO instala, NAO
// configura e NAO escreve no I2S: I2S0 e do video composto e o I2S1 tem dono
// (o audioTask do main.cpp). Aqui so se produz PCM.
//
// Por que 441 amostras:
//
//   1 kHz a 22050 Hz da 22,05 amostras por ciclo — nao fecha em inteiro, e uma
//   tabela de um ciclo so emendaria com salto de fase a cada volta (clique
//   audivel, e um espalhamento de espectro que estraga a medida). A saida e
//   fechar em VARIOS ciclos:
//
//       441 amostras = 20 ciclos exatos
//       f = 22050 * 20 / 441 = 1000,000000 Hz   (exato, nao aproximado)
//
//   441 = 3^2 * 7^2 e 22050 = 50 * 441, entao a divisao e exata por
//   construcao. Como mdc(20, 441) = 1, as 441 amostras sao uma permutacao de
//   sin(2*pi*k/441): a tabela usa a resolucao de fase inteira, sem repeticao.
//
//   Custo: 441 * 2 = **882 bytes**, e em FLASH (.rodata), porque e um
//   `static const` com inicializador constante dentro de funcao inline. A SRAM
//   gasta sao os 4 bytes de estado do gerador. Uma tabela de 1 segundo estereo
//   seria 88.200 bytes — mais do que o heap livre inteiro do aparelho (28-38
//   KB), que e exatamente o erro que esta conta evita.
//
//   Conferido: soma das 441 amostras = 0 exato (sem offset DC) e pico
//   +32767/-32767 (sem estouro). O gerador atenua, nunca amplifica, entao nao
//   ha como cortar.
// ---------------------------------------------------------------------------

constexpr uint32_t SAMPLE_RATE = 22050; // igual ao `sampleRate` do main.cpp
constexpr uint32_t TONE_HZ = 1000;      // exato, nao aproximado
constexpr int TONE_TABLE_LEN = 441;     // 20 ciclos
constexpr int TONE_TABLE_BYTES = TONE_TABLE_LEN * 2; // 882

// Amplitudes em Q8 (256 = escala cheia). O padrao de sala de controle para
// "bars and tone" e -20 dBFS: audivel, com folga de sobra no caminho analogico.
constexpr uint8_t AMP_MINUS20DB = 26;  // 26/256 = 0,1016 -> -19,86 dBFS
constexpr uint8_t AMP_MINUS12DB = 64;  // 64/256 = 0,2500 -> -12,04 dBFS
constexpr uint8_t AMP_MINUS6DB = 128;  // 128/256 = 0,500 ->  -6,02 dBFS
constexpr uint8_t AMP_FULL = 255;      // 255/256 -> -0,03 dBFS (ainda sem cortar)

// A tabela vive aqui dentro para ter uma copia so no binario, mesmo se o
// header for incluido por varias unidades de traducao.
inline const int16_t *toneTable() {
  static const int16_t kSine[TONE_TABLE_LEN] = {
           0,   9211,  17679,  24722,  29771,  32418,  32451,  29867,  24874,  17876,
        9435,    233,  -8987, -17482, -24568, -29672, -32383, -32483, -29963, -25026,
      -18071,  -9658,   -467,   8762,  17285,  24413,  29572,  32347,  32513,  30056,
       25176,  18265,   9881,    700,  -8537, -17086, -24257, -29471, -32309, -32541,
      -30148, -25325, -18458, -10103,   -934,   8311,  16886,  24099,  29368,  32269,
       32567,  30239,  25472,  18651,  10325,   1167,  -8085, -16686, -23940, -29264,
      -32228, -32592, -30328, -25618, -18842, -10546,  -1400,   7859,  16484,  23780,
       29158,  32185,  32616,  30416,  25763,  19033,  10767,   1633,  -7632, -16282,
      -23619, -29051, -32140, -32637, -30502, -25907, -19222, -10987,  -1866,   7405,
       16079,  23457,  28942,  32094,  32657,  30586,  26049,  19411,  11207,   2099,
       -7178, -15876, -23293, -28832, -32046, -32675, -30669, -26190, -19598, -11426,
       -2332,   6950,  15671,  23128,  28721,  31997,  32692,  30751,  26330,  19785,
       11645,   2565,  -6721, -15466, -22963, -28608, -31945, -32707, -30831, -26468,
      -19970, -11862,  -2798,   6493,  15259,  22795,  28493,  31893,  32720,  30909,
       26605,  20155,  12080,   3030,  -6264, -15052, -22627, -28377, -31838, -32732,
      -30986, -26740, -20339, -12296,  -3263,   6034,  14845,  22458,  28260,  31782,
       32742,  31061,  26875,  20521,  12512,   3495,  -5805, -14636, -22287, -28141,
      -31725, -32750, -31134, -27007, -20702, -12728,  -3727,   5575,  14427,  22116,
       28020,  31666,  32757,  31206,  27139,  20883,  12943,   3959,  -5345, -14217,
      -21943, -27899, -31605, -32762, -31277, -27269, -21062, -13157,  -4190,   5114,
       14006,  21769,  27776,  31542,  32765,  31345,  27398,  21240,  13370,   4422,
       -4884, -13795, -21594, -27651, -31478, -32767, -31413, -27525, -21418, -13583,
       -4653,   4653,  13583,  21418,  27525,  31413,  32767,  31478,  27651,  21594,
       13795,   4884,  -4422, -13370, -21240, -27398, -31345, -32765, -31542, -27776,
      -21769, -14006,  -5114,   4190,  13157,  21062,  27269,  31277,  32762,  31605,
       27899,  21943,  14217,   5345,  -3959, -12943, -20883, -27139, -31206, -32757,
      -31666, -28020, -22116, -14427,  -5575,   3727,  12728,  20702,  27007,  31134,
       32750,  31725,  28141,  22287,  14636,   5805,  -3495, -12512, -20521, -26875,
      -31061, -32742, -31782, -28260, -22458, -14845,  -6034,   3263,  12296,  20339,
       26740,  30986,  32732,  31838,  28377,  22627,  15052,   6264,  -3030, -12080,
      -20155, -26605, -30909, -32720, -31893, -28493, -22795, -15259,  -6493,   2798,
       11862,  19970,  26468,  30831,  32707,  31945,  28608,  22963,  15466,   6721,
       -2565, -11645, -19785, -26330, -30751, -32692, -31997, -28721, -23128, -15671,
       -6950,   2332,  11426,  19598,  26190,  30669,  32675,  32046,  28832,  23293,
       15876,   7178,  -2099, -11207, -19411, -26049, -30586, -32657, -32094, -28942,
      -23457, -16079,  -7405,   1866,  10987,  19222,  25907,  30502,  32637,  32140,
       29051,  23619,  16282,   7632,  -1633, -10767, -19033, -25763, -30416, -32616,
      -32185, -29158, -23780, -16484,  -7859,   1400,  10546,  18842,  25618,  30328,
       32592,  32228,  29264,  23940,  16686,   8085,  -1167, -10325, -18651, -25472,
      -30239, -32567, -32269, -29368, -24099, -16886,  -8311,    934,  10103,  18458,
       25325,  30148,  32541,  32309,  29471,  24257,  17086,   8537,   -700,  -9881,
      -18265, -25176, -30056, -32513, -32347, -29572, -24413, -17285,  -8762,    467,
        9658,  18071,  25026,  29963,  32483,  32383,  29672,  24568,  17482,   8987,
        -233,  -9435, -17876, -24874, -29867, -32451, -32418, -29771, -24722, -17679,
       -9211,
  };
  return kSine;
}

// Gerador de tom. 4 bytes de estado; a fase sobrevive entre chamadas, entao
// blocos consecutivos emendam sem salto.
class Tone1k {
public:
  constexpr Tone1k() : phase_(0), amp_(AMP_MINUS20DB) {}

  void setAmplitude(uint8_t q8) { amp_ = q8; }
  uint8_t amplitude() const { return amp_; }
  void reset() { phase_ = 0; }

  // Preenche `samples` valores int16 ESTEREO INTERCALADOS (L, R, L, R...) —
  // `samples` conta valores, nao quadros, que e como o audioTask ja conta
  // (`bytes / sizeof(int16_t)`). Um valor impar sobrando e ignorado.
  // Devolve quantos valores foram escritos.
  size_t fillPcm(int16_t *dst, size_t samples) {
    if (!dst)
      return 0;
    const int16_t *t = toneTable();
    const size_t n = samples & ~(size_t)1;
    const int32_t a = (int32_t)amp_;
    uint16_t p = phase_;
    for (size_t i = 0; i < n; i += 2) {
      // /256 e nao >>8: o deslocamento arredonda para -infinito, entao cada
      // amostra negativa perde ate 1 LSB e a positiva nao. Sao 220 negativas
      // em 441, o que injeta -218 por periodo -- medido -10900 de offset DC em
      // 1 s. A divisao trunca em direcao ao zero, simetrica, e a soma volta a
      // ser exatamente 0. O compilador nao troca isto por um shift justamente
      // porque os dois nao sao equivalentes com sinal.
      const int16_t v = (int16_t)(((int32_t)t[p] * a) / 256);
      dst[i] = v;
      dst[i + 1] = v;
      if (++p >= (uint16_t)TONE_TABLE_LEN)
        p = 0;
    }
    phase_ = p;
    return n;
  }

  // Mesma coisa em mono, para quem for misturar antes de intercalar.
  size_t fillPcmMono(int16_t *dst, size_t samples) {
    if (!dst)
      return 0;
    const int16_t *t = toneTable();
    const int32_t a = (int32_t)amp_;
    uint16_t p = phase_;
    for (size_t i = 0; i < samples; ++i) {
      dst[i] = (int16_t)(((int32_t)t[p] * a) / 256); // ver fillPcm: >>8 injeta DC
      if (++p >= (uint16_t)TONE_TABLE_LEN)
        p = 0;
    }
    phase_ = p;
    return samples;
  }

private:
  uint16_t phase_;
  uint8_t amp_;
};

// Instancia de conveniencia, constante-inicializada (construtor constexpr).
inline Tone1k &sharedTone() {
  static Tone1k t;
  return t;
}

} // namespace testpattern
