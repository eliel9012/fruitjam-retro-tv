#pragma once
// ============================================================================
//  BurnIn — gerente de ociosidade e mitigacao de queima de fosforo (burn-in)
//  da saida composta (RCA) do M5 RETRO TV.
//
//  ---------------------------------------------------------------------------
//  Por que isto existe
//  ---------------------------------------------------------------------------
//  Este aparelho nao desenha numa LCD que voce apaga e volta ao normal: ele
//  aciona um TUBO. O menu inicial, o relogio, a tela de informacoes e a
//  previsao ficam paradas por horas, sempre com os MESMOS pixels acesos nas
//  MESMAS posicoes. O fosforo envelhece onde e excitado; o que sobra e uma
//  marca permanente do menu por cima de tudo que a TV mostrar depois. Nao da
//  para desfazer. O firmware ate hoje nao fazia nada a respeito.
//
//  A mitigacao classica de estudio/TV e escalonada, do mais discreto para o
//  mais agressivo:
//
//    ativo  -> deriva  -> escurece -> apaga
//    (nada)    (pixel     (redesenha  (protetor de tela:
//               shift)     escuro)     so um elemento movel no preto)
//
//  ---------------------------------------------------------------------------
//  1. A deriva (pixel shift) — o ponto principal
//  ---------------------------------------------------------------------------
//  Todo pintor deste firmware ja tem a assinatura (dst, ox, oy[, user]) — ver
//  crt::fx::Paint no ScreenFx.h e paintCurrent/paintForecast/weatherHeader no
//  src/main.cpp. Entao a deriva global entra como o (ox, oy) que o CHAMADOR
//  passa, sem tocar no desenho de uma unica tela.
//
//  Este modulo NAO desenha nada. Ele so diz qual e o deslocamento agora e
//  levanta uma bandeira quando esse deslocamento mudou; repintar e do chamador,
//  que e quem sabe em que estado da maquina de interface esta.
//
//  Regras da deriva, nesta ordem de importancia:
//    * pequena — poucos pixels. Uma interface que anda e PIOR que burn-in.
//    * lenta   — um passo a cada DRIFT_STEP_MS (minutos), nunca por quadro.
//    * de 1 px por passo, num eixo so: nada de salto perceptivel.
//    * dentro da area segura. Ver "Geometria" logo abaixo.
//
//  ---------------------------------------------------------------------------
//  2. Geometria: de onde sai o alcance da deriva
//  ---------------------------------------------------------------------------
//  O orcamento vem da margem de overscan que o SafeArea.h ja reserva:
//  SAFE_L = W - SAFE_R = 24 px na horizontal, SAFE_T = H - SAFE_B = 18 px na
//  vertical. Gastamos 1/8 dessa margem (DRIFT_DIVISOR), com teto de
//  DRIFT_CAP = 3 px:
//
//    DRIFT_X = min(min(24, 24) / 8, 3) = 3
//    DRIFT_Y = min(min(18, 18) / 8, 3) = 2
//
//  Por que 1/8 e nao a margem inteira: os 7% de overscan sao NOMINAIS. Ha tubo
//  que esconde 10%. Gastar a margem toda com deriva apagaria texto em alguns
//  aparelhos. 1/8 e ruido perto da variacao entre TVs.
//
//  ATENCAO ao contrato exato: derivar empurra o conteudo, entao conteudo
//  desenhado ENCOSTADO em crt::SAFE_* sai da caixa segura assim que a deriva
//  for diferente de zero. Por isso este header tambem publica a caixa
//  reduzida INNER_L/T/R/B = SAFE_* recuado de (DRIFT_X, DRIFT_Y). A garantia
//  e esta, e esta verificada por static_assert e pelo probe:
//
//    conteudo dentro de INNER_* + qualquer deslocamento valido
//      => continua dentro de crt::SAFE_*.
//
//  Uma tela que ainda desenhe ate SAFE_* continua funcionando e continua
//  dentro do RASTER (a deriva cabe folgada na margem de overscan); ela so nao
//  tem a garantia forte da caixa segura. Ao mexer no layout de uma tela,
//  mire em INNER_*.
//
//  ---------------------------------------------------------------------------
//  3. O que existe DE VERDADE para escurecer a saida composta
//  ---------------------------------------------------------------------------
//  Investigado no M5GFX 0.2.29 deste repositorio. O resultado importa porque a
//  resposta ingenua ("usa setBrightness") esta errada:
//
//  a) M5.Display.setBrightness() -> AXP192/Light_PWM. Mexe SO no backlight da
//     LCD do Core2. O caminho CVBS nao passa por ali. Em src/main.cpp o
//     setBacklight() ja faz isso pelo DCDC3. Irrelevante para o tubo.
//
//  b) lgfx::Panel_CVBS::setOutputLevel(uint8_t) EXISTE e e controle de
//     luminancia de verdade (Panel_CVBS.inl:2380). Ele chama
//     updateSignalLevel(), que recalcula WHITE/BLACK/BLANKING a partir de
//     `48 * output_level` e reconstroi a paleta NO LUGAR. Nao realoca nada.
//     O setup() atual usa rca.setOutputBoost(true), que e output_level = 200.
//
//     DUAS armadilhas:
//
//     b1) NAO chame M5ModuleRCA::setOutputLevel(). A versao do MODULO
//         (M5ModuleRCA.h:232) passa por Panel_CVBS::config_detail(), que faz
//         deinit() + init(false) — ou seja, libera e REALOCA o framebuffer de
//         76.800 B e reinicia o DMA do I2S0. Com 28 a 38 KB de heap livre isso
//         e um pedido de falha de alocacao, alem de apagar a imagem. O caminho
//         barato e o do PAINEL:
//           static_cast<lgfx::Panel_CVBS *>(rca.panel())->setOutputLevel(n);
//
//     b2) internal.SYNC_LEVEL e constante 0 (Panel_CVBS.inl:215) enquanto
//         BLANKING_LEVEL escala junto com output_level. Logo a amplitude do
//         SINCRONISMO encolhe na mesma proporcao do video. Cair muito abaixo
//         do nivel configurado arrisca o separador de sincronismo da TV perder
//         o engate: imagem rolando, nao imagem escura. Por isso
//         OUT_LEVEL_DIM = 150 (-25% de 200) e um teto conservador, e por isso
//         USE_OUTPUT_LEVEL_DIM nasce DESLIGADO.
//
//  c) Panel_CVBS::setChromaLevel(uint8_t) tambem existe, mesmo caminho barato.
//     0 = preto e branco. Util e seguro (nao toca no sincronismo), mas nao
//     escurece: so tira cor.
//
//  VEREDITO: existe controle de brilho real para o composto, mas ele e
//  arriscado (b2) e nao e o caminho padrao aqui. O escurecimento deste modulo
//  e REDESENHO EM CORES MAIS ESCURAS (dim565), que sempre funciona, custa um
//  repinte e nao pode derrubar o sincronismo. O ajuste por output_level fica
//  disponivel como constante documentada, para quem quiser testar no aparelho.
//
//  ---------------------------------------------------------------------------
//  4. Custo
//  ---------------------------------------------------------------------------
//  sizeof(burnin::Manager) e da ordem de 40 bytes. Nenhuma alocacao, nenhum
//  LGFX_Sprite (um sprite de 5 KB ja causou regressao de memoria aqui), nenhum
//  float, nenhuma dependencia de M5GFX — so <stdint.h>, SafeArea.h e UiLogic.h.
//  O tick() e um punhado de comparacoes; o protetor de tela repinta dois
//  retangulos de SAVER_W x SAVER_H a cada SAVER_STEP_MS.
//
//  ---------------------------------------------------------------------------
//  5. Armadilha de cor (vale para QUALQUER cor que passe por aqui)
//  ---------------------------------------------------------------------------
//  A LovyanGFX escolhe o formato pelo TIPO DO ARGUMENTO
//  (misc/colortype.hpp:861-866): uint16_t/int16_t/int32_t viram RGB565, mas
//  uint32_t vira RGB888. Um (uint32_t)0xBDF7 silenciosamente vira
//  R=0, G=189, B=247. dim565() recebe e devolve uint16_t de proposito —
//  mantenha o uint16_t ate a chamada de desenho.
//
//  E: nunca ciano saturado (TFT_CYAN / 0x07FF) no composto — o dot crawl da
//  crominancia do NTSC faz aquilo cintilar. O acento seguro do projeto e
//  0x96BC, que e o SAVER_COLOR daqui.
//
//  ---------------------------------------------------------------------------
//  6. Uso (integracao no src/main.cpp — o chamador faz, este header nao)
//  ---------------------------------------------------------------------------
//    static burnin::Manager idle;
//
//    // toda acao do usuario (handleNavigation, handleTouch, comando do diag)
//    idle.notifyActivity(millis());
//
//    // uma vez por loop()
//    switch (idle.tick(millis())) { ... }
//    if (idle.takeRepaint()) redesenhaTelaAtual();   // usa idle.ox()/idle.oy()
//
//    // nos pintores ja existentes, somando a deriva ao que ja vinha:
//    paintCurrent(&rca, idle.ox(), idle.oy(), nullptr);
// ============================================================================

