#pragma once
// ============================================================================
// ChannelMode.h — "modo canal" (reproducao continua) e timer de soneca.
//
// Duas pecas que transformam o player de arquivos num aparelho de TV:
//
//   1. channel::Channel   — quando um programa acaba, o proximo entra sozinho,
//                           com uma vinheta curta ("A SEGUIR") no meio. E o que
//                           separa "abrir um arquivo" de "ligar um canal".
//   2. sleeptimer::SleepTimer — o SLEEP do videocassete: 15/30/60/90/120 min e
//                           o aparelho se desliga. Quem dorme na frente da TV
//                           nao levanta para apertar nada.
//
// ---------------------------------------------------------------------------
// O que este header NAO faz (de proposito)
// ---------------------------------------------------------------------------
//
// * Nao varre o cartao SD. A biblioteca ja existe em src/main.cpp
//   (scanLibrary / libraryProgramCount / libraryProgramAt) e re-implementar
//   isso aqui seria a armadilha 8 do AGENTS.md — duas coisas descrevendo o
//   mesmo estado, divergindo na primeira manutencao. O Channel guarda so
//   INDICES e pede o caminho ao chamador por um ponteiro de funcao.
// * Nao abre, fecha nem toca nada. Ele devolve "toque o indice N agora"; quem
//   chama o startProgram() continua sendo o loop().
// * Nao desliga o aparelho. O SleepTimer so responde `expired()`.
//
// ---------------------------------------------------------------------------
// Restricoes respeitadas (ver AGENTS.md secao 2)
// ---------------------------------------------------------------------------
//
// * C++11 (-std=gnu++11). Nenhum constexpr com laco ou variavel local — foi
//   isso que quebrou a VcrFont.h uma vez. As tabelas pequenas sao arrays locais
//   de funcao inline, que o compilador resolve sem exigir C++14.
// * SRAM. O objeto Channel inteiro cabe em ~90 bytes: a ordem embaralhada e um
//   array de 64 uint8_t (indices, nao ponteiros, nao String). Nenhum sprite —
//   um sprite de 5 KB ja causou falta de memoria neste projeto, e a vinheta e
//   desenhada direto no painel.
// * Texto ASCII e sem acento. O nome da pasta vem do cartao, entao passa por
//   ascii::normalizeUpper antes de ir para a tela.
// * Nada de ciano saturado (0x07FF): o dot crawl do NTSC faz aquilo tremer. O
//   acento e o 0x96BC que o resto do firmware ja usa.
// * Tudo dentro de crt::SAFE_* (SafeArea.h); so o fundo sangra ate a borda do
//   raster, sem tarja preta.
// * Prazo com millis(): TODA conta de tempo passa por timeReached() (UiLogic.h),
//   que compara a diferenca com sinal. millis() da a volta em ~49 dias e
//   `now > deadline` cru trava o timer para sempre quando isso acontece.
//
// Dependencias: M5GFX (vem pela VcrFont), SafeArea.h, UiLogic.h, Ascii.h.
// Nada de Arduino, SD, WiFi ou FreeRTOS: o destino e um lgfx::LovyanGFX*, entao
// o mesmo header serve ao painel CVBS do aparelho e ao painel SDL do simulador.
//
// Bancada: sim/probes/channel_mode.cpp.
// ============================================================================

#include "Ascii.h"
#include "SafeArea.h"
#include "UiLogic.h" // timeReached()
#include "VcrFont.h" // traz o M5GFX junto

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

