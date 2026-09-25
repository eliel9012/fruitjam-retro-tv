#pragma once
// ============================================================================
// TransferScreen — tela do modo "receber arquivo pelo Wi-Fi".
//
// Enquanto o aparelho esta servindo HTTP, esta tela e a unica coisa que ele
// mostra. Quem esta na frente da TV precisa LER dali o endereco, o usuario e a
// senha da sessao e digitar tudo num computador do outro lado da sala. Ou
// seja: e um cartaz, nao um painel de diagnostico. Dai a hierarquia do
// desenho, que e a regra que decide todo o resto deste arquivo:
//
//   1. endereco   — o dado que a pessoa precisa copiar primeiro, em fonte
//                   grossa (VcrFont 12x16) dentro de uma moldura ciano;
//   2. usuario/senha — tambem 12x16, porque tambem sao digitados;
//   3. estado     — faixa colorida, que se le pela cor mesmo antes do texto;
//   4. progresso  — nome do arquivo, porcentagem, barra e contagem de bytes;
//   5. rede e aviso — contexto, em corpo pequeno.
//
// Por que fontes diferentes por campo (nao e capricho):
//
//   * O endereco so tem digitos, ponto, dois-pontos e barra — nada que dependa
//     de caixa. Entao vai na VcrFont, que tem traco de 2 px constante e e a
//     unica que aguenta o borrao horizontal do NTSC (ver o cabecalho da
//     VcrFont.h). Ela desenha so em caixa alta, e isso aqui e inofensivo.
//   * Usuario e senha NAO podem passar pela VcrFont: a senha e gerada por
//     sessao e diferencia maiuscula de minuscula. Uma senha "aB7k" desenhada
//     como "AB7K" faria a pessoa digitar errado e culpar o aparelho. Esses dois
//     campos saem na Font0 do M5GFX em tamanho 2 — que tem exatamente a mesma
//     celula de 12x16 e traco de 2 px depois da escala, mas preserva a caixa.
//   * SSID, nome de arquivo e bytes saem em Font0 tamanho 1 quando nao couberem
//     grandes: e preferivel ler pequeno a ler truncado.
//
// Texto de fora (SSID, nome do arquivo, mensagem de erro) passa por
// ascii::normalize antes de chegar na tela. As fontes bitmap so tem ASCII e em
// UTF-8 cada letra acentuada sao dois bytes sem glifo — "Não" viraria "N  o".
//
// Geometria: tudo dentro de crt::SAFE_* (SafeArea.h), porque um tubo esconde
// ~7% de cada borda. So o fundo sangra ate a borda do raster. O cabecalho
// (HEAD_Y / HEAD_RULE_Y) e a faixa de rodape (BAR_Y / BAR_H) sao os mesmos das
// outras telas, para esta nao destoar.
//
// LCD e saida composta usam o MESMO desenho, nas mesmas coordenadas. O LCD do
// Core2 nao tem overscan e sobraria margem, mas duas geometrias para a mesma
// tela e justamente a armadilha 8 do AGENTS.md ("duas constantes descrevendo a
// mesma geometria"): elas divergem na primeira manutencao. No LCD isto aparece
// apenas como uma margem um pouco maior, que e o que drawInfo()/drawSettings()
// ja fazem hoje.
//
// Dependencias: LovyanGFX (fj/Gfx.h, vem pela VcrFont), SafeArea.h e Ascii.h. Nada de
// Arduino, WiFi, SD ou FreeRTOS: o destino e um LovyanGFX*, entao o mesmo
// header serve ao painel CVBS do aparelho e ao painel SDL do simulador.
//
// O modulo de transferencia em si (HTTP, cartao, senha de sessao) e de outro
// arquivo: esta tela recebe um State ja pronto e so desenha. Quem integra faz
// a ponte entre os dois.
//
// Compila em -std=gnu++11 (conferir com `make -C sim cxx11`).
// ============================================================================

#include "Ascii.h"
#include "SafeArea.h"
#include "VcrFont.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace xfer {

// ---------------------------------------------------------------------------
//  Estado exibido
// ---------------------------------------------------------------------------

