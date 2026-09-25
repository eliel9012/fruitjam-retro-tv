#pragma once
// ============================================================================
// AudioScope.h — visualizador de audio ao vivo para a saida composta.
//
// O que resolve: durante a musica (MUSIC_NOW_PLAYING) e a radio (RADIO) a tela
// fica praticamente parada. Isso e chato e, num tubo, e ruim: pixel parado por
// meia hora marca o fosforo. Um osciloscopio e um analisador de espectro
// resolvem as duas coisas de uma vez, e sao exatamente o que um som de
// componentes ou um radiogravador da epoca tinha no painel.
//
// Tres modos, ciclados por `nextMode()`:
//
//   SCOPE     dominio do tempo. A propria onda, 128 colunas. Custo ~zero: e so
//             um mapeamento linear amostra -> linha.
//   BARS      dominio da frequencia. 12 barras de meia oitava, 177 Hz a 8 kHz,
//             com ponto de pico que cai devagar. E o modo que custa conta.
//   VU        par de agulhas L/R com retencao de pico. Quase de graca: o pico
//             ja e medido pelo produtor.
//
// ---------------------------------------------------------------------------
// 1. A REGRA DAS DUAS CPUs (a mais importante deste arquivo)
// ---------------------------------------------------------------------------
// O `audioTask` roda no **core 0**, prioridade 4, e alimenta o I2S. Atraso ali
// e estouro de buffer AUDIVEL. Entao **nenhuma analise roda dentro dele**.
//
//   produtor (core 0, audioTask):  Tap::publish()  -> copia N amostras e um
//                                  par de picos, e incrementa um contador
//                                  atomico. Nada de janela, nada de Goertzel,
//                                  nada de desenho.
//   consumidor (core 1, loop()):   Tap::snapshot() -> copia a janela para si e
//                                  so entao faz janelamento, transformada e
//                                  desenho.
//
// O canal e um **seqlock de banco duplo**, no mesmo espirito do buffer duplo
// que o radar e a previsao ja usam (AGENTS.md 3.3):
//
//   * `seq_` e o unico atomico. O banco publicado e `seq_ & 1`.
//   * O produtor sempre escreve no banco `(seq_+1) & 1`, isto e, no que NAO
//     esta publicado, e so depois faz `seq_.store(s+1, release)`. Enquanto o
//     consumidor le o banco publicado, o produtor esta fisicamente no outro:
//     nao ha escrita e leitura no mesmo endereco dentro de um periodo do
//     produtor (~11,6 ms com AUDIO_CHUNK a 22050 Hz).
//   * O consumidor le `seq_`, copia, le `seq_` de novo e **exige que seja
//     igual**. Se o produtor deu duas voltas durante a copia (ou seja, se ele
//     chegou a tocar no banco que estavamos lendo), a desigualdade denuncia e
//     a copia e DESCARTADA.
//
// SE O CONSUMIDOR NAO ACOMPANHAR O PRODUTOR, O CERTO E PERDER QUADROS DE
// ANALISE. E o que acontece: `snapshot()` devolve false, `tick()` nao
// reanalisa, as barras continuam caindo pela balistica e o desenho segue vivo
// com o dado anterior. O produtor nunca espera, nunca toma mutex, nunca olha
// se alguem leu — `publish()` e uma copia e um store, e retorna. **Bloquear o
// audioTask para nao perder um quadro de analise seria trocar um defeito
// invisivel (uma barra desatualizada por 50 ms) por um audivel (um estalo).**
//
// Na pratica o produtor publica a ~86 Hz (um AUDIO_CHUNK = 256 quadros
// estereo = 11,6 ms) e o consumidor consome a 20 Hz: ~3 de cada 4 janelas sao
// descartadas de proposito, porque 20 Hz ja e mais rapido do que o olho
// distingue num mostrador desses.
//
// ---------------------------------------------------------------------------
// 2. POR QUE GOERTZEL E NAO FFT (com os numeros)
// ---------------------------------------------------------------------------
// Janela de N = 128 amostras mono a 22050 Hz = 5,8 ms de audio, resolucao de
// 172,3 Hz por bin.
//
//   Banco de Goertzel, 12 bandas, aritmetica de ponto fixo
//     media + janela (uma vez, in loco): 128 somas, 128 subtracoes,
//                                        128 mul e 128 div  = ~2.500 ciclos
//     recursao:                    12 x 128 = 1.536 iteracoes
//     por iteracao: 1 mul 32x32->64, 1 div por 2^14, 1 soma, 1 subtracao,
//                   ~16 instrucoes Xtensa LX6
//     -> 1.536 x 16 = 24.576 ciclos  +  ~2.500 de preparo  +  ~800 de
//        magnitude e log  = ~27.900 ciclos
//     -> **~116 us a 240 MHz**, a 20 Hz = 2,3 ms/s = **0,23% de um core**.
//     Memoria extra: **ZERO** (a recursao sao tres int32 em registrador).
//
//   FFT radix-2 de 128 pontos, int16, para comparar
//     (N/2)*log2(N) = 64 x 7 = 448 borboletas x ~22 instr = 9.856 ciclos
//     + inversao de bits (~600) + 64 magnitudes (~1.900)  = ~55 us.
//     Memoria: re[128] + im[128] em int16 = **512 B de SRAM** de rascunho.
//
// Ou seja, **a FFT e cerca de 2x mais rapida em ciclos** — e nao e por isso
// que ela perde. Perde por duas outras razoes:
//
//   a) 512 B de SRAM contra 0. Com heap livre medido em 28-38 KB (AGENTS.md
//      2.2), 512 B e ~1,5% do que sobra, gastos para produzir 64 bins lineares
//      dos quais 12 seriam usados.
//   b) **Bins lineares nao servem a um mostrador logaritmico.** Com 172,3 Hz
//      por bin, as cinco barras graves (177, 250, 354, 500, 707 Hz) cairiam
//      todas dentro dos bins 1 a 4 — quatro numeros para cinco barras, e a
//      metade grave do mostrador ficaria colada. O Goertzel avalia a DFT
//      EXATAMENTE na frequencia pedida (indice de bin fracionario), entao cada
//      barra tem o seu proprio filtro centrado onde deve.
//
// Os dois cabem folgadamente no orcamento (0,22% contra 0,13% de um core), o
// que torna ciclo o criterio errado. Decidiu-se por memoria e por correcao do
// mostrador.
//
// Limite honesto da escolha: com N = 128 e Hann, o lobulo principal tem +-345
// Hz. As bandas de meia oitava so ficam mais largas que isso acima de ~1,4 kHz,
// entao **abaixo disso as barras vizinhas se sobrepoem** — um tom de 1 kHz
// acende a barra de 1 kHz no topo e a de 707 Hz uns 22 dB abaixo. E o
// comportamento de um analisador de meia oitava de janela curta, e e o que
// qualquer aparelho da epoca fazia. Medido em sim/probes/audio_scope.cpp.
//
// ---------------------------------------------------------------------------
// 3. ORCAMENTO DE MEMORIA (o recurso escasso e a SRAM, AGENTS.md 2.2)
// ---------------------------------------------------------------------------
//   audioscope::Tap    banco duplo 2 x 128 x int16      512 B
//                      picos 2 x 2 x uint16               8 B
//                      seq_ + drops_ (atomicos)           8 B
//                                                      -------
//                                                       528 B
//   audioscope::Scope  work[128] int16 (copia local)    256 B
//                      prevTop/prevBot 128 x 2 uint8    256 B
//                      nivel/pico/desenhado 12 x 4       48 B
//                      estado do VU                       8 B
//                      modo, posicao, prazos, contadores  56 B
//                                                      -------
//                                                       624 B
//   TOTAL DE SRAM                                      1.152 B  (~1,1 KB)
//
// Os valores exatos de sizeof() sao impressos pelo probe. **Nenhum
// LGFX_Sprite e alocado** — um sprite de 5 KB ja causou falta de memoria
// neste projeto. Todo o desenho e fillRect direto no destino.
//
// Em FLASH (.rodata, `static const` dentro de funcao inline, uma copia por
// binario): janela de Hann 128 x int16 = 256 B, coeficientes 12 x int16 =
// 24 B. Total 280 B de flash, 0 B de SRAM.
//
// ---------------------------------------------------------------------------
// 4. PONTO FIXO: A ARMADILHA DO DESLOCAMENTO
// ---------------------------------------------------------------------------
// `>> k` num valor COM SINAL arredonda para -infinito, nao para zero. Num
// deslocamento de 8 bits isso custa 1 LSB por amostra negativa e injeta offset
// DC — foi exatamente o que aconteceu com o gerador de 1 kHz deste repositorio
// (-218 por periodo, -10.900 por segundo).
//
// Por isso **nao ha um unico `>>` sobre valor com sinal neste arquivo**. Onde
// o denominador e potencia de dois, esta escrito como DIVISAO:
//
//     soma / N                             nivel medio do bloco
//     (int32_t(x) * w[n]) / 32768          janelamento
//     ((int64_t)coef * s1) / 16384         recursao do Goertzel
//     (int32_t(v) * HALF) / 32768          amostra -> linha do osciloscopio
//     (int32_t(l) + int32_t(r)) / 2        mistura L+R do produtor
//
// A divisao em C++ trunca em direcao ao ZERO, que e simetrica: o erro medio
// sobre um sinal simetrico e zero, sem DC. O compilador gera deslocamento mais
// correcao de sinal (2 a 3 instrucoes a mais), preco que cabe no orcamento
// acima. Os unicos `>>` do arquivo estao em `log2Q8`, sobre `uint64_t` e
// `uint32_t` — sem sinal, sem armadilha.
//
// O probe confere isso diretamente: soma das amostras janeladas de um seno
// puro, e a banda DC do resultado.
//
// ---------------------------------------------------------------------------
// 5. CORES E A TV
// ---------------------------------------------------------------------------
//   * **O LovyanGFX escolhe o formato pelo TIPO do argumento.** `uint16_t`,
//     `int16_t` e `int32_t` viram RGB565; **`uint32_t` vira RGB888**
//     (misc/colortype.hpp:861-866). Um `(uint32_t)0xBDF7` sai como
//     R=0,G=189,B=247. Toda cor aqui e `uint16_t` e as funcoes recebem
//     `uint16_t` — nao troque por `uint32_t` nem por `auto`.
//   * **Nada de ciano saturado (TFT_CYAN / 0x07FF).** O croma do NTSC faz o
//     ciano fervilhar, e fervilha PIOR justamente em grafico brilhante que se
//     mexe — que e este. O acento seguro do projeto e 0x96BC.
//   * Contraste por LUMINANCIA, nao por saturacao: verde de fosforo no
//     osciloscopio, ambar nas barras, pontos de pico em BRANCO (croma zero,
//     nunca fervilha), reguas em cinza (croma zero tambem).
//   * Fundo do painel PRETO, de proposito: alem de parecer a janela de um
//     aparelho de som, pixel apagado nao marca o tubo.
//
// ---------------------------------------------------------------------------
// 6. REPINTURA: NENHUM PIXEL QUE NAO MUDOU E REESCRITO
// ---------------------------------------------------------------------------
// Apagar e repintar o painel inteiro a 20 Hz num tubo pisca de doer. Aqui cada
// modo repinta so o DELTA:
//
//   BARS   por barra: se subiu, pinta so as linhas ganhas; se desceu, apaga so
//          as linhas perdidas. A sobreposicao nao e tocada. O ponto de pico
//          apaga a posicao antiga (2 linhas) e pinta a nova (2 linhas).
//          Barra parada = zero pixels escritos.
//   SCOPE  por coluna guarda o intervalo [topo, base] desenhado. Na atualizacao
//          apaga so as partes do intervalo VELHO que estao fora do NOVO, e
//          pinta so as partes do NOVO fora do VELHO.
//   VU     so o trecho ganho ou perdido de cada agulha, mais os dois marcadores
//          de pico.
//
// `Scope::dirty` guarda o retangulo envolvente (em coordenadas do quadro) do
// que a ultima chamada a `tick()` repintou, e `Scope::touched` o numero de
// pixels escritos — para quem precisar compor por cima, e para o probe medir.
//
// ---------------------------------------------------------------------------
// 7. RESTRICOES DO PROJETO RESPEITADAS AQUI
// ---------------------------------------------------------------------------
//   * **C++11** (`-std=gnu++11`): todo `constexpr` daqui e um unico `return`,
//     sem laco e sem variavel local. Conferir com `make -C sim cxx11`.
//   * Prazos com `timeReached()` (UiLogic.h) — `millis()` da a volta.
//   * Tudo dentro da area segura (SafeArea.h); o painel padrao ocupa
//     x [31,289) e y [99,149), contra a caixa segura [24,296) x [18,222).
//   * Texto ASCII sem acento.
//   * Entrada de desenho `(lgfx::LovyanGFX *dst, int ox, int oy, ...)`, como
//     todos os pintores do projeto: o mesmo codigo serve ao LCD e ao CVBS.
//   * Estado no CHAMADOR (`Scope`), nunca em `static` de funcao — a armadilha
//     6 do AGENTS.md e justamente estado herdado da visita anterior. Ao entrar
//     na tela, chame `reset()`.
//
// Dependencias: SafeArea.h, VcrFont.h (que traz a LovyanGFX) e UiLogic.h. Nada de
// Arduino, FreeRTOS, SD ou I2S: este header NAO toca no barramento de audio,
// so recebe PCM de quem e dono dele.
// ============================================================================