#include <stdint.h>

#include "SafeArea.h"
#include "UiLogic.h" // timeReached(): millis() da a volta em ~49 dias

namespace burnin {

// ---------------------------------------------------------------------------
// Estagios
// ---------------------------------------------------------------------------
enum Stage : uint8_t {
  STAGE_ACTIVE = 0, // alguem esta mexendo: desenho normal, deriva PARADA (nao zerada)
  STAGE_DRIFT,      // deriva de poucos pixels, o resto igual
  STAGE_DIM,        // deriva + redesenho em cores escurecidas
  STAGE_BLANK       // protetor de tela: preto + um elemento movel
};

// ATIVO e DERIVA desenham exatamente igual (mesmas cores, mesmo deslocamento):
// so a partir de ESCURO e que a aparencia muda. Serve para nao mandar repintar
// a tela numa troca de estagio que ninguem veria.
inline bool visualDiffers(Stage a, Stage b) {
  return a != b && (a >= STAGE_DIM || b >= STAGE_DIM);
}

inline const char *stageLabel(Stage s) { // ASCII, para o "diag status"
  return s == STAGE_ACTIVE ? "ATIVO"
         : s == STAGE_DRIFT ? "DERIVA"
         : s == STAGE_DIM   ? "ESCURO"
                            : "APAGADO";
}

// ---------------------------------------------------------------------------
// Limiares — os numeros para mexer.
//
// DRIFT_AFTER_MS = 90 s. A deriva e invisivel (1 px a cada 2 min) e a queima
//   e cumulativa desde o primeiro segundo, entao comeca cedo e de graca. 90 s e
//   so para nao mover a tela debaixo de quem acabou de largar o controle.
// DIM_AFTER_MS = 10 min. Tempo de sobra para ler a previsao inteira ou um
//   ciclo completo das duas paginas do Weather (troca a cada 10 s). Escurecer
//   antes disso seria escurecer em cima de alguem lendo.
// BLANK_AFTER_MS = 30 min. Meia hora parado no mesmo menu nao e mais uso, e
//   a partir dai a unica mitigacao honesta e parar de mostrar a interface.
// DRIFT_STEP_MS = 2 min. E o numero mais delicado: acima disso a mitigacao
//   fica fraca, abaixo a interface "anda" e o olho pega. 2 min dao um ciclo
//   completo de 56 passos em ~112 min.
// SAVER_STEP_MS = 200 ms. 5 Hz — movimento continuo o bastante para o olho e
//   barato o bastante para rodar a noite inteira.
// ---------------------------------------------------------------------------
static const uint32_t DRIFT_AFTER_MS = 90UL * 1000UL;
static const uint32_t DIM_AFTER_MS = 10UL * 60UL * 1000UL;
static const uint32_t BLANK_AFTER_MS = 30UL * 60UL * 1000UL;
static const uint32_t DRIFT_STEP_MS = 2UL * 60UL * 1000UL;
static const uint32_t SAVER_STEP_MS = 200UL;

// ---------------------------------------------------------------------------
// Geometria da deriva, derivada de crt::SAFE_*.
//
// C++11: corpo de constexpr e UM return so — sem laco, sem variavel local.
// Ja quebrou o firmware antes (VcrFont.h). Onde precisar de conta, recursao ou
// composicao de funcoes, como aqui.
// ---------------------------------------------------------------------------
constexpr int cmin(int a, int b) { return a < b ? a : b; }
constexpr int cmax(int a, int b) { return a > b ? a : b; }

// Margem de overscan disponivel em cada eixo (o lado mais apertado).
constexpr int MARGIN_X = cmin(crt::SAFE_L, crt::W - crt::SAFE_R); // 24
constexpr int MARGIN_Y = cmin(crt::SAFE_T, crt::H - crt::SAFE_B); // 18

constexpr int DRIFT_DIVISOR = 8; // gastamos 1/8 da margem nominal
constexpr int DRIFT_CAP = 3;     // teto absoluto: acima disso o olho pega

constexpr int DRIFT_X = cmax(cmin(MARGIN_X / DRIFT_DIVISOR, DRIFT_CAP), 0); // 3
constexpr int DRIFT_Y = cmax(cmin(MARGIN_Y / DRIFT_DIVISOR, DRIFT_CAP), 0); // 2

// Caixa segura reduzida: conteudo aqui dentro aguenta qualquer deslocamento.
constexpr int INNER_L = crt::SAFE_L + DRIFT_X; // 27
constexpr int INNER_T = crt::SAFE_T + DRIFT_Y; // 20
constexpr int INNER_R = crt::SAFE_R - DRIFT_X; // 293
constexpr int INNER_B = crt::SAFE_B - DRIFT_Y; // 220
constexpr int INNER_W = INNER_R - INNER_L;     // 266
constexpr int INNER_H = INNER_B - INNER_T;     // 200

// A promessa do cabecalho, conferida pelo compilador.
static_assert(INNER_L - DRIFT_X >= crt::SAFE_L, "deriva vaza pela esquerda da area segura");
static_assert(INNER_T - DRIFT_Y >= crt::SAFE_T, "deriva vaza por cima da area segura");
static_assert(INNER_R + DRIFT_X <= crt::SAFE_R, "deriva vaza pela direita da area segura");
static_assert(INNER_B + DRIFT_Y <= crt::SAFE_B, "deriva vaza por baixo da area segura");
static_assert(INNER_W > 0 && INNER_H > 0, "caixa interna degenerada");

// Malha percorrida pela deriva: (2*DRIFT_X+1) x (2*DRIFT_Y+1) = 7 x 5 = 35
// posicoes. O passeio e uma serpentina com as LINHAS tambem em vaivem, o que
// garante que cada passo mova exatamente 1 px em exatamente um eixo — inclusive
// na virada de linha e na virada do ciclo. Periodo = COLS * ROW_CYCLE = 56.
constexpr int DRIFT_COLS = 2 * DRIFT_X + 1;                 // 7
constexpr int DRIFT_ROWS = 2 * DRIFT_Y + 1;                 // 5
constexpr int DRIFT_ROW_CYCLE = cmax(2 * DRIFT_ROWS - 2, 1); // 8
constexpr int DRIFT_CYCLE = DRIFT_COLS * DRIFT_ROW_CYCLE;    // 56

// Fase inicial: o passo 0 tem de ser o deslocamento ZERO, senao entrar no
// estagio de deriva daria um salto de 3 px de uma vez. O centro da malha e a
// coluna DRIFT_X da linha DRIFT_Y (linha par, entao a serpentina nao inverte).
constexpr int DRIFT_PHASE = DRIFT_Y * DRIFT_COLS + DRIFT_X; // 17

// Indice do passo -> deslocamento. Funcoes puras, testadas pelo probe.
// (linhas 0..ROWS-1 e de volta: as linhas das pontas sao visitadas metade das
// vezes que as do meio. Sobre 1 px de amplitude isso e irrelevante perto da
// alternativa, que e nao derivar.)
inline int driftX(uint32_t step) {
  const uint32_t k = step + (uint32_t)DRIFT_PHASE;
  const int col = (int)(k % (uint32_t)DRIFT_COLS);
  const int rowRaw = (int)((k / (uint32_t)DRIFT_COLS) % (uint32_t)DRIFT_ROW_CYCLE);
  return ((rowRaw & 1) ? (DRIFT_COLS - 1 - col) : col) - DRIFT_X;
}
inline int driftY(uint32_t step) {
  const uint32_t k = step + (uint32_t)DRIFT_PHASE;
  const int rowRaw = (int)((k / (uint32_t)DRIFT_COLS) % (uint32_t)DRIFT_ROW_CYCLE);
  return (rowRaw < DRIFT_ROWS ? rowRaw : (2 * DRIFT_ROWS - 2 - rowRaw)) - DRIFT_Y;
}

// ---------------------------------------------------------------------------
// Escurecimento por redesenho
// ---------------------------------------------------------------------------
// Numerador/denominador em vez de float: sem FPU no caminho e exato.
// 3/8 derruba a energia do fosforo para ~37% sem sumir com a leitura.
static const uint8_t DIM_NUM = 3, DIM_DEN = 8;

// Ajuste opcional do nivel de saida do CVBS. Ver secao 3 do cabecalho: so pelo
// PAINEL (Panel_CVBS::setOutputLevel), nunca pelo M5ModuleRCA, e nao desca de
// OUT_LEVEL_DIM sob risco de a TV perder o sincronismo.
static const uint8_t OUT_LEVEL_NORMAL = 200; // = rca.setOutputBoost(true)
static const uint8_t OUT_LEVEL_DIM = 150;    // -25%, teto conservador
static const bool USE_OUTPUT_LEVEL_DIM = false; // nasce desligado: nao testado na TV

// RGB565 escurecido. RECEBE E DEVOLVE uint16_t: um uint32_t aqui seria lido
// como RGB888 na hora de desenhar (colortype.hpp:861-866).
inline uint16_t dim565(uint16_t c, uint8_t num, uint8_t den) {
  return (uint16_t)(((uint16_t)((((c >> 11) & 0x1F) * num) / den) << 11) |
                    ((uint16_t)((((c >> 5) & 0x3F) * num) / den) << 5) |
                    ((uint16_t)(((c & 0x1F) * num) / den)));
}
inline uint16_t dim565(uint16_t c) { return dim565(c, DIM_NUM, DIM_DEN); }

// Cor que uma tela deve usar no estagio `s`. No STAGE_BLANK tudo e preto: quem
// chegou ali nao desenha interface nenhuma, so o protetor.
inline uint16_t shade(Stage s, uint16_t c) {
  return s == STAGE_BLANK ? (uint16_t)0 : (s == STAGE_DIM ? dim565(c) : c);
}

// ---------------------------------------------------------------------------
// Protetor de tela
// ---------------------------------------------------------------------------
// Um retangulo pequeno quicando no preto. Percorre o RASTER INTEIRO, nao so a
// area segura: a borda tambem tem fosforo, e ali o movimento nem aparece.
// Velocidades 4 e 3 (primas entre si) alongam bastante o caminho antes de ele
// se repetir, o que evita a diagonal fixa do protetor virar ela mesma uma
// marca.
static const int16_t SAVER_W = 16, SAVER_H = 12;
static const int16_t SAVER_VX = 4, SAVER_VY = 3;
static const uint16_t SAVER_COLOR = 0x96BC; // acento do projeto; JAMAIS 0x07FF

// ---------------------------------------------------------------------------
// O gerente
// ---------------------------------------------------------------------------
class Manager {
public:
  Manager() { reset(0); }