// Estagio da sessao de transferencia. E o que escolhe a cor da faixa de estado
// e o que a area de progresso mostra.
enum class Stage : uint8_t {
  Waiting,   // servidor no ar, ninguem conectou ainda
  Receiving, // recebendo um arquivo agora
  Done,      // arquivo gravado no cartao
  Error      // falhou (sem espaco, conexao caiu, senha errada...)
};

// Rotulos padrao. Ficam aqui, e nao na LocalizationPTBR.h, porque este header
// nao pode depender de nada do firmware — mas todos sao sobrescreviveis pelo
// State, entao a integracao pode apontar para as strings de la.
constexpr const char *kTitle = "TRANSFERIR ARQUIVOS";
constexpr const char *kWarning = "RESTO DO APARELHO SUSPENSO";
// Enumera o que o aparelho aceita, porque essa e a duvida de quem esta parado
// na frente da TV com o computador aberto — "envie o arquivo no PC" nao dizia
// QUAL arquivo, e agora sao tres categorias (videos/, music/, fotos/).
//
// Curto de proposito: com 21 caracteres a dica ainda cabe em corpo grande na
// largura segura (21 x 12 = 252 px, contra os 272 de crt::SAFE_W). Uma frase
// mais longa cairia para a fonte fina e viraria um rodape ilegivel no meio da
// tela — 22 caracteres e o teto absoluto. ASCII sem acento: as fontes bitmap
// nao tem glifo para acentuado (AGENTS 2.7).
constexpr const char *kHint = "VIDEO, MUSICA E FOTOS";

struct State {
  // O que a pessoa digita no navegador, ja pronto: "http://192.168.0.12".
  // Nao inventamos o prefixo aqui — quem monta a string sabe se ha porta.
  const char *address = nullptr;
  const char *ssid = nullptr;     // rede em que o aparelho esta
  const char *user = nullptr;     // usuario do acesso HTTP
  const char *password = nullptr; // senha da sessao (caixa preservada!)

  Stage stage = Stage::Waiting;
  const char *fileName = nullptr; // arquivo em transito / recebido
  // Contagem de bytes. uint32_t basta: o limite de arquivo do FAT32 e 4 GiB-1,
  // que e exatamente o alcance deste tipo.
  uint32_t bytesReceived = 0;
  uint32_t bytesTotal = 0; // 0 = tamanho desconhecido (sem Content-Length)

  // Texto da faixa de estado. Nulo usa o rotulo padrao do estagio; e por aqui
  // que passa o detalhe do erro ("SEM ESPACO NO CARTAO").
  const char *statusText = nullptr;

  const char *title = kTitle;
  const char *warning = kWarning;      // aviso de que o aparelho esta suspenso
  const char *buttonLabel = nullptr;   // legenda do botao de sair, se houver
};

// ---------------------------------------------------------------------------
//  Cores — RGB565 escritas na mao, para o header nao depender das macros TFT_*
// ---------------------------------------------------------------------------

constexpr uint16_t kBg = 0x000F;     // azul-marinho, o mesmo das outras telas
constexpr uint16_t kPanel = 0x0018;  // azul do painel do endereco (um degrau
                                     // acima do fundo: em RGB332 o azul tem so
                                     // 2 bits, e estes dois caem em indices
                                     // diferentes — 0x0010 nao cairia)
constexpr uint16_t kInk = 0xFFFF;    // branco: valores
constexpr uint16_t kAccent = 0x07FF; // ciano: rotulos e molduras
constexpr uint16_t kDim = 0x8410;    // cinza: dica e texto secundario

// Cores dos estagios. Saturadas de proposito: em RGB332 (a profundidade do
// framebuffer composto) meio-tom vira banda, e a cor tem de ser reconhecivel
// do outro lado da sala antes de a pessoa ler a palavra.
constexpr uint16_t kWaitFill = 0x07FF; // ciano
constexpr uint16_t kRecvFill = 0xFFE0; // amarelo
constexpr uint16_t kDoneFill = 0x07E0; // verde
constexpr uint16_t kErrFill = 0xF800;  // vermelho