#include "SafeArea.h"
#include "UiLogic.h" // timeReached(): millis() da a volta em ~49 dias
#include "VcrFont.h" // traz fj/Gfx.h (LovyanGFX) e a fonte grossa 12x16

#include <atomic>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace audioscope {

// ===========================================================================
//  1. Cores
// ===========================================================================
//
// RGB888 -> RGB565 por truncamento, igual ao color565() do M5GFX. Um unico
// `return` para continuar valendo em C++11.
constexpr uint16_t rgb565(int r, int g, int b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xF8) >> 3));
}

// Fundo da janela do visualizador. Preto tambem por causa do tubo.
constexpr uint16_t BG = 0x0000;
// Moldura e reguas: cinza medio, croma ZERO, nao fervilha.
constexpr uint16_t FRAME = 0x8410;
// Verde de fosforo P1, dessaturado de proposito (R=48 em vez de 0): mesma
// leitura de "verde de osciloscopio" com bem menos croma que o TFT_GREEN.
constexpr uint16_t TRACE = rgb565(48, 255, 96);
// Ambar de mostrador de som. Luminancia alta, que e o que da contraste no tubo.
constexpr uint16_t BAR = rgb565(255, 176, 0);
// Ponto de pico e agulha do VU em branco: croma zero, nunca fervilha.
constexpr uint16_t PEAK = 0xFFFF;
// Acento seguro do projeto (main.cpp RCA_ACCENT). NUNCA 0x07FF.
constexpr uint16_t ACCENT = 0x96BC;