namespace channel {

// Acento seguro na saida composta. TFT_CYAN (0x07FF) esta proibido aqui: a
// crominancia saturada do NTSC produz dot crawl visivel em borda de texto.
constexpr uint16_t kAccent = 0x96BC;

// Mesmo teto de MAX_LIBRARY_ITEMS em src/main.cpp. Cabe em uint8_t de
// proposito: a permutacao do modo aleatorio e um array de 64 bytes.
constexpr int MAX_ITEMS = 64;

// Duracao padrao da vinheta entre programas. Curta: e respiro, nao intervalo.
constexpr uint32_t kDefaultBumperMs = 3000;

enum class Mode : uint8_t {
  Off = 0,        // comportamento atual: acabou o programa, volta para a lista
  Sequential = 1, // proximo da lista, dando a volta no fim
  Shuffle = 2     // permutacao sem repetir ate a lista esgotar
};

enum class Phase : uint8_t {
  Idle = 0,   // nada agendado
  Playing = 1,// um programa esta no ar
  Bumper = 2  // vinheta correndo; o proximo indice ja esta escolhido
};

// Como o Channel resolve um indice em caminho de pasta. O `user` e opaco: em
// src/main.cpp ele nem precisa ser usado, porque libraryProgramAt() e global.
typedef const char *(*PathFn)(int index, void *user);

// ---------------------------------------------------------------------------
//  Channel
// ---------------------------------------------------------------------------
//
// Ciclo de vida visto pelo chamador:
//
//   ch.attach(libraryProgramCount(), pathAdapter);   // ao entrar na biblioteca
//   ch.seed(millis());                               // semente da permutacao
//   ...
//   startProgram(p) deu certo  -> ch.started(index, millis());
//   playbackFinished           -> if (ch.finished(millis())) { desenha vinheta }
//                                 else { comportamento antigo: volta a lista }
//   a cada loop, em Bumper     -> ch.drawBumper(...); int n = ch.ready(millis());
//                                 if (n >= 0) startProgram(ch.pathAt(n));
//   startProgram falhou        -> if (!ch.failed(millis())) { volta a lista }
//   usuario apertou VOLTAR     -> ch.stop();
//
class Channel {
public:
  Channel()
      : pathFn_(0), user_(0), count_(0), cursor_(0), current_(-1), pending_(-1),
        failStreak_(0), mode_(Mode::Off), phase_(Phase::Idle), shuffled_(false),
        bumperMs_(kDefaultBumperMs), bumperStart_(0), bumperUntil_(0), rng_(1u), seed_(1u) {
    for (int i = 0; i < MAX_ITEMS; ++i)
      order_[i] = (uint8_t)i;
    paintDst_[0] = paintDst_[1] = 0;
    paintKey_[0] = paintKey_[1] = 0;
  }

  // -- playlist ------------------------------------------------------------

  // `count` e quantos programas a biblioteca listou; `fn` traduz indice em
  // caminho. Chamar de novo depois de re-varrer o cartao e barato.
  void attach(int count, PathFn fn, void *user = 0) {
    pathFn_ = fn;
    user_ = user;
    setCount(count);
  }

  void setCount(int count) {
    if (count < 0)
      count = 0;
    if (count > MAX_ITEMS)
      count = MAX_ITEMS;
    if (count == count_)
      return;
    count_ = count;
    shuffled_ = false; // a permutacao antiga nao vale mais
    if (current_ >= count_)
      current_ = -1;
    if (pending_ >= count_)
      pending_ = -1;
  }

  int count() const { return count_; }

  // Semente da permutacao. Deterministica de proposito: com a mesma semente a
  // ordem se repete, o que torna o modo aleatorio testavel numa bancada. Zero
  // nao serve como estado de LCG, entao vira 1.
  void seed(uint32_t s) {
    seed_ = s ? s : 1u;
    rng_ = seed_;
    shuffled_ = false;
  }

  const char *pathAt(int index) const {
    if (!pathFn_ || index < 0 || index >= count_)
      return 0;
    return pathFn_(index, user_);
  }

  const char *pendingPath() const { return pathAt(pending_); }

  // -- modo ----------------------------------------------------------------

  Mode mode() const { return mode_; }

  void setMode(Mode m) {
    if (m == mode_)
      return;
    mode_ = m;
    shuffled_ = false;
    if (mode_ == Mode::Off)
      stop();
  }

  // OFF -> SEQUENCIAL -> ALEATORIO -> OFF. Devolve o modo novo.
  Mode cycleMode() {
    setMode(mode_ == Mode::Off ? Mode::Sequential
            : mode_ == Mode::Sequential ? Mode::Shuffle
                                        : Mode::Off);
    return mode_;
  }

  // Rotulo ASCII para a tela de configuracoes.
  const char *modeLabel() const {
    return mode_ == Mode::Off ? "DESLIGADO" : mode_ == Mode::Sequential ? "EM ORDEM" : "ALEATORIO";
  }

  // -- vinheta -------------------------------------------------------------