// ---------------------------------------------------------------------------
//  Geometria — colunas e linhas fixas dentro da area segura
// ---------------------------------------------------------------------------
namespace layout {

constexpr int kRowH = 16; // altura de uma linha de valor (a celula da VcrFont)

constexpr int kTitleY = crt::HEAD_Y;      // 18 — titulo, Font0 tamanho 2
constexpr int kRuleY = crt::HEAD_RULE_Y;  // 46 — regua ciano do cabecalho

constexpr int kNetY = 52;    // linha "REDE  <ssid>", corpo pequeno
constexpr int kNetH = 8;

constexpr int kAddrY = 62;   // painel do endereco
constexpr int kAddrH = 28;

constexpr int kUserY = 94;   // "USUARIO <valor>"
constexpr int kPassY = 114;  // "SENHA   <valor>"

constexpr int kStatusY = 136; // faixa colorida de estado
constexpr int kStatusH = 20;

constexpr int kFileY = 160;   // nome do arquivo + porcentagem
constexpr int kPctW = 56;     // reserva da porcentagem ("100%" = 48 px)

constexpr int kBarY = 180;    // barra de progresso
constexpr int kBarH = 12;

constexpr int kBytesY = 194;  // "RECEBIDO 1,2 MB / 4,6 MB", corpo pequeno
constexpr int kBytesH = 8;

// Coluna dos valores de REDE/USUARIO/SENHA. 54 px = o rotulo mais longo
// ("USUARIO", 42 px na Font0 tamanho 1) mais 12 de respiro. A coluna foi
// apertada ate aqui de proposito: cada pixel que sobra a esquerda e um
// caractere a mais de senha em corpo grande, e uma senha de sessao com 16
// caracteres so cabia grande depois disso.
constexpr int kLabelX = crt::SAFE_L;           // 24
constexpr int kValueX = crt::SAFE_L + 54;      // 78
constexpr int kValueW = crt::SAFE_R - kValueX; // 218 -> 18 caracteres grandes

// Faixa que drawProgress() repinta: do topo da faixa de estado ate o fim da
// linha de bytes. Exposta porque quem integra pode querer limpar a regiao por
// conta propria (numa transicao, por exemplo).
constexpr int kDynamicY = kStatusY;                        // 136
constexpr int kDynamicH = (kBytesY + kBytesH) - kStatusY;  // 66

// Quantos caracteres cabem em corpo grosso na largura segura. E o teto do
// endereco: "http://192.168.100.130" tem 22 e cabe raspando; com porta
// ("...:8080") ja nao cabe e o painel cai para a fonte fina, que e legivel de
// perto mas nao da poltrona. Quem monta a string faz bem em evitar a porta.
constexpr int kThickChars = crt::SAFE_W / vcrfont::CELL_W; // 22

static_assert(kTitleY >= crt::SAFE_T, "titulo escapa pelo topo da area segura");
static_assert(kBytesY + kBytesH <= crt::BAR_Y, "linha de bytes invade a faixa de rodape");
static_assert(crt::BAR_Y + crt::BAR_H <= crt::SAFE_B, "rodape escapa pela base da area segura");
static_assert(kValueX + kValueW <= crt::SAFE_R, "coluna de valores escapa pela direita");
static_assert(kThickChars >= 22, "endereco IPv4 sem porta nao cabe mais em corpo grosso");

// As linhas nao podem se encavalar. Sem isto, mexer em uma constante para
// ganhar 4 px desenha texto por cima de texto e so aparece no probe — se
// alguem lembrar de olhar o PNG.
static_assert(kRuleY < kNetY, "regua do cabecalho invade a linha da rede");
static_assert(kNetY + kNetH <= kAddrY, "linha da rede invade o painel do endereco");
static_assert(kAddrY + kAddrH <= kUserY, "painel do endereco invade a linha do usuario");
static_assert(kUserY + kRowH <= kPassY, "usuario invade a senha");
static_assert(kPassY + kRowH <= kStatusY, "senha invade a faixa de estado");
static_assert(kStatusY + kStatusH <= kFileY, "faixa de estado invade a linha do arquivo");
static_assert(kFileY + kRowH <= kBarY, "linha do arquivo invade a barra");
static_assert(kBarY + kBarH <= kBytesY, "barra invade a linha de bytes");

} // namespace layout

