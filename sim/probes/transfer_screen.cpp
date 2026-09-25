// ============================================================================
//  Probe da tela de transferencia por Wi-Fi (include/TransferScreen.h).
//
//  Desenha a tela em todos os estagios — aguardando, recebendo a 0/40/100%,
//  concluido e erro — mais os dois casos que costumam quebrar diagramacao:
//  nome de arquivo longo e SSID acentuado (que TEM de sair sem buracos, ja que
//  as fontes bitmap so tem ASCII). Grava um PNG de cada e, no fim, confere
//  pixel a pixel que nada de conteudo caiu fora da area segura do tubo.
//
//    make -C sim probes && (cd sim && ./build/probe_transfer_screen)
//
//  Desenha num LGFX_Sprite, como o canvas `tv` do firmware, entao nao abre
//  janela nem precisa de SDL em tempo de execucao. Roda de dentro de sim/: os
//  PNGs vao para build/ (armadilha 14 do AGENTS.md).
//
//  Sai com 1 se algum pixel escapar, para servir de porta em CI.
//
//  A regra de probe do Makefile nao depende dos headers: depois de mexer em
//  include/TransferScreen.h, um `touch sim/probes/transfer_screen.cpp` antes
//  do make.
// ============================================================================

#include "fj/Gfx.h" // LovyanGFX, a mesma porta de entrada do firmware (PORTING.md 3.1)

#include <cstdio>
#include <cstdlib>

#include "SafeArea.h"
#include "TransferScreen.h"

// Um LGFX_Sprite 320x240, e nao um painel SDL: no Fruit Jam o canvas `tv` E um
// LGFX_Sprite (o framebuffer do DVI, fj/Display.h), entao desenhar num sprite
// aqui e desenhar exatamente como o aparelho desenha — e sem janela nenhuma,
// o que deixa o probe rodar em CI. O nome `rca` fica por historia.
static LGFX_Sprite rca;

static void savePng(const char *name) {
  size_t len = 0;
  uint8_t *png = (uint8_t *)rca.createPng(&len, 0, 0, crt::W, crt::H);
  if (!png) {
    printf("falha ao gerar %s\n", name);
    return;
  }
  char path[256];
  snprintf(path, sizeof(path), "build/%s", name);
  FILE *f = fopen(path, "wb");
  if (!f) { // rodado fora de sim/: sem isto o fwrite abaixo e segfault mudo
    printf("nao abriu %s (rode de dentro de sim/)\n", path);
    free(png);
    return;
  }
  fwrite(png, 1, len, f);
  fclose(f);
  free(png);
  printf("gravado %s (%zu bytes)\n", path, len);
}

static void shot(const char *name, const xfer::State &s) {
  xfer::draw(&rca, s);
  savePng(name);
}

// ---------------------------------------------------------------------------
//  Conferencia por pixel da area segura
// ---------------------------------------------------------------------------

// A tela pinta o proprio fundo ate a borda do raster, que e o que a regra da
// area segura permite. Entao "conteudo" aqui e todo pixel que NAO ficou da cor
// de fundo — e nenhum deles pode estar fora da caixa segura.
//
// A referencia e lida do framebuffer, nao do literal RGB565: assim a conferencia
// vale em qualquer profundidade de cor, inclusive quando a cor volta quantizada
// do readPixel.
static int checkSafeArea(const char *what, const xfer::State &s) {
  rca.fillScreen(xfer::kBg);
  const uint16_t bg = rca.readPixel(0, 0);
  xfer::draw(&rca, s);

  int minX = crt::W, minY = crt::H, maxX = -1, maxY = -1, outside = 0;
  for (int y = 0; y < crt::H; ++y) {
    for (int x = 0; x < crt::W; ++x) {
      if (rca.readPixel(x, y) == bg)
        continue;
      if (x < minX)
        minX = x;
      if (y < minY)
        minY = y;
      if (x > maxX)
        maxX = x;
      if (y > maxY)
        maxY = y;
      if (x < crt::SAFE_L || x >= crt::SAFE_R || y < crt::SAFE_T || y >= crt::SAFE_B) {
        if (outside < 8)
          printf("  FORA DA AREA SEGURA: (%d,%d)\n", x, y);
        ++outside;
      }
    }
  }
  if (maxX < 0) {
    printf(" %-22s nada desenhado\n", what);
    return 1; // tela em branco tambem e defeito
  }
  printf(" %-22s caixa x %d..%d  y %d..%d  (segura: x %d..%d  y %d..%d)  fora=%d\n", what, minX, maxX,
         minY, maxY, crt::SAFE_L, crt::SAFE_R - 1, crt::SAFE_T, crt::SAFE_B - 1, outside);
  return outside;
}