  void setBumperMs(uint32_t ms) { bumperMs_ = ms; }
  uint32_t bumperMs() const { return bumperMs_; }

  Phase phase() const { return phase_; }
  bool bumperActive() const { return phase_ == Phase::Bumper; }
  int currentIndex() const { return current_; }
  int pendingIndex() const { return pending_; }

  // Quanto falta da vinheta, em ms. Sempre pela diferenca sem sinal — nunca
  // `deadline - now` cru sem antes checar timeReached, senao a volta do
  // millis() devolveria um numero gigante e a vinheta ficaria eterna.
  uint32_t bumperRemainingMs(uint32_t now) const {
    if (phase_ != Phase::Bumper || timeReached(now, bumperUntil_))
      return 0;
    return bumperUntil_ - now;
  }

  uint32_t bumperElapsedMs(uint32_t now) const {
    if (phase_ != Phase::Bumper)
      return 0;
    const uint32_t d = now - bumperStart_; // subtracao sem sinal: atravessa a volta
    return d > bumperMs_ ? bumperMs_ : d;
  }

  // -- transicoes ----------------------------------------------------------

  // O programa do indice `index` comecou a tocar de verdade.
  void started(int index, uint32_t now) {
    (void)now;
    current_ = (index >= 0 && index < count_) ? index : -1;
    pending_ = -1;
    failStreak_ = 0;
    phase_ = Phase::Playing;
    invalidateBumperPaint();
    // O usuario pode ter escolhido um titulo a dedo na biblioteca no meio de
    // uma rodada aleatoria. Puxar esse titulo para a posicao do cursor mantem a
    // promessa de nao repetir ninguem ate a lista esgotar.
    if (mode_ == Mode::Shuffle && shuffled_ && current_ >= 0 && cursor_ < count_ &&
        order_[cursor_] != (uint8_t)current_) {
      for (int i = cursor_; i < count_; ++i) {
        if (order_[i] == (uint8_t)current_) {
          const uint8_t t = order_[i];
          order_[i] = order_[cursor_];
          order_[cursor_] = t;
          break;
        }
      }
    }
  }

  // O programa acabou (chamar quando playbackFinished e nao houve erro de
  // midia). Devolve true quando o canal assumiu: o proximo indice ja esta
  // escolhido e a vinheta comecou — o chamador NAO deve voltar para a
  // biblioteca. Devolve false quando o comportamento antigo vale (modo
  // desligado, lista vazia ou programas demais falhando em sequencia).
  bool finished(uint32_t now) {
    if (mode_ == Mode::Off || count_ <= 0) {
      stop();
      return false;
    }
    // Portao contra laco apertado (armadilha 4 do AGENTS.md): um cartao cheio
    // de pastas quebradas nao pode fazer o aparelho girar a lista para sempre.
    if (failStreak_ > count_) {
      stop();
      return false;
    }
    const int next = nextIndex();
    if (next < 0) {
      stop();
      return false;
    }
    pending_ = next;
    bumperStart_ = now;
    bumperUntil_ = now + bumperMs_;
    phase_ = Phase::Bumper;
    invalidateBumperPaint();
    return true;
  }

  // Acabou a vinheta? Devolve o indice a tocar agora, ou -1 enquanto nao for a
  // hora. Depois de devolver um indice, ele nao volta a ser devolvido.
  int ready(uint32_t now) {
    if (phase_ != Phase::Bumper || pending_ < 0)
      return -1;
    if (!timeReached(now, bumperUntil_))
      return -1;
    phase_ = Phase::Idle; // esperando o startProgram() do chamador
    // Ja adota o pendente como atual: se o start falhar, failed() avanca a
    // partir dele e nao tenta o mesmo de novo.
    current_ = pending_;
    return pending_;
  }

  // O startProgram() do indice devolvido por ready() falhou. Reagenda o
  // seguinte; devolve false quando ja tentou a lista inteira sem sucesso.
  bool failed(uint32_t now) {
    ++failStreak_;
    return finished(now);
  }

  // Cancelamento explicito (usuario saiu do player, erro de midia, entrou
  // outra tela). Nao mexe no modo: o canal volta a valer no proximo programa.
  void stop() {
    phase_ = Phase::Idle;
    pending_ = -1;
    failStreak_ = 0;
    invalidateBumperPaint();
  }