// ===========================================================================
//  2. Geometria
// ===========================================================================
//
// Coordenadas abaixo sao LOCAIS ao painel; a origem do painel no quadro esta
// em Scope::x / Scope::y, que o integrador pode mover. Duas constantes
// descrevendo a mesma geometria e a armadilha 8 do AGENTS.md, entao tudo aqui
// deriva de N, BANDS e PANEL_H.

constexpr uint32_t SAMPLE_RATE = 22050; // igual ao `sampleRate` do main.cpp
constexpr int N = 128;                  // amostras mono por janela
constexpr int BANDS = 12;               // barras do espectro

// 128 colunas de 2 px = 256 px, que e a largura do painel: uma coluna por
// amostra, sem decimacao e sem interpolacao.
constexpr int COL_W = 2;
constexpr int PANEL_W = N * COL_W; // 256
constexpr int PANEL_H = 48;

// Posicao padrao: centrada na caixa segura. 24 + (272-256)/2 = 32, e a moldura
// de 1 px fica em 31 — ainda dentro de SAFE_L.
constexpr int DEFAULT_X = crt::SAFE_L + (crt::SAFE_W - PANEL_W) / 2; // 32
constexpr int DEFAULT_Y = 100;

// AVISO AO INTEGRADOR: com y = 100 o painel ocupa as linhas 99 a 148, que na
// MUSIC_NOW_PLAYING de hoje sao da capa do album (24..112 x 62..150) e da
// metade de baixo dos metadados. Essa tela esta cheia: header ate 46, capa e
// tags 62..150, barra de progresso 160..166, relogio 174..182, e a base da
// area segura em 222. Nao sobram 50 linhas livres em lugar nenhum — abrir
// espaco e decisao de quem integra (encolher a capa de 88 para 64 px ja
// libera as linhas 96..150, por exemplo). Mova o painel escrevendo Scope::x /
// Scope::y; toda a geometria deste arquivo e relativa a esse canto.

// Barras: 12 x 19 + 11 x 2 = 250, sobrando 3 px de folga a esquerda.
constexpr int BAR_W = 19;
constexpr int BAR_GAP = 2;
constexpr int BAR_PAD = (PANEL_W - (BANDS * BAR_W + (BANDS - 1) * BAR_GAP)) / 2; // 3
constexpr int BASE_Y = PANEL_H - 1; // 47, linha de base das barras
constexpr int BAR_MAX = 44;         // altura maxima; topo em 47-44+1 = 4
constexpr int PEAK_H = 2;           // espessura do ponto de pico

// Osciloscopio: linha central e meia amplitude.
constexpr int SCOPE_MID = PANEL_H / 2;     // 24
constexpr int SCOPE_HALF = PANEL_H / 2 - 1; // 23

// VU: duas agulhas horizontais com rotulo de uma celula da VcrFont.
constexpr int VU_LABEL_W = vcrfont::CELL_W; // 12
constexpr int VU_X = VU_LABEL_W + 4;        // 16
constexpr int VU_LEN = PANEL_W - VU_X - 4;  // 236
constexpr int VU_H = 14;
constexpr int VU_Y_L = 5;
constexpr int VU_Y_R = 27;

// Cadencia do visualizador. 20 Hz ja parece vivo e nao precisa de todo quadro:
// a saida composta sustenta ~23,5 fps, entao isto e ~1 quadro em cada 2.
constexpr uint32_t UPDATE_MS = 50;
// Sem PCM novo por este tempo (pausa, fim da faixa, radio reconectando), o
// mostrador decai ate o repouso em vez de congelar numa foto.
constexpr uint32_t STALE_MS = 250;

// Balistica, em pixels por atualizacao (20 Hz). Ataque instantaneo e queda
// lenta e o comportamento classico; o ponto de pico cai mais devagar ainda.
constexpr int BAR_FALL = 4;  // 44 px em ~0,55 s
constexpr int PEAK_FALL = 1; // 44 px em ~2,2 s
constexpr int VU_FALL = 24;  // 236 px em ~0,5 s (agulha de VU de verdade)

// Janela do mostrador do espectro, em bits de log2 da potencia. A referencia
// vem de um seno de fundo de escala na banda: |X| = (A/2) * soma(w) =
// (32767/2) * 64 = 1,049e6, potencia 1,099e12, log2 = 40,00 — conferido pelo
// probe. 8 bits de janela = 48,2 dB, que e o alcance de um mostrador de som.
constexpr int32_t REF_LOG2Q8 = 40 * 256;   // 10240
constexpr int32_t RANGE_LOG2Q8 = 8 * 256;  // 2048
// VU: referencia e o pico de fundo de escala ao quadrado (32767^2, log2 = 30)
// e a janela e 6 bits = 36,1 dB.
constexpr int32_t VU_REF_LOG2Q8 = 30 * 256;  // 7680
constexpr int32_t VU_RANGE_LOG2Q8 = 6 * 256; // 1536

struct Rect {
  int16_t x, y, w, h;
};

// ===========================================================================
//  3. Modos
// ===========================================================================

enum Mode : uint8_t { SCOPE = 0, BARS = 1, VU = 2, MODE_COUNT = 3 };

inline Mode nextMode(Mode m) {
  return (Mode)((uint8_t)(m + 1) % (uint8_t)MODE_COUNT);
}

// ASCII sem acento (AGENTS.md 2.7).
inline const char *modeName(Mode m) {
  return m == SCOPE ? "OSCILOSCOPIO" : m == BARS ? "ESPECTRO" : "VU";
}