// ---------------------------------------------------------------------------
//  Utilitarios internos
// ---------------------------------------------------------------------------
namespace detail {

// Metrica da Font0 do M5GFX (GLCDfont 6x8). Usada direto, em vez de
// gfx->textWidth(), para o calculo nao depender de qual fonte o chamador
// deixou ativa — draw() fixa a Font0 antes de qualquer medida.
constexpr int kM5W = 6;
constexpr int kM5H = 8;

// Buffer de texto normalizado. 64 bytes cobrem o pior caso util: um SSID tem no
// maximo 32 caracteres e um nome de arquivo maior que isso ja sai truncado de
// qualquer jeito. Pequeno de proposito — a pilha do loopTask e 8 KB e o
// AGENTS.md conta duas travadas causadas por array grande em pilha (2.3).
constexpr size_t kBuf = 64;

// Normaliza para ASCII no buffer do chamador. Nulo vira string vazia, para o
// resto do desenho nao precisar testar ponteiro a cada linha.
inline const char *norm(char *dst, size_t cap, const char *src) {
  if (!cap)
    return "";
  if (!src) {
    dst[0] = 0;
    return dst;
  }
  ascii::normalize(dst, cap, src);
  return dst;
}

// Desenha na Font0 escolhendo o maior tamanho (<= maxSize) que couber em `w` e,
// se nem o tamanho 1 couber, trunca com "..." no fim. Centraliza verticalmente
// na faixa de altura `h`, para que cair de tamanho 2 para 1 nao desalinhe a
// linha. Devolve a largura consumida.
inline int drawFine(LovyanGFX *gfx, const char *s, int x, int y, int w, int h, int32_t ink,
                    int maxSize) {
  if (!gfx || !s || !*s)
    return 0;
  int len = (int)strlen(s);
  int size = maxSize;
  while (size > 1 && len * kM5W * size > w)
    --size;

  char cut[kBuf];
  const int maxChars = w / (kM5W * size);
  if (len > maxChars) {
    // Trunca com reticencias: um nome de arquivo cortado em seco parece nome
    // de arquivo, e a pessoa procura no computador um arquivo que nao existe.
    int keep = maxChars - 3;
    if (keep < 1)
      keep = maxChars > 0 ? maxChars : 0;
    if (keep > (int)kBuf - 4)
      keep = (int)kBuf - 4;
    memcpy(cut, s, (size_t)keep);
    cut[keep] = 0;
    if (maxChars >= 4)
      strncat(cut, "...", kBuf - strlen(cut) - 1);
    s = cut;
    len = (int)strlen(s);
  }

  gfx->setFont(&fonts::Font0);
  gfx->setTextSize((uint8_t)size);
  gfx->setTextDatum(lgfx::textdatum::top_left);
  gfx->setTextColor(ink); // fore == back no LovyanGFX significa fundo transparente
  gfx->drawString(s, x, y + (h - kM5H * size) / 2);
  gfx->setTextSize(1);
  return len * kM5W * size;
}

// Texto grosso: VcrFont 12x16 quando couber, senao a Font0 pequena, que cabe o
// dobro de caracteres. Degradar para a fonte fina e melhor que truncar um dado
// que a pessoa precisa copiar.
inline void drawThick(LovyanGFX *gfx, const char *s, int x, int y, int w, int h, int32_t ink) {
  if (!gfx || !s || !*s)
    return;
  if (vcrfont::textWidth(s) <= w) {
    vcrfont::drawText(gfx, s, x, y + (h - vcrfont::textHeight()) / 2, ink);
    return;
  }
  drawFine(gfx, s, x, y, w, h, ink, 1);
}

// Idem, centralizado na faixa [x, x + w).
inline void drawThickCentered(LovyanGFX *gfx, const char *s, int x, int y, int w, int h, int32_t ink) {
  if (!gfx || !s || !*s)
    return;
  const int tw = vcrfont::textWidth(s);
  if (tw <= w) {
    vcrfont::drawText(gfx, s, x + (w - tw) / 2, y + (h - vcrfont::textHeight()) / 2, ink);
    return;
  }
  const int len = (int)strlen(s);
  const int fw = len * kM5W;
  drawFine(gfx, s, x + (w > fw ? (w - fw) / 2 : 0), y, w, h, ink, 1);
}

// Bytes em unidade legivel, com uma casa decimal e virgula (pt-BR).
// Aritmetica inteira: %f no newlib-nano do ESP32 depende de flag de link e ja
// imprimiu lixo neste projeto; e a divisao por 1024 aqui e um shift.
inline void formatBytes(char *dst, size_t cap, uint32_t b) {
  if (b < 1024u) {
    snprintf(dst, cap, "%lu B", (unsigned long)b);
  } else if (b < 1024u * 1024u) {
    snprintf(dst, cap, "%lu,%lu KB", (unsigned long)(b >> 10), (unsigned long)(((b & 1023u) * 10u) >> 10));
  } else if (b < 1024u * 1024u * 1024u) {
    const uint32_t k = b >> 10;
    snprintf(dst, cap, "%lu,%lu MB", (unsigned long)(k >> 10), (unsigned long)(((k & 1023u) * 10u) >> 10));
  } else {
    const uint32_t m = b >> 20;
    snprintf(dst, cap, "%lu,%lu GB", (unsigned long)(m >> 10), (unsigned long)(((m & 1023u) * 10u) >> 10));
  }
}

// Rotulo padrao de cada estagio.
inline const char *stageLabel(Stage stage) {
  return stage == Stage::Waiting     ? "AGUARDANDO CONEXAO"
         : stage == Stage::Receiving ? "RECEBENDO ARQUIVO"
         : stage == Stage::Done      ? "CONCLUIDO"
                                     : "ERRO NA TRANSFERENCIA";
}

inline uint16_t stageFill(Stage stage) {
  return stage == Stage::Waiting     ? kWaitFill
         : stage == Stage::Receiving ? kRecvFill
         : stage == Stage::Done      ? kDoneFill
                                     : kErrFill;
}

// Tinta sobre a faixa: marinho sobre ciano/amarelo/verde (claros) e branco
// sobre vermelho (escuro). O criterio e luminancia, nao gosto.
inline uint16_t stageTextInk(Stage stage) {
  return stage == Stage::Error ? kInk : kBg;
}

} // namespace detail