  // -- desenho da vinheta --------------------------------------------------

  // Cartao "A SEGUIR" + titulo do proximo programa + barra de progresso da
  // vinheta. `nextTitle` pode ser 0: o titulo sai do caminho pendente.
  //
  // Repintura: o cartao inteiro so e pintado na primeira chamada de cada
  // vinheta e para cada destino (LCD e CVBS sao dois destinos). Nas chamadas
  // seguintes so a barra cresce. Nao e micro-otimizacao: repintar o fundo a
  // cada quadro deixaria o texto ausente do framebuffer nos microssegundos em
  // que o DMA do I2S0 varre aquelas linhas, e isso pisca na TV.
  void drawBumper(lgfx::LovyanGFX *dst, int ox, int oy, const char *nextTitle, uint32_t now) {
    if (!dst || phase_ != Phase::Bumper)
      return;

    char derived[kTitleCap];
    if (!nextTitle) {
      titleOf(pending_, derived, sizeof(derived));
      nextTitle = derived;
    }

    const int barX = crt::SAFE_L, barW = crt::SAFE_W;
    const int barY = crt::SAFE_B - 28, barH = 8;

    if (needsFullPaint(dst)) {
      // O fundo pode sangrar ate a borda do raster; o conteudo, nao.
      dst->fillRect(ox, oy, crt::W, crt::H, TFT_BLACK);
      vcrfont::drawTextCentered(dst, "A SEGUIR", ox + crt::SAFE_L, oy + crt::HEAD_Y, crt::SAFE_W,
                                kAccent);
      dst->drawFastHLine(ox + crt::SAFE_L, oy + crt::HEAD_RULE_Y, crt::SAFE_W, kAccent);
      drawTitle(dst, ox, oy, nextTitle);
      dst->drawRect(ox + barX, oy + barY, barW, barH, kAccent);
      vcrfont::drawTextCentered(dst, "M5 RETRO TV", ox + crt::SAFE_L, oy + crt::SAFE_B - 16,
                                crt::SAFE_W, kDim);
    }

    // A barra so cresce da esquerda para a direita: nunca apaga pixel, entao
    // repintar nao produz cintilacao.
    const uint32_t span = bumperMs_ ? bumperMs_ : 1u;
    const int inner = barW - 2;
    int fill = (int)((uint32_t)inner * bumperElapsedMs(now) / span);
    if (fill > inner)
      fill = inner;
    if (fill > 0)
      dst->fillRect(ox + barX + 1, oy + barY + 1, fill, barH - 2, kAccent);
  }

  // Forca a proxima drawBumper() a repintar o cartao inteiro. Use ao voltar
  // para a tela depois de outra coisa ter escrito por cima.
  void invalidateBumperPaint() {
    paintDst_[0] = paintDst_[1] = 0;
    paintKey_[0] = paintKey_[1] = 0;
  }

  // Titulo ASCII de um indice, a partir do ultimo segmento do caminho. Devolve
  // false (e escreve string vazia) quando nao ha caminho.
  bool titleOf(int index, char *out, size_t cap) const {
    if (!out || cap == 0)
      return false;
    out[0] = '\0';
    const char *path = pathAt(index);
    if (!path)
      return false;
    const char *base = path;
    for (const char *p = path; *p; ++p)
      if (*p == '/' || *p == '\\')
        base = p + 1;
    if (!*base)
      base = path;
    // Texto vindo do cartao: sem normalizar, um acento vira dois buracos.
    ascii::normalizeUpper(out, cap, base);
    return out[0] != '\0';
  }

private:
  static constexpr int kTitleCap = 25;  // 22 celulas de 12 px cabem em SAFE_W
  static constexpr uint16_t kDim = 0x8410; // cinza medio, luminancia segura

  // LCG de 32 bits (Numerical Recipes). Nada de rand(): a libc do ESP32 tem
  // estado global compartilhado com o resto do firmware e nao e reproduzivel
  // numa bancada.
  uint32_t nextRandom() {
    rng_ = rng_ * 1664525u + 1013904223u;
    return rng_;
  }