// ===========================================================================
//  4. Tap — o canal entre o core 0 e o core 1
// ===========================================================================
//
// Ver a secao 1 do cabecalho. Resumo do contrato:
//
//   * `publish()` so pode ser chamado pelo audioTask (core 0). Custo: uma
//     passada de N amostras (copia + mistura + pico) e um store atomico.
//     Medido em ~128 iteracoes de ~8 instrucoes = ~1.000 ciclos = **4,3 us**,
//     contra os 11,6 ms de audio que o bloco representa: **0,04% do
//     audioTask**. Nao ha mutex, nao ha alocacao, nao ha espera.
//   * `snapshot()` so pode ser chamado pelo consumidor (core 1). Se devolver
//     false, o consumidor PERDE esse quadro de analise, que e o
//     comportamento correto sob sobrecarga.
class Tap {
public:
  // Chamado pelo audioTask com o PCM que acabou de ir para o I2S — ou seja,
  // DEPOIS do playback::scalePcm(), para que o mostrador reflita o que se
  // ouve, e nao o que estava no arquivo.
  //
  //   pcm      buffer intercalado (L,R,L,R...) ou mono
  //   shorts   quantidade de int16_t validos em `pcm`
  //   channels 1 ou 2
  //
  // Devolve false e NAO publica se o bloco tiver menos de N quadros — assim a
  // janela publicada esta sempre cheia, sem zeros no final que sujariam o
  // espectro. Um AUDIO_CHUNK de 1024 B da 256 quadros estereo, o dobro do
  // necessario, entao o caso comum sempre publica.
  bool publish(const int16_t *pcm, size_t shorts, int channels) {
    if (!pcm || channels < 1 || channels > 2)
      return false;
    const size_t frames = (channels == 2) ? (shorts / 2) : shorts;
    if (frames < (size_t)N)
      return false;

    const uint32_t s = seq_.load(std::memory_order_relaxed);
    const int w = (int)((s + 1) & 1u); // banco NAO publicado: o consumidor
                                       // nunca esta lendo este endereco
    int16_t *dst = buf_[w];
    uint32_t pl = 0, pr = 0;

    if (channels == 2) {
      for (int n = 0; n < N; ++n) {
        const int32_t l = pcm[2 * n], r = pcm[2 * n + 1];
        // DIVISAO, nao `>> 1`: `>>` arredondaria para -infinito e injetaria
        // offset DC de meio LSB por amostra (ver secao 4 do cabecalho).
        dst[n] = (int16_t)((l + r) / 2);
        const uint32_t al = (uint32_t)(l < 0 ? -l : l);
        const uint32_t ar = (uint32_t)(r < 0 ? -r : r);
        if (al > pl)
          pl = al;
        if (ar > pr)
          pr = ar;
      }
    } else {
      for (int n = 0; n < N; ++n) {
        const int32_t v = pcm[n];
        dst[n] = (int16_t)v;
        const uint32_t a = (uint32_t)(v < 0 ? -v : v);
        if (a > pl)
          pl = a;
      }
      pr = pl;
    }
    // uint16_t e nao int16_t: |-32768| = 32768 nao cabe em int16_t.
    peak_[w][0] = (uint16_t)pl;
    peak_[w][1] = (uint16_t)pr;
    // Publica. `release` garante que a janela acima ficou visivel ANTES do
    // contador novo para quem ler com `acquire` no outro core.
    seq_.store(s + 1, std::memory_order_release);
    return true;
  }

  // Zera a janela publicada. Util quando quem integra sabe que o audio parou
  // (STOP, troca de faixa) e quer o mostrador caindo ja, sem esperar os
  // STALE_MS. Continua sendo uma copia e um store.
  void publishSilence() {
    const uint32_t s = seq_.load(std::memory_order_relaxed);
    const int w = (int)((s + 1) & 1u);
    memset(buf_[w], 0, sizeof(buf_[w]));
    peak_[w][0] = 0;
    peak_[w][1] = 0;
    seq_.store(s + 1, std::memory_order_release);
  }

  // Consumidor. Copia a janela publicada para `dst` (N int16_t) e os picos.
  // Devolve false quando nada foi publicado ainda ou quando o produtor deu
  // duas voltas durante a copia — nesse caso o quadro de analise e perdido de
  // proposito. NUNCA bloqueia e NUNCA faz o produtor esperar.
  bool snapshot(int16_t *dst, uint16_t &peakL, uint16_t &peakR, uint32_t &seq) const {
    const uint32_t a = seq_.load(std::memory_order_acquire);
    if (!a)
      return false; // nada publicado desde o reset
    const int b = (int)(a & 1u);
    memcpy(dst, buf_[b], sizeof(buf_[b]));
    peakL = peak_[b][0];
    peakR = peak_[b][1];
    // Se `seq_` andou, o produtor pode ter comecado a escrever justamente
    // neste banco: a copia esta suspeita e vai fora.
    if (seq_.load(std::memory_order_acquire) != a) {
      drops_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    seq = a;
    return true;
  }

  uint32_t published() const { return seq_.load(std::memory_order_relaxed); }
  uint32_t dropped() const { return drops_.load(std::memory_order_relaxed); }

  // So no reset da tela, com o audio parado.
  void clear() {
    memset(buf_, 0, sizeof(buf_));
    memset(peak_, 0, sizeof(peak_));
    seq_.store(0, std::memory_order_release);
    drops_.store(0, std::memory_order_relaxed);
  }

private:
  int16_t buf_[2][N];    // 512 B — banco duplo
  uint16_t peak_[2][2];  //   8 B — [banco][L,R]
  std::atomic<uint32_t> seq_{0};           // 4 B
  mutable std::atomic<uint32_t> drops_{0}; // 4 B
};

// ===========================================================================
//  5. Tabelas (FLASH, nao SRAM)
// ===========================================================================

namespace detail {

// Janela de Hann PERIODICA, w[n] = 0,5*(1-cos(2*pi*n/N)), em Q15.
// Periodica e nao simetrica porque a soma fecha em N/2 = 64 exato, que e o que
// faz a referencia de fundo de escala dar log2 = 40,00 redondo.
// 128 x 2 = 256 B de .rodata. `static const` dentro de funcao inline: uma copia
// por binario, mesmo com o header incluido em varias unidades.
inline const int16_t *hann() {
  static const int16_t kW[N] = {
          0,    20,    79,   177,   315,   491,   705,   958,
       1247,  1573,  1935,  2331,  2761,  3224,  3719,  4244,
       4799,  5381,  5990,  6624,  7281,  7961,  8660,  9379,
      10114, 10864, 11628, 12403, 13187, 13980, 14778, 15580,
      16383, 17187, 17989, 18787, 19580, 20364, 21139, 21903,
      22653, 23388, 24107, 24806, 25486, 26143, 26777, 27386,
      27968, 28523, 29048, 29543, 30006, 30436, 30832, 31194,
      31520, 31809, 32062, 32276, 32452, 32590, 32688, 32747,
      32767, 32747, 32688, 32590, 32452, 32276, 32062, 31809,
      31520, 31194, 30832, 30436, 30006, 29543, 29048, 28523,
      27968, 27386, 26777, 26143, 25486, 24806, 24107, 23388,
      22653, 21903, 21139, 20364, 19580, 18787, 17989, 17187,
      16384, 15580, 14778, 13980, 13187, 12403, 11628, 10864,
      10114,  9379,  8660,  7961,  7281,  6624,  5990,  5381,
       4799,  4244,  3719,  3224,  2761,  2331,  1935,  1573,
       1247,   958,   705,   491,   315,   177,    79,    20,
  };
  return kW;
}

// Centros das 12 bandas: f_k = 125 * 2^((k+1)/2), ou seja MEIA OITAVA, de
// 176,78 Hz a 8000 Hz. A razao sqrt(2) foi escolhida por duas razoes praticas:
// cobre 6 oitavas com 12 barras, e faz 1000 Hz cair EXATAMENTE num centro, o
// que torna o teste do tom de referencia do probe uma medida e nao um chute.
inline const int16_t *bandCoeff() {
  // 2*cos(2*pi*f/22050) em Q14. O maior, 32726, ainda cabe em int16_t — por
  // isso a tabela e int16 e nao int32 (12 x 2 = 24 B de flash).
  static const int16_t kC[BANDS] = {
      32726,  //   176,78 Hz
      32685,  //   250,00 Hz
      32602,  //   353,55 Hz
      32436,  //   500,00 Hz
      32105,  //   707,11 Hz
      31447,  //  1000,00 Hz
      30143,  //  1414,21 Hz
      27589,  //  2000,00 Hz
      22689,  //  2828,43 Hz
      13689,  //  4000,00 Hz
      -1347,  //  5656,85 Hz
      -21330, //  8000,00 Hz
  };
  return kC;
}

// Frequencia central de cada banda em Hz, so para rotulo e para o probe.
inline uint16_t bandHz(int b) {
  static const uint16_t kF[BANDS] = {177, 250, 354, 500, 707, 1000, 1414, 2000, 2828, 4000, 5657, 8000};
  return kF[(b >= 0 && b < BANDS) ? b : 0];
}

// log2(v) * 256, para v >= 1. Expoente exato pelo contador de zeros a esquerda
// e mantissa por interpolacao LINEAR (log2(1+f) ~ f), cujo erro maximo e 0,0861
// bit = **0,52 dB** — meio pixel numa barra de 44 px sobre 48 dB de janela.
// Nao vale pagar por uma aproximacao melhor.
//
// Os `>>` daqui sao sobre `uint64_t`/`uint32_t`: sem sinal, sem a armadilha do
// arredondamento para -infinito.
inline int32_t log2Q8(uint64_t v) {
  if (v < 2)
    return 0;
#if defined(__GNUC__)
  const int e = 63 - __builtin_clzll(v);
#else
  int e = 0;
  for (uint64_t t = v; t > 1; t >>= 1)
    ++e;
#endif
  // Mantissa normalizada para [2^16, 2^17).
  const uint32_t m = (uint32_t)((e >= 16) ? (v >> (e - 16)) : (v << (16 - e)));
  return (int32_t)(e * 256) + (int32_t)((m - 65536u) >> 8);
}

// Mapeia log2(potencia)*256 para altura em pixels, dentro da janela do
// mostrador. Fora da janela satura em 0 ou em `full`.
inline int logToPixels(int32_t lq8, int32_t refQ8, int32_t rangeQ8, int full) {
  const int32_t floorQ8 = refQ8 - rangeQ8;
  if (lq8 <= floorQ8)
    return 0;
  const int32_t h = (lq8 - floorQ8) * (int32_t)full / rangeQ8;
  return h >= full ? full : (int)h;
}

} // namespace detail

// ===========================================================================
//  6. Estado do visualizador (mora no CHAMADOR)
// ===========================================================================
//
// Nada de `static` de funcao: a armadilha 6 do AGENTS.md e exatamente estado
// herdado da visita anterior a uma tela. Ao entrar em MUSIC_NOW_PLAYING ou
// RADIO, chame `reset()`.
struct Scope {
  // --- posicao do painel no quadro (o integrador pode mover) ---
  int16_t x = DEFAULT_X;
  int16_t y = DEFAULT_Y;