// Porcentagem concluida, ou -1 quando nao da para saber (sem tamanho total).
// Publica porque a integracao costuma querer o mesmo numero no log serial.
inline int percent(const State &s) {
  if (s.stage == Stage::Done)
    return 100; // concluido sem Content-Length ainda e 100%
  if (!s.bytesTotal)
    return -1;
  // 64 bits no meio do caminho: bytesReceived * 100 estoura 32 bits a partir de
  // 43 MB, e um arquivo de video passa disso na primeira tentativa.
  uint64_t p = ((uint64_t)s.bytesReceived * 100u) / s.bytesTotal;
  if (p > 100u)
    p = 100u;
  return (int)p;
}

// ---------------------------------------------------------------------------
//  Desenho
// ---------------------------------------------------------------------------

// Parte que nao muda durante a sessao: cabecalho, rede, endereco, credenciais e
// a faixa de rodape com o aviso. Separada de drawProgress() porque a senha e o
// endereco nao mudam enquanto a tela existe, e repintar tudo a cada pacote
// recebido faria a tela piscar na TV.
inline void drawStatic(LovyanGFX *gfx, const State &s) {
  if (!gfx)
    return;
  char buf[detail::kBuf];
  using namespace layout;

  // Cabecalho, igual ao das outras telas.
  gfx->setFont(&fonts::Font0);
  gfx->setTextDatum(lgfx::textdatum::top_left);
  gfx->setTextSize(2);
  gfx->setTextColor(kInk);
  gfx->drawString(detail::norm(buf, sizeof(buf), s.title ? s.title : kTitle), crt::SAFE_L, kTitleY);
  gfx->setTextSize(1);
  gfx->drawFastHLine(crt::SAFE_L, kRuleY, crt::SAFE_W, kAccent);

  // Rede: contexto, nao dado a copiar — fica pequeno de proposito.
  detail::drawFine(gfx, "REDE", kLabelX, kNetY, kValueX - kLabelX, kNetH, kAccent, 1);
  detail::drawFine(gfx, detail::norm(buf, sizeof(buf), s.ssid), kValueX, kNetY, kValueW, kNetH, kInk, 1);

  // Painel do endereco: o unico elemento com moldura, porque e o unico que a
  // pessoa procura na tela antes de ler qualquer outra coisa.
  gfx->fillRect(crt::SAFE_L, kAddrY, crt::SAFE_W, kAddrH, kPanel);
  gfx->drawRect(crt::SAFE_L, kAddrY, crt::SAFE_W, kAddrH, kAccent);
  detail::drawThickCentered(gfx, detail::norm(buf, sizeof(buf), s.address), crt::SAFE_L + 2, kAddrY + 1,
                            crt::SAFE_W - 4, kAddrH - 2, kInk);

  // Usuario e senha: valores na Font0 tamanho 2, que preserva maiuscula e
  // minuscula. A senha e gerada por sessao; desenha-la em caixa alta seria
  // entregar uma senha errada.
  detail::drawFine(gfx, "USUARIO", kLabelX, kUserY, kValueX - kLabelX, kRowH, kAccent, 1);
  detail::drawFine(gfx, detail::norm(buf, sizeof(buf), s.user), kValueX, kUserY, kValueW, kRowH, kInk, 2);
  detail::drawFine(gfx, "SENHA", kLabelX, kPassY, kValueX - kLabelX, kRowH, kAccent, 1);
  detail::drawFine(gfx, detail::norm(buf, sizeof(buf), s.password), kValueX, kPassY, kValueW, kRowH, kInk,
                   2);

  // Rodape: mesma faixa das legendas das outras telas. Aqui ela carrega o aviso
  // de que o aparelho esta suspenso — a legenda do botao entra no mesmo texto
  // quando existe, porque 20 px nao comportam duas linhas.
  gfx->fillRect(0, crt::BAR_Y, crt::W, crt::BAR_H, kBg);
  char foot[detail::kBuf * 2];
  char warn[detail::kBuf];
  detail::norm(warn, sizeof(warn), s.warning ? s.warning : kWarning);
  if (s.buttonLabel && *s.buttonLabel) {
    char btn[detail::kBuf];
    detail::norm(btn, sizeof(btn), s.buttonLabel);
    snprintf(foot, sizeof(foot), "[ %s ]  %s", btn, warn);
    // Nao cabendo os dois, o aviso tem prioridade: a legenda do botao a pessoa
    // descobre apertando, o aviso nao.
    if ((int)strlen(foot) * detail::kM5W > crt::SAFE_W)
      snprintf(foot, sizeof(foot), "%s", warn);
  } else {
    snprintf(foot, sizeof(foot), "%s", warn);
  }
  const int fw = (int)strlen(foot) * detail::kM5W;
  detail::drawFine(gfx, foot, crt::SAFE_L + (fw < crt::SAFE_W ? (crt::SAFE_W - fw) / 2 : 0), crt::BAR_Y,
                   crt::SAFE_W, crt::BAR_H, kAccent, 1);
}