  // Zera tudo. Chame no setup() com millis() e nada mais.
  void reset(uint32_t now) {
    last_ = now;
    stage_ = STAGE_ACTIVE;
    driftDue_ = now + DRIFT_AFTER_MS;
    saverDue_ = now + SAVER_STEP_MS;
    step_ = 0;
    ox_ = 0;
    oy_ = 0;
    repaint_ = false;
    offsetChanged_ = false;
    saverMoved_ = false;
    sx_ = (int16_t)((crt::W - SAVER_W) / 2);
    sy_ = (int16_t)((crt::H - SAVER_H) / 2);
    px_ = sx_;
    py_ = sy_;
    vx_ = SAVER_VX;
    vy_ = SAVER_VY;
  }

  // Toda acao do usuario passa por aqui: botao, toque, comando do diag.
  // Volta ao estagio ativo e devolve true se a tela precisa ser repintada.
  //
  // O deslocamento e o contador de passos NAO sao zerados, de proposito, por
  // duas razoes que o probe pegou:
  //
  //   * uniformidade. Zerando, o passeio recomecaria sempre na mesma celula da
  //     malha; quem usa o aparelho de meia em meia hora so visitaria um canto
  //     dela e a mitigacao ficaria enviesada justo nas posicoes mais usadas.
  //   * nada de solavanco. Voltar o deslocamento a (0,0) seria um salto de ate
  //     3 px MAIS um repinte de tela cheia a cada botao apertado — a TV
  //     piscando a cada navegacao, que e exatamente o que a secao 1 proibe.
  //
  // Visualmente ATIVO e DERIVA sao iguais (mesmas cores, mesmo deslocamento),
  // entao so voltar de ESCURO/APAGADO e que obriga a repintar.
  bool notifyActivity(uint32_t now) {
    last_ = now;
    driftDue_ = now + DRIFT_AFTER_MS;
    const bool wasDark = (stage_ >= STAGE_DIM);
    stage_ = STAGE_ACTIVE;
    if (wasDark)
      repaint_ = true;
    return wasDark;
  }