  Mode mode = BARS;

  // --- janela copiada do Tap e resultado da analise ---
  int16_t work[N] = {}; // 256 B
  uint16_t peakL = 0, peakR = 0;

  // --- espectro: alvo, nivel com balistica, pico, e o que esta na tela ---
  uint8_t target[BANDS] = {};
  uint8_t level[BANDS] = {};
  uint8_t peak[BANDS] = {};
  uint8_t drawnLevel[BANDS] = {};
  uint8_t drawnPeak[BANDS] = {};

  // --- osciloscopio: intervalo [topo, base] desenhado em cada coluna ---
  // Inicializados com base < topo em reset(): "coluna ainda sem traco".
  uint8_t prevTop[N] = {};
  uint8_t prevBot[N] = {};

  // --- VU ---
  uint8_t vuL = 0, vuR = 0, vuPeakL = 0, vuPeakR = 0;
  uint8_t drawnVuL = 0, drawnVuR = 0, drawnVuPeakL = 0, drawnVuPeakR = 0;

  // --- controle ---
  bool painted = false;   // o fundo estatico ja foi desenhado neste modo?
  uint32_t nextAt = 0;    // proximo prazo de atualizacao
  uint32_t lastDataAt = 0;// ultima vez que chegou janela nova
  uint32_t lastSeq = 0;   // ultima sequencia consumida
  uint32_t frames = 0;    // quadros de analise efetivamente processados
  uint32_t starved = 0;   // prazos em que nao havia dado novo (perda proposital)