  // Sorteio em [0, n) pelos bits altos — o resto de um LCG tem periodo curto
  // nos bits baixos e produziria uma "aleatoriedade" que anda em ciclo.
  int below(int n) {
    if (n <= 1)
      return 0;
    return (int)(((uint64_t)nextRandom() * (uint32_t)n) >> 32);
  }

  // Fisher-Yates completo: a rodada e uma permutacao, entao ninguem se repete
  // antes de todo mundo ter tocado. `avoid` e o programa que acabou de sair,
  // que nao pode abrir a rodada seguinte (seria repeticao imediata na virada).
  void reshuffle(int avoid) {
    for (int i = 0; i < count_; ++i)
      order_[i] = (uint8_t)i;
    for (int i = count_ - 1; i > 0; --i) {
      const int j = below(i + 1);
      const uint8_t t = order_[i];
      order_[i] = order_[j];
      order_[j] = t;
    }
    if (count_ > 1 && avoid >= 0 && order_[0] == (uint8_t)avoid) {
      const uint8_t t = order_[0];
      order_[0] = order_[count_ - 1];
      order_[count_ - 1] = t;
    }
    cursor_ = 0;
    shuffled_ = true;
  }

  int nextIndex() {
    if (count_ <= 0)
      return -1;
    if (count_ == 1)
      return 0;
    if (mode_ == Mode::Shuffle) {
      if (!shuffled_)
        reshuffle(current_);
      else if (++cursor_ >= count_)
        reshuffle(current_); // lista esgotada: nova rodada
      return (int)order_[cursor_];
    }
    return current_ < 0 ? 0 : (current_ + 1) % count_;
  }

  // Titulo grande quando couber (24x32), pequeno quando nao. Truncar e melhor
  // do que deixar escapar da area segura.
  void drawTitle(lgfx::LovyanGFX *dst, int ox, int oy, const char *title) const {
    // Normaliza mesmo quando o titulo veio pronto do chamador: ele tambem pode
    // ter saido de um meta.json do cartao, e um acento em UTF-8 sao dois bytes
    // sem glifo — dois buracos, nao um (armadilha 9 do AGENTS.md).
    char buf[kTitleCap];
    int n = (int)ascii::normalizeUpper(buf, sizeof(buf), title ? title : "");
    int scale = 2;
    int maxChars = crt::SAFE_W / (vcrfont::CELL_W * 2); // 11
    if (n > maxChars) {
      scale = 1;
      maxChars = crt::SAFE_W / vcrfont::CELL_W; // 22
      if (n > maxChars) {
        // Reticencias: sem elas o corte parece nome de pasta errado, e nao
        // titulo que nao coube.
        buf[maxChars] = '\0';
        buf[maxChars - 1] = '.';
        buf[maxChars - 2] = '.';
      }
    }
    const int h = vcrfont::textHeight(scale);
    const int y = crt::SAFE_T + (crt::SAFE_H - h) / 2;
    vcrfont::drawTextCentered(dst, buf, ox + crt::SAFE_L, oy + y, crt::SAFE_W, TFT_WHITE, scale);
  }

  // Duas vagas: exatamente os dois destinos que existem (LCD e painel CVBS).
  bool needsFullPaint(const void *dst) {
    int slot = -1;
    for (int i = 0; i < 2; ++i)
      if (paintDst_[i] == dst)
        slot = i;
    if (slot < 0)
      for (int i = 0; i < 2 && slot < 0; ++i)
        if (paintDst_[i] == 0)
          slot = i;
    if (slot < 0)
      slot = 0; // mais de dois destinos: o terceiro so repinta, nunca erra
    const uint32_t key = bumperStart_ | 1u; // 0 fica reservado para "vazio"
    if (paintDst_[slot] == dst && paintKey_[slot] == key)
      return false;
    paintDst_[slot] = dst;
    paintKey_[slot] = key;
    return true;
  }

  PathFn pathFn_;
  void *user_;
  int count_;
  int cursor_;  // posicao atual dentro de order_ (modo aleatorio)
  int current_; // indice tocando agora, -1 se nenhum
  int pending_; // indice escolhido para depois da vinheta, -1 se nenhum
  int failStreak_;
  Mode mode_;
  Phase phase_;
  bool shuffled_;
  uint32_t bumperMs_;
  uint32_t bumperStart_;
  uint32_t bumperUntil_;
  uint32_t rng_;
  uint32_t seed_;
  uint8_t order_[MAX_ITEMS];
  const void *paintDst_[2];
  uint32_t paintKey_[2];
};

} // namespace channel