int main(int, char **) {
  // RGB565, igual ao canvas do DVI (fj/Display.h). O RGB332 do composto
  // do Core2 nao existe mais neste fork (PORTING.md 3.5).
  rca.setColorDepth(16);
  if (!rca.createSprite(crt::W, crt::H))
    return 1;

  // Caso base: o que a pessoa ve assim que o servidor sobe.
  xfer::State waiting;
  waiting.address = "http://192.168.0.12";
  waiting.ssid = "CASA-DO-ELIEL";
  waiting.user = "m5tv";
  waiting.password = "aB7k-9QpD"; // com minusculas: nao pode sair em caixa alta
  waiting.buttonLabel = "VOLTAR";
  waiting.stage = xfer::Stage::Waiting;
  shot("probe_transfer_waiting.png", waiting);

  // Recebendo, nos tres pontos da barra que importam.
  xfer::State recv = waiting;
  recv.stage = xfer::Stage::Receiving;
  recv.fileName = "JORNAL-1987.MJPG";
  recv.bytesTotal = 42u * 1024u * 1024u;
  recv.bytesReceived = 0;
  shot("probe_transfer_recv_000.png", recv);

  recv.bytesReceived = (uint32_t)((uint64_t)recv.bytesTotal * 40u / 100u);
  shot("probe_transfer_recv_040.png", recv);

  recv.bytesReceived = recv.bytesTotal;
  shot("probe_transfer_recv_100.png", recv);

  // Concluido.
  xfer::State done = recv;
  done.stage = xfer::Stage::Done;
  shot("probe_transfer_done.png", done);

  // Erro, com o detalhe na faixa. A porcentagem continua visivel (ela fica fora
  // da faixa justamente para sobreviver a um statusText personalizado).
  xfer::State err = recv;
  err.stage = xfer::Stage::Error;
  err.bytesReceived = (uint32_t)((uint64_t)recv.bytesTotal * 63u / 100u);
  err.statusText = "SEM ESPACO NO CARTAO";
  shot("probe_transfer_error.png", err);

  // Pior caso de texto: nome de arquivo longo (tem de cair para a fonte fina e
  // truncar com reticencias, nunca vazar) e SSID acentuado vindo do settings
  // (tem de sair "REDE DO JOAO - APTO 31", sem buracos duplos).
  xfer::State stress = recv;
  stress.bytesReceived = (uint32_t)((uint64_t)recv.bytesTotal * 77u / 100u);
  stress.fileName = "Especial de Fim de Ano 1994 - Fita 02 - copia final.mjpg";
  stress.ssid = "Rede do Jo\xc3\xa3o - Apto 31 (2,4 GHz)";
  stress.address = "http://192.168.100.130:8080";
  stress.password = "Sen#a-Long@-2026";
  shot("probe_transfer_stress.png", stress);

  // Tamanho total desconhecido (upload sem Content-Length): sem porcentagem e
  // sem barra preenchida, mas com o que ja chegou.
  xfer::State unknown = recv;
  unknown.bytesTotal = 0;
  unknown.bytesReceived = 3u * 1024u * 1024u + 512u * 1024u;
  shot("probe_transfer_unknown.png", unknown);

  // O mesmo desenho num painel de 8 bits (RGB332), que era a saida composta do
  // Core2. O fork so usa RGB565, mas o upstream ainda desenha esta tela em
  // RGB332: este PNG confere que nenhuma cor depende da profundidade.
  rca.setColorDepth(8);
  rca.createSprite(crt::W, crt::H);
  shot("probe_transfer_rgb332.png", recv);
  rca.setColorDepth(16);
  rca.createSprite(crt::W, crt::H);

  printf("\nconferencia da area segura:\n");
  int outside = 0;
  outside += checkSafeArea("aguardando", waiting);
  outside += checkSafeArea("recebendo 40%", recv);
  outside += checkSafeArea("concluido", done);
  outside += checkSafeArea("erro", err);
  outside += checkSafeArea("nome longo + acento", stress);
  outside += checkSafeArea("total desconhecido", unknown);
  printf("\n%s\n", outside ? "FALHOU: a tela escapa da area segura" : "OK: tela inteira dentro da area segura");
  return outside ? 1 : 0;
}