  // Uma vez por loop(). Devolve o estagio corrente.
  Stage tick(uint32_t now) {
    advanceStage(now);
    // No STAGE_BLANK a interface nao esta na tela: continuar derivando so
    // levantaria a bandeira de repintura em cima do protetor.
    if (stage_ == STAGE_DRIFT || stage_ == STAGE_DIM)
      advanceDrift(now);
    if (stage_ == STAGE_BLANK)
      advanceSaver(now);
    else
      saverDue_ = now + SAVER_STEP_MS;
    return stage_;
  }

  Stage stage() const { return stage_; }
  // Ha quanto tempo ninguem mexe. Subtracao sem sinal: correta na volta do
  // millis() ate 49 dias.
  uint32_t idleMs(uint32_t now) const { return now - last_; }

  // Deriva corrente, para alimentar o (ox, oy) dos pintores.
  int ox() const { return ox_; }
  int oy() const { return oy_; }

  // Repintura pendente (mudou o estagio ou mudou o deslocamento). O modulo NAO
  // repinta: quem sabe qual tela esta no ar e o chamador.
  bool repaintPending() const { return repaint_; }
  bool takeRepaint() {
    const bool r = repaint_;
    repaint_ = false;
    offsetChanged_ = false;
    return r;
  }
  // So para diagnostico/teste: o deslocamento mudou neste tick?
  bool offsetChanged() const { return offsetChanged_; }