namespace sleeptimer {

// OFF / 15 / 30 / 60 / 90 / 120 — os mesmos degraus do SLEEP de um
// videocassete da epoca. Em minutos; 0 e "desligado".
constexpr int OPTION_COUNT = 6;

// Array local de funcao inline em vez de tabela no escopo do namespace: evita
// objeto com ligacao interna referenciado por funcao inline (e continua sendo
// C++11 — um constexpr com laco quebraria o firmware).
inline uint16_t optionAt(int i) {
  const uint16_t v[OPTION_COUNT] = {0, 15, 30, 60, 90, 120};
  return (i >= 0 && i < OPTION_COUNT) ? v[i] : 0;
}

class SleepTimer {
public:
  SleepTimer() : option_(0), start_(0), deadline_(0) {}

  bool active() const { return option_ != 0; }
  int option() const { return option_; }
  uint16_t minutes() const { return optionAt(option_); }

  void cancel() {
    option_ = 0;
    start_ = deadline_ = 0;
  }

  // Seleciona pelo indice da tabela e rearma a contagem.
  void setOption(int i, uint32_t now) {
    option_ = (uint8_t)((i >= 0 && i < OPTION_COUNT) ? i : 0);
    arm(now);
  }

  // Seleciona por valor em minutos. Valor fora da tabela desliga o timer —
  // melhor desligado do que com um prazo que a interface nao sabe mostrar.
  void setMinutes(uint16_t m, uint32_t now) {
    for (int i = 0; i < OPTION_COUNT; ++i) {
      if (optionAt(i) == m) {
        setOption(i, now);
        return;
      }
    }
    cancel();
  }

  // Proximo degrau, dando a volta. Devolve o valor novo em minutos (0 = OFF).
  uint16_t cycle(uint32_t now) {
    setOption((option_ + 1) % OPTION_COUNT, now);
    return minutes();
  }

  // Reinicia a contagem sem mudar o valor (o usuario mexeu em algo: nao e hora
  // de desligar na cara dele).
  void restart(uint32_t now) { arm(now); }

  // Venceu? Unico lugar que decide isso. timeReached() compara a DIFERENCA com
  // sinal: `now >= deadline` cru daria falso para sempre depois da volta do
  // millis(), e o aparelho nunca desligaria.
  bool expired(uint32_t now) const { return active() && timeReached(now, deadline_); }

  uint32_t remainingMs(uint32_t now) const {
    if (!active() || timeReached(now, deadline_))
      return 0;
    return deadline_ - now;
  }

  // Arredonda para cima: enquanto sobrar 1 s o mostrador diz "1", nunca "0".
  // Um "SLEEP 0" na tela por um minuto inteiro parece aparelho travado.
  uint16_t remainingMinutes(uint32_t now) const {
    return (uint16_t)((remainingMs(now) + 59999UL) / 60000UL);
  }

  uint32_t deadline() const { return deadline_; }

  // Indicador pequeno "SLEEP 30", alinhado a direita da area segura. Com
  // contorno preto por glifo (mesma ideia do VcrOsd) para continuar legivel
  // por cima da imagem, sem tarja de fundo.
  void drawBadge(lgfx::LovyanGFX *dst, int ox, int oy, uint32_t now, int y = crt::SAFE_T,
                 int32_t ink = (int32_t)channel::kAccent, int32_t outline = TFT_BLACK) const {
    if (!dst || !active())
      return;
    char text[16];
    snprintf(text, sizeof(text), "SLEEP %u", (unsigned)remainingMinutes(now));
    const int w = vcrfont::textWidth(text);
    vcrfont::drawText(dst, text, ox + crt::SAFE_R - w, oy + y, ink, 1, outline);
  }

private:
  void arm(uint32_t now) {
    start_ = now;
    // 120 min = 7.200.000 ms, folgado dentro dos 24,8 dias em que a
    // comparacao com sinal do timeReached() vale.
    deadline_ = now + (uint32_t)minutes() * 60000UL;
  }

  uint8_t option_;
  uint32_t start_;
  uint32_t deadline_;
};

} // namespace sleeptimer