// Parte viva: faixa de estado, nome do arquivo, porcentagem, barra e bytes.
// Repinta o proprio fundo, entao pode ser chamada sozinha a cada atualizacao do
// progresso sem piscar o resto da tela.
inline void drawProgress(LovyanGFX *gfx, const State &s) {
  if (!gfx)
    return;
  char buf[detail::kBuf];
  using namespace layout;

  // O fundo sangra ate a borda do raster (regra da area segura: so o conteudo
  // fica dentro da caixa), entao a limpeza vai de x=0 a x=W.
  gfx->fillRect(0, kDynamicY, crt::W, kDynamicH, kBg);

  // Faixa de estado. A cor e o primeiro sinal: da para saber de longe que algo
  // deu errado sem conseguir ler a palavra "ERRO".
  const uint16_t fill = detail::stageFill(s.stage);
  gfx->fillRect(crt::SAFE_L, kStatusY, crt::SAFE_W, kStatusH, fill);
  detail::drawThickCentered(gfx, detail::norm(buf, sizeof(buf), s.statusText ? s.statusText
                                                                             : detail::stageLabel(s.stage)),
                            crt::SAFE_L + 2, kStatusY, crt::SAFE_W - 4, kStatusH,
                            detail::stageTextInk(s.stage));

  const int pct = percent(s);

  // Linha do arquivo: nome a esquerda, porcentagem grossa a direita. A
  // porcentagem fica fora da faixa de estado para sobreviver a um statusText
  // personalizado (mensagem de erro, por exemplo).
  if (s.stage == Stage::Waiting) {
    // Sem arquivo ainda: uma dica apagada ocupa o lugar, para a metade de baixo
    // da tela nao parecer defeito.
    detail::drawFine(gfx, kHint, crt::SAFE_L, kFileY, crt::SAFE_W, kRowH, kDim, 2);
  } else {
    detail::drawThick(gfx, detail::norm(buf, sizeof(buf), s.fileName), crt::SAFE_L, kFileY,
                      crt::SAFE_W - kPctW - 8, kRowH, kInk);
    if (pct >= 0) {
      char pctText[8];
      snprintf(pctText, sizeof(pctText), "%d%%", pct);
      const int pw = vcrfont::textWidth(pctText);
      vcrfont::drawText(gfx, pctText, crt::SAFE_R - pw, kFileY, fill);
    }
  }

  // Barra: moldura sempre, para o lugar do progresso existir antes de haver
  // progresso. O miolo tem 2 px de folga de cada lado, senao o preenchimento
  // encosta na moldura e some em RGB332.
  gfx->drawRect(crt::SAFE_L, kBarY, crt::SAFE_W, kBarH, kAccent);
  const int inner = crt::SAFE_W - 4;
  if (pct > 0) {
    int w = (inner * pct) / 100;
    if (w < 1)
      w = 1; // 1% tem de aparecer; zero px passaria por "nao comecou"
    gfx->fillRect(crt::SAFE_L + 2, kBarY + 2, w, kBarH - 4, fill);
  }

  // Bytes: a unica linha da tela que sobrou em corpo pequeno (8 px de altura
  // entre a barra e a faixa de rodape). Sai em branco, e nao no cinza da dica,
  // porque 6x8 em cinza sobre marinho nao sobrevive ao borrao do NTSC. Sem
  // total conhecido, mostra so o que ja chegou — inventar um total seria mentir
  // sobre o prazo.
  if (s.stage != Stage::Waiting) {
    char got[24], all[24];
    detail::formatBytes(got, sizeof(got), s.bytesReceived);
    if (s.bytesTotal) {
      detail::formatBytes(all, sizeof(all), s.bytesTotal);
      snprintf(buf, sizeof(buf), "RECEBIDO %s / %s", got, all);
    } else {
      snprintf(buf, sizeof(buf), "RECEBIDO %s", got);
    }
    detail::drawFine(gfx, buf, crt::SAFE_L, kBytesY, crt::SAFE_W, kBytesH, kInk, 1);
  }
}

// Tela inteira. O fillScreen sangra ate a borda do raster de proposito: tarja
// preta em volta faria a imagem parecer pequena no tubo (SafeArea.h).
inline void draw(LovyanGFX *gfx, const State &s) {
  if (!gfx)
    return;
  gfx->fillScreen(kBg);
  drawStatic(gfx, s);
  drawProgress(gfx, s);
  // Deixa o contexto de texto num estado conhecido: varias telas do firmware
  // desenham logo depois assumindo tamanho 1 e datum top_left.
  gfx->setTextSize(1);
  gfx->setTextDatum(lgfx::textdatum::top_left);
}

} // namespace xfer