  // Protetor de tela. Quando takeSaverStep() devolve true, apague o retangulo
  // em (saverPrevX, saverPrevY) e desenhe em (saverX, saverY).
  bool takeSaverStep() {
    const bool m = saverMoved_;
    saverMoved_ = false;
    return m;
  }
  int saverX() const { return sx_; }
  int saverY() const { return sy_; }
  int saverPrevX() const { return px_; }
  int saverPrevY() const { return py_; }

  // Nivel de saida do CVBS sugerido para o estagio corrente. Ver secao 3: so
  // vale alguma coisa se o chamador optar por USE_OUTPUT_LEVEL_DIM e aplicar
  // pelo Panel_CVBS, nunca pelo M5ModuleRCA.
  uint8_t outputLevel() const {
    return (USE_OUTPUT_LEVEL_DIM && stage_ >= STAGE_DIM) ? OUT_LEVEL_DIM : OUT_LEVEL_NORMAL;
  }

  // Cor ja ajustada ao estagio, atalho para os pintores.
  uint16_t shade(uint16_t c) const { return burnin::shade(stage_, c); }

private:
  // O estagio so sobe; quem desce e o notifyActivity(). Alem de ser a semantica
  // certa, isso mata a borda do timeReached(): int32_t(now - prazo) volta a ser
  // negativo ~24,8 dias depois do prazo, e um aparelho parado por um mes nao
  // pode "acordar" sozinho do protetor de tela.
  void advanceStage(uint32_t now) {
    Stage want = stage_;
    if (stage_ < STAGE_BLANK && timeReached(now, last_ + BLANK_AFTER_MS))
      want = STAGE_BLANK;
    else if (stage_ < STAGE_DIM && timeReached(now, last_ + DIM_AFTER_MS))
      want = STAGE_DIM;
    else if (stage_ < STAGE_DRIFT && timeReached(now, last_ + DRIFT_AFTER_MS))
      want = STAGE_DRIFT;
    if (want == stage_)
      return;
    if (visualDiffers(want, stage_))
      repaint_ = true; // ATIVO->DERIVA nao muda nada na tela; DERIVA->ESCURO sim
    stage_ = want;
    if (stage_ == STAGE_BLANK) { // entrou no protetor: recomeca do centro
      sx_ = (int16_t)((crt::W - SAVER_W) / 2);
      sy_ = (int16_t)((crt::H - SAVER_H) / 2);
      px_ = sx_;
      py_ = sy_;
      saverDue_ = now + SAVER_STEP_MS;
    }
  }