  // --- resultado da ultima repintura ---
  Rect dirty = {0, 0, 0, 0};
  uint32_t touched = 0;
};

inline void reset(Scope &s) {
  // Preserva so o que e configuracao (posicao e modo escolhido) e zera todo o
  // resto. Todos os membros do Scope tem inicializador, entao Scope() e
  // completamente definido — nao ha array com lixo.
  const int16_t px = s.x, py = s.y;
  const Mode m = s.mode;
  s = Scope();
  s.x = px;
  s.y = py;
  s.mode = m;
  // Sem coluna desenhada ainda: intervalo vazio e marcado por base < topo.
  for (int i = 0; i < N; ++i) {
    s.prevTop[i] = 1;
    s.prevBot[i] = 0;
  }
}

// Troca de modo. Forca o redesenho do fundo estatico e esquece o que estava na
// tela, porque o modo anterior deixou pixels que nao sao deste.
inline void setMode(Scope &s, Mode m) {
  s.mode = m;
  s.painted = false;
  memset(s.drawnLevel, 0, sizeof(s.drawnLevel));
  memset(s.drawnPeak, 0, sizeof(s.drawnPeak));
  s.drawnVuL = s.drawnVuR = s.drawnVuPeakL = s.drawnVuPeakR = 0;
  // `work` pode conter dado JANELADO (analyzeSpectrum destroi a onda in loco);
  // o osciloscopio desenharia isso ate a proxima janela chegar.
  memset(s.work, 0, sizeof(s.work));
  for (int i = 0; i < N; ++i) {
    s.prevTop[i] = 1;
    s.prevBot[i] = 0;
  }
}

inline void cycleMode(Scope &s) { setMode(s, nextMode(s.mode)); }

// Retangulo do painel no quadro, incluindo a moldura de 1 px. Serve a quem
// precisa reservar espaco na tela.
inline Rect panelRect(const Scope &s) {
  return Rect{(int16_t)(s.x - 1), (int16_t)(s.y - 1), (int16_t)(PANEL_W + 2), (int16_t)(PANEL_H + 2)};
}

// Envolvente do que a ultima chamada a tick() repintou. w == 0 quer dizer que
// nada mudou.
inline Rect dirtyRect(const Scope &s) { return s.dirty; }

// ===========================================================================
//  7. Analise (core 1)
// ===========================================================================

namespace detail {

// Preparo da janela, IN LOCO em s.work: remove o nivel medio e aplica Hann.
// Feito uma vez e reaproveitado pelas 12 recursoes — e o que evita 11 x 128
// multiplicacoes redundantes.
//
// POR QUE REMOVER A MEDIA. Janelar um bloco com offset DC nao produz "energia
// em 0 Hz" (nao ha banda em 0 Hz): produz o ESPECTRO DA PROPRIA JANELA, cujo
// lobulo principal cai justamente em cima das bandas graves. Medido pelo
// probe antes desta correcao: uma entrada constante de +10000 (DC puro,
// nenhum som) levantava a barra de 177 Hz a 23 px de 44, e um tom de 1 kHz a
// -12 dBFS com +9000 de offset era DOMINADO pelo offset — a barra de 177 Hz
// ficava acima da de 1 kHz. Nao e hipotese de laboratorio: este repositorio
// acabou de descobrir um gerador de tom que injetava -10.900 de DC por
// segundo, e um WAV mal produzido faz o mesmo.
//
// QUAL MEDIA. Subtrair a media ARITMETICA do bloco nao serve e chega a
// PIORAR: a janela pesa as amostras de forma desigual, entao o termo DC do
// bloco janelado e a media PONDERADA PELA JANELA. Medido no mesmo sinal de
// 1 kHz de fundo de escala, energia da banda de 177 Hz relativa ao pico:
//
//     sem remocao alguma .................. -54,4 dB
//     media aritmetica .................... -34,7 dB   (piorou 20 dB)
//     media ponderada pela janela ......... -52,7 dB   (correto)
//
// e, com +9000 de offset grudado no mesmo tom, -0,0 dB / -34,3 dB / -52,3 dB.
// So a terceira le o tom em vez de ler o offset.
//
// COMO, SEM DIVISAO DE 64 BITS. A media ponderada e soma(x*h)/soma(h). A
// primeira passada ja calcula y[n] = x[n]*h[n]/32768, entao basta somar esses
// y (cabe em int32: 128 * 32767 = 4,2e6) e dividir por soma(h)/32768 =
// 2.097.088/32768 = 63,998, arredondado para 64 — o resto de 0,003% fica 28
// bits abaixo do piso do mostrador. Divisao por 64 de inteiro de 32 bits, e
// nao __divdi3.
//
// Custo: uma segunda passada de 128 multiplicacoes, ~1.500 ciclos, ~6 us.
//
// A saturacao no fim importa: com media grande e amostra no extremo a
// subtracao passaria de 16 bits. Com audio de verdade nada satura; o limite so
// garante que o |x| <= 32767 assumido pela analise de estouro do Goertzel
// continua valendo.
//
// `/ 64` e `/ 32768` e nao `>> 6` / `>> 15`: divisao trunca para ZERO, que e
// simetrica. O deslocamento arredondaria para -infinito e injetaria
// exatamente o DC que esta funcao existe para tirar (secao 4 do cabecalho).
inline void applyWindow(int16_t *w128) {
  const int16_t *h = hann();
  int32_t sum = 0;
  for (int n = 0; n < N; ++n) {
    const int32_t y = ((int32_t)w128[n] * h[n]) / 32768;
    w128[n] = (int16_t)y;
    sum += y;
  }
  // Media ponderada pela janela, em unidades de amostra. soma(h)/32768 = 64.
  const int32_t mean = sum / 64;
  if (!mean)
    return; // caso comum com audio de verdade: nada a corrigir
  for (int n = 0; n < N; ++n) {
    int32_t v = (int32_t)w128[n] - (mean * h[n]) / 32768;
    if (v > 32767)
      v = 32767;
    else if (v < -32768)
      v = -32768;
    w128[n] = (int16_t)v;
  }
}

// Goertzel generalizado numa banda, sobre dado JA janelado.
//
// Limites (conferidos pelo probe, nao estimados): com entrada de fundo de
// escala o maior |s| observado sobre todas as bandas e fases e 2,1e7, contra
// 2,1e9 de int32_t — 100x de folga, nao ha estouro. O produto coef*s1 chega a
// 2^39 e por isso e feito em int64_t; a potencia chega a 2^50 e tambem.
inline int64_t goertzelPower(const int16_t *w128, int32_t coef) {
  int32_t s1 = 0, s2 = 0;
  for (int n = 0; n < N; ++n) {
    // DIVISAO por 16384, nao `>> 14`. Ver secao 4 do cabecalho.
    const int32_t s0 = (int32_t)w128[n] + (int32_t)(((int64_t)coef * s1) / 16384) - s2;
    s2 = s1;
    s1 = s0;
  }
  const int64_t cs = ((int64_t)coef * s1) / 16384;
  const int64_t p = (int64_t)s1 * s1 + (int64_t)s2 * s2 - cs * s2;
  return p < 0 ? 0 : p; // matematicamente >= 0; a guarda e contra o
                        // arredondamento do ponto fixo perto do zero
}

} // namespace detail

// Roda o banco de filtros sobre s.work e preenche s.target em pixels.
// DESTROI s.work (janela aplicada in loco) — por isso o osciloscopio le
// s.work ANTES, e por isso o modo SCOPE nao chama esta funcao.
inline void analyzeSpectrum(Scope &s) {
  detail::applyWindow(s.work);
  const int16_t *c = detail::bandCoeff();
  for (int b = 0; b < BANDS; ++b) {
    const int64_t p = detail::goertzelPower(s.work, c[b]);
    s.target[b] = (uint8_t)detail::logToPixels(detail::log2Q8((uint64_t)p), REF_LOG2Q8, RANGE_LOG2Q8, BAR_MAX);
  }
}

// Potencia bruta de uma banda, sem mapeamento para pixel. So o probe usa.
inline int64_t bandPower(const int16_t *windowed, int band) {
  return detail::goertzelPower(windowed, detail::bandCoeff()[(band >= 0 && band < BANDS) ? band : 0]);
}

// ===========================================================================
//  8. Desenho
// ===========================================================================

namespace detail {

// Unico ponto por onde passa pixel pintado: mantem `dirty` e `touched` certos
// sem espalhar contabilidade pelo arquivo.
//
// A cor e `uint16_t` de proposito: o LovyanGFX le `uint32_t` como RGB888 e a
// cor sairia errada em silencio (ver secao 5 do cabecalho).
inline void paint(lgfx::LovyanGFX *dst, int ox, int oy, Scope &s, int x, int y, int w, int h,
                  uint16_t color) {
  if (w <= 0 || h <= 0)
    return;
  dst->fillRect(ox + s.x + x, oy + s.y + y, w, h, color);
  s.touched += (uint32_t)w * (uint32_t)h;
  const int16_t fx = (int16_t)(s.x + x), fy = (int16_t)(s.y + y);
  if (!s.dirty.w) {
    s.dirty.x = fx;
    s.dirty.y = fy;
    s.dirty.w = (int16_t)w;
    s.dirty.h = (int16_t)h;
    return;
  }
  const int16_t x0 = fx < s.dirty.x ? fx : s.dirty.x;
  const int16_t y0 = fy < s.dirty.y ? fy : s.dirty.y;
  const int16_t x1 = (int16_t)((fx + w) > (s.dirty.x + s.dirty.w) ? (fx + w) : (s.dirty.x + s.dirty.w));
  const int16_t y1 = (int16_t)((fy + h) > (s.dirty.y + s.dirty.h) ? (fy + h) : (s.dirty.y + s.dirty.h));
  s.dirty.x = x0;
  s.dirty.y = y0;
  s.dirty.w = (int16_t)(x1 - x0);
  s.dirty.h = (int16_t)(y1 - y0);
}

// Atualiza um intervalo vertical numa coluna sem repintar a sobreposicao:
// apaga so [velho \ novo] e pinta so [novo \ velho]. Intervalo vazio e
// sinalizado por bot < top.
inline void spanUpdate(lgfx::LovyanGFX *dst, int ox, int oy, Scope &s, int x, int w, int oldTop,
                       int oldBot, int newTop, int newBot, uint16_t ink) {
  const bool hadOld = oldBot >= oldTop;
  const bool hasNew = newBot >= newTop;
  if (!hadOld) {
    if (hasNew)
      paint(dst, ox, oy, s, x, newTop, w, newBot - newTop + 1, ink);
    return;
  }
  if (!hasNew) {
    paint(dst, ox, oy, s, x, oldTop, w, oldBot - oldTop + 1, BG);
    return;
  }
  if (oldBot < newTop || newBot < oldTop) { // sem sobreposicao
    paint(dst, ox, oy, s, x, oldTop, w, oldBot - oldTop + 1, BG);
    paint(dst, ox, oy, s, x, newTop, w, newBot - newTop + 1, ink);
    return;
  }
  // Apaga as sobras do intervalo velho.
  if (oldTop < newTop)
    paint(dst, ox, oy, s, x, oldTop, w, newTop - oldTop, BG);
  if (oldBot > newBot)
    paint(dst, ox, oy, s, x, newBot + 1, w, oldBot - newBot, BG);
  // Pinta so o que o intervalo novo ganhou.
  if (newTop < oldTop)
    paint(dst, ox, oy, s, x, newTop, w, oldTop - newTop, ink);
  if (newBot > oldBot)
    paint(dst, ox, oy, s, x, oldBot + 1, w, newBot - oldBot, ink);
}

// Barra vertical crescendo da base: so o delta e escrito.
inline void barUpdate(lgfx::LovyanGFX *dst, int ox, int oy, Scope &s, int x, int w, int oldH, int newH,
                      uint16_t ink) {
  if (newH > oldH)
    paint(dst, ox, oy, s, x, BASE_Y - newH + 1, w, newH - oldH, ink);
  else if (newH < oldH)
    paint(dst, ox, oy, s, x, BASE_Y - oldH + 1, w, oldH - newH, BG);
}

// Agulha horizontal crescendo da esquerda: idem.
inline void needleUpdate(lgfx::LovyanGFX *dst, int ox, int oy, Scope &s, int y, int h, int oldL, int newL,
                         uint16_t ink) {
  if (newL > oldL)
    paint(dst, ox, oy, s, VU_X + oldL, y, newL - oldL, h, ink);
  else if (newL < oldL)
    paint(dst, ox, oy, s, VU_X + newL, y, oldL - newL, h, BG);
}

inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

} // namespace detail

// Fundo estatico do modo corrente: limpa o painel, desenha a moldura e o que
// nao muda (reguas, rotulos). Chamado por tick() sempre que `painted` e falso,
// ou seja no primeiro quadro e a cada troca de modo. O integrador tambem pode
// chamar diretamente depois de repintar a tela inteira por baixo.
inline void drawStatic(lgfx::LovyanGFX *dst, int ox, int oy, Scope &s) {
  const int px = ox + s.x, py = oy + s.y;
  dst->fillRect(px, py, PANEL_W, PANEL_H, BG);
  dst->drawRect(px - 1, py - 1, PANEL_W + 2, PANEL_H + 2, FRAME);

  if (s.mode == SCOPE) {
    // Marcas da linha central nas bordas, fora das colunas do traco, para que
    // apagar o traco nunca precise repintar regua nenhuma.
    dst->drawFastHLine(px, py + SCOPE_MID, 3, FRAME);
    dst->drawFastHLine(px + PANEL_W - 3, py + SCOPE_MID, 3, FRAME);
  } else if (s.mode == BARS) {
    // Linha de base e uma marca de meia escala na lateral.
    dst->drawFastHLine(px, py + BASE_Y, PANEL_W, FRAME);
    dst->drawFastHLine(px, py + BASE_Y - BAR_MAX / 2, 2, FRAME);
    dst->drawFastHLine(px + PANEL_W - 2, py + BASE_Y - BAR_MAX / 2, 2, FRAME);
  } else {
    vcrfont::drawText(dst, "L", px, py + VU_Y_L - 1, (int32_t)ACCENT, 1, vcrfont::NO_OUTLINE);
    vcrfont::drawText(dst, "R", px, py + VU_Y_R - 1, (int32_t)ACCENT, 1, vcrfont::NO_OUTLINE);
    // Reguas de -30, -20, -10 e 0 dB sob as agulhas. Posicoes derivadas da
    // mesma janela usada pelo mapeamento, para nao virar constante solta.
    // 10 dB = 10/6,0206 = 1,661 bits = 425 em Q8.
    for (int d = 0; d <= 3; ++d) {
      const int32_t lq8 = VU_REF_LOG2Q8 - (int32_t)(3 - d) * 425;
      const int len = detail::logToPixels(lq8, VU_REF_LOG2Q8, VU_RANGE_LOG2Q8, VU_LEN);
      dst->drawFastVLine(px + VU_X + detail::clampi(len - 1, 0, VU_LEN - 1), py + PANEL_H - 4, 3, FRAME);
    }
  }
  s.painted = true;
  // O fundo estatico repinta tudo: nada do que estava desenhado sobrevive.
  memset(s.drawnLevel, 0, sizeof(s.drawnLevel));
  memset(s.drawnPeak, 0, sizeof(s.drawnPeak));
  s.drawnVuL = s.drawnVuR = s.drawnVuPeakL = s.drawnVuPeakR = 0;
  for (int i = 0; i < N; ++i) {
    s.prevTop[i] = 1;
    s.prevBot[i] = 0;
  }
}

namespace detail {

inline void drawScope(lgfx::LovyanGFX *dst, int ox, int oy, Scope &s) {
  int prevY = -1;
  for (int n = 0; n < N; ++n) {
    // DIVISAO por 32768, nao `>> 15`: com `>>` toda amostra negativa perderia
    // 1 LSB e o traco inteiro subiria meio pixel (o mesmo DC da secao 4).
    const int y = clampi(SCOPE_MID - ((int32_t)s.work[n] * SCOPE_HALF) / 32768, 0, PANEL_H - 1);
    // A coluna cobre o salto desde a amostra anterior: sem isso uma onda
    // aguda vira pontilhado.
    const int a = (prevY < 0) ? y : prevY;
    const int top = a < y ? a : y;
    const int bot = a < y ? y : a;
    spanUpdate(dst, ox, oy, s, n * COL_W, COL_W, s.prevTop[n], s.prevBot[n], top, bot, TRACE);
    s.prevTop[n] = (uint8_t)top;
    s.prevBot[n] = (uint8_t)bot;
    prevY = y;
  }
}

inline void drawBars(lgfx::LovyanGFX *dst, int ox, int oy, Scope &s) {
  for (int b = 0; b < BANDS; ++b) {
    const int x = BAR_PAD + b * (BAR_W + BAR_GAP);
    const int oldH = s.drawnLevel[b], newH = s.level[b];
    if (oldH != newH)
      barUpdate(dst, ox, oy, s, x, BAR_W, oldH, newH, BAR);
    s.drawnLevel[b] = (uint8_t)newH;

    // Ponto de pico: 2 linhas brancas flutuando acima da barra.
    const int oldP = s.drawnPeak[b], newP = s.peak[b];
    if (oldP != newP) {
      // Apaga o antigo, mas so a parte que nao caiu dentro da barra nova (a
      // barra ja pintou aquilo de ambar; repintar de preto seria um buraco).
      const int oy0 = BASE_Y - oldP - PEAK_H + 1;
      for (int r = 0; r < PEAK_H; ++r) {
        const int row = oy0 + r;
        if (row < 0 || row > BASE_Y)
          continue;
        const int h = BASE_Y - row + 1; // altura que essa linha representa
        paint(dst, ox, oy, s, x, row, BAR_W, 1, (h <= newH) ? BAR : BG);
      }
      const int ny0 = BASE_Y - newP - PEAK_H + 1;
      for (int r = 0; r < PEAK_H; ++r) {
        const int row = ny0 + r;
        if (row < 0 || row > BASE_Y)
          continue;
        paint(dst, ox, oy, s, x, row, BAR_W, 1, PEAK);
      }
      s.drawnPeak[b] = (uint8_t)newP;
    }
  }
}

inline void drawVu(lgfx::LovyanGFX *dst, int ox, int oy, Scope &s) {
  needleUpdate(dst, ox, oy, s, VU_Y_L, VU_H, s.drawnVuL, s.vuL, BAR);
  s.drawnVuL = (uint8_t)s.vuL;
  needleUpdate(dst, ox, oy, s, VU_Y_R, VU_H, s.drawnVuR, s.vuR, BAR);
  s.drawnVuR = (uint8_t)s.vuR;

  // Marcadores de pico: 2 px de largura, brancos.
  const int pairs[2][4] = {{s.drawnVuPeakL, s.vuPeakL, VU_Y_L, 0}, {s.drawnVuPeakR, s.vuPeakR, VU_Y_R, 1}};
  for (int i = 0; i < 2; ++i) {
    const int oldP = pairs[i][0], newP = pairs[i][1], yy = pairs[i][2];
    if (oldP == newP)
      continue;
    const int lvl = (i == 0) ? s.vuL : s.vuR;
    const int oldX = clampi(oldP - 2, 0, VU_LEN - 2);
    paint(dst, ox, oy, s, VU_X + oldX, yy, 2, VU_H, (oldX + 2 <= lvl) ? BAR : BG);
    const int newX = clampi(newP - 2, 0, VU_LEN - 2);
    paint(dst, ox, oy, s, VU_X + newX, yy, 2, VU_H, PEAK);
  }
  s.drawnVuPeakL = (uint8_t)s.vuPeakL;
  s.drawnVuPeakR = (uint8_t)s.vuPeakR;
}

// Ataque instantaneo, queda linear. Classico de mostrador de som, e barato:
// nenhuma multiplicacao, nenhum float.
inline uint8_t fall(uint8_t cur, int step) {
  return (uint8_t)(cur > (uint8_t)step ? cur - step : 0);
}

// ATENCAO ao `>=` e nao `>`: com `>`, um alvo ESTAVEL cai na perna do
// decaimento, perde BAR_FALL pixels e no quadro seguinte o ataque o traz de
// volta — a barra fica tremendo 4 px para sempre. O probe pegou isso medindo
// 266 px reescritos por quadro com sinal parado, onde o certo e ZERO. Num tubo
// isso e cintilacao visivel; aqui e um unico caractere de diferenca.
inline void ballistics(Scope &s) {
  for (int b = 0; b < BANDS; ++b) {
    const uint8_t t = s.target[b];
    s.level[b] = (t >= s.level[b]) ? t : fall(s.level[b], BAR_FALL);
    // O pico nunca fica abaixo da barra, senao some por baixo dela.
    uint8_t p = (s.level[b] >= s.peak[b]) ? s.level[b] : fall(s.peak[b], PEAK_FALL);
    if (p < s.level[b])
      p = s.level[b];
    s.peak[b] = p;
  }
}

inline void vuBallistics(Scope &s, int tl, int tr) {
  s.vuL = (uint8_t)((tl >= s.vuL) ? tl : fall(s.vuL, VU_FALL));
  s.vuR = (uint8_t)((tr >= s.vuR) ? tr : fall(s.vuR, VU_FALL));
  uint8_t pl = (s.vuL >= s.vuPeakL) ? s.vuL : fall(s.vuPeakL, PEAK_FALL);
  uint8_t pr = (s.vuR >= s.vuPeakR) ? s.vuR : fall(s.vuPeakR, PEAK_FALL);
  if (pl < s.vuL)
    pl = s.vuL;
  if (pr < s.vuR)
    pr = s.vuR;
  s.vuPeakL = pl;
  s.vuPeakR = pr;
}

} // namespace detail

// ---------------------------------------------------------------------------
//  Entrada principal do consumidor (core 1, dentro do loop()).
// ---------------------------------------------------------------------------
//
// Chame a cada passagem do loop. Ela mesma se limita a UPDATE_MS (20 Hz) e
// devolve `true` quando escreveu pixel — util para quem compoe por cima.
//
// Se `tap.snapshot()` falhar (produtor passou por cima durante a copia) ou se
// nao houver janela nova, NAO ha reanalise: a balistica continua correndo com
// o ultimo alvo e o mostrador segue vivo. Isso e proposital, e o contador
// `Scope::starved` mede quantas vezes aconteceu.
inline bool tick(lgfx::LovyanGFX *dst, int ox, int oy, Scope &s, const Tap &tap, uint32_t nowMs) {
  if (s.painted && !timeReached(nowMs, s.nextAt))
    return false;
  s.nextAt = nowMs + UPDATE_MS;
  s.dirty = Rect{0, 0, 0, 0};
  s.touched = 0;

  if (!s.painted) {
    drawStatic(dst, ox, oy, s);
    // O fundo estatico nao passa pelo `paint()`, entao a contabilidade e feita
    // aqui: o painel inteiro, com moldura, foi repintado.
    s.dirty = panelRect(s);
    s.touched = (uint32_t)(PANEL_W + 2) * (uint32_t)(PANEL_H + 2);
  }

  uint32_t seq = 0;
  uint16_t pl = 0, pr = 0;
  const bool fresh = tap.snapshot(s.work, pl, pr, seq) && seq != s.lastSeq;
  if (fresh) {
    s.lastSeq = seq;
    s.lastDataAt = nowMs;
    s.peakL = pl;
    s.peakR = pr;
    ++s.frames;
  } else {
    ++s.starved;
    if (timeReached(nowMs, s.lastDataAt + STALE_MS)) {
      // Audio parado ou consumidor sem dado ha muito tempo: o mostrador desce
      // ate o repouso em vez de congelar numa foto (que e o que marca o tubo).
      memset(s.target, 0, sizeof(s.target));
      memset(s.work, 0, sizeof(s.work));
      s.peakL = s.peakR = 0;
    }
  }

  switch (s.mode) {
  case SCOPE:
    // Le s.work CRU: analyzeSpectrum() destruiria a onda ao janelar in loco.
    detail::drawScope(dst, ox, oy, s);
    break;
  case BARS:
    if (fresh)
      analyzeSpectrum(s); // aqui s.work vira dado janelado
    detail::ballistics(s);
    detail::drawBars(dst, ox, oy, s);
    break;
  default: {
    const int32_t lq = detail::log2Q8((uint64_t)s.peakL * s.peakL);
    const int32_t rq = detail::log2Q8((uint64_t)s.peakR * s.peakR);
    detail::vuBallistics(s, detail::logToPixels(lq, VU_REF_LOG2Q8, VU_RANGE_LOG2Q8, VU_LEN),
                         detail::logToPixels(rq, VU_REF_LOG2Q8, VU_RANGE_LOG2Q8, VU_LEN));
    detail::drawVu(dst, ox, oy, s);
    break;
  }
  }
  return s.touched != 0;
}

// Rotulo curto do modo, para quem quiser imprimir na legenda dos botoes.
// Desenha com a VcrFont (a Font0/2/4 some na saida composta, ver VcrFont.h).
inline void drawModeLabel(lgfx::LovyanGFX *dst, int ox, int oy, const Scope &s) {
  vcrfont::drawTextCentered(dst, modeName(s.mode), ox + s.x, oy + s.y + PANEL_H + 3, PANEL_W,
                            (int32_t)ACCENT, 1, vcrfont::NO_OUTLINE);
}

} // namespace audioscope