  void advanceDrift(uint32_t now) {
    if (!timeReached(now, driftDue_))
      return;
    // Recoloca o prazo a partir de agora (e nao do prazo anterior): se o loop()
    // ficou preso decodificando video por mais de um passo, nao queremos uma
    // rajada de passos de deriva para "recuperar o atraso".
    driftDue_ = now + DRIFT_STEP_MS;
    ++step_;
    const int8_t nx = (int8_t)driftX(step_), ny = (int8_t)driftY(step_);
    if (nx == ox_ && ny == oy_)
      return;
    ox_ = nx;
    oy_ = ny;
    offsetChanged_ = true;
    repaint_ = true; // mudou o deslocamento: a tela inteira tem de sair de novo
  }

  void advanceSaver(uint32_t now) {
    if (!timeReached(now, saverDue_))
      return;
    saverDue_ = now + SAVER_STEP_MS;
    px_ = sx_;
    py_ = sy_;
    sx_ = (int16_t)(sx_ + vx_);
    sy_ = (int16_t)(sy_ + vy_);
    if (sx_ < 0) {
      sx_ = 0;
      vx_ = (int16_t)-vx_;
    } else if (sx_ > (int16_t)(crt::W - SAVER_W)) {
      sx_ = (int16_t)(crt::W - SAVER_W);
      vx_ = (int16_t)-vx_;
    }
    if (sy_ < 0) {
      sy_ = 0;
      vy_ = (int16_t)-vy_;
    } else if (sy_ > (int16_t)(crt::H - SAVER_H)) {
      sy_ = (int16_t)(crt::H - SAVER_H);
      vy_ = (int16_t)-vy_;
    }
    saverMoved_ = (sx_ != px_ || sy_ != py_);
  }

  uint32_t last_, driftDue_, saverDue_, step_;
  int16_t sx_, sy_, px_, py_, vx_, vy_;
  int8_t ox_, oy_;
  Stage stage_;
  bool repaint_, offsetChanged_, saverMoved_;
};

} // namespace burnin
