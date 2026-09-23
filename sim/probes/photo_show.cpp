// ============================================================================
//  probe_photo_show — bancada do include/PhotoShow.h
//
//  Testa a parte que NÃO depende do cartão nem do JPEGDEC, que é justamente a
//  que quebra em silêncio no aparelho:
//
//    1. catálogo: filtro de extensão, nome cru preservado, ordenação por nome
//       sem diferenciar caixa, pool e teto de entradas respeitados;
//    2. escolha do fator de redução (1/2, 1/4, 1/8) a partir das dimensões,
//       incluindo a foto de câmera de 4000x3000 que NEM em 1/8 cabe;
//    3. política de formato: progressivo e 4:4:4 recusados, 4:2:0 e escala de
//       cinza aceitos;
//    4. navegação: avança, volta, dá a volta nas duas pontas;
//    5. prazo com millis() dando a volta — o caso em que a comparação ingênua
//       `now >= prazo` erra e a apresentação congelaria por 49 dias.
//
//  Grava ainda build/probe_photo_show.png com a tela desenhada sem foto (só a
//  legenda e a faixa de botões), para conferir a área segura a olho.
//
//    make -C sim probes && ./sim/build/probe_photo_show
// ============================================================================

#include <SDL2/SDL.h> // antes do M5GFX: define SDL_h_
#include <M5GFX.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "PhotoShow.h"
#include "SafeArea.h"

static int failures = 0;

static void check(bool ok, const char *what) {
  printf("  %s  %s\n", ok ? "ok  " : "FALHA", what);
  if (!ok)
    ++failures;
}

static void checkInt(long got, long want, const char *what) {
  const bool ok = got == want;
  printf("  %s  %-52s got=%ld want=%ld\n", ok ? "ok  " : "FALHA", what, got, want);
  if (!ok)
    ++failures;
}

// ---------------------------------------------------------------------------
//  1. Catálogo
// ---------------------------------------------------------------------------
static char pool[512];
static uint16_t offs[64];

static void testCatalog() {
  printf("\n[1] catalogo: filtro, nome cru e ordenacao\n");
  photo::Catalog cat;
  cat.attachStorage(pool, sizeof(pool), offs, 64);

  // Mistura de propósito: fora de ordem, caixa trocada, caminho completo,
  // extensões erradas, lixo do macOS e um nome acentuado (que tem de ser
  // guardado CRU, senão não abre o arquivo depois).
  const char *feed[] = {"zebra.JPG",
                        "IMG_0002.jpg",
                        "/M5RETRO/fotos/img_0001.jpeg",
                        "anotacao.txt",
                        "video.mjpeg",
                        "._IMG_9999.JPG",
                        "F\xc3\xa9rias.jpg", // "Férias.jpg" em UTF-8
                        "Banana.JPEG",
                        "semextensao",
                        ".jpg"};
  int added = 0;
  for (size_t i = 0; i < sizeof(feed) / sizeof(feed[0]); ++i)
    if (cat.add(feed[i]))
      ++added;

  checkInt(added, 5, "aceitas (jpg/jpeg, sem lixo e sem ._)");
  checkInt(cat.count(), 5, "count()");
  check(!photo::Catalog::isJpeg("video.mjpeg"), ".mjpeg nao e foto");
  check(!photo::Catalog::isJpeg("._IMG_9999.JPG"), "recurso do macOS ignorado");
  check(photo::Catalog::isJpeg("a.JPEG"), ".JPEG aceito");
  check(strcmp(photo::Catalog::baseName("/M5RETRO/fotos/x.jpg"), "x.jpg") == 0,
        "baseName() descarta o diretorio");

  cat.sort();
  const char *want[] = {"Banana.JPEG", "F\xc3\xa9rias.jpg", "img_0001.jpeg", "IMG_0002.jpg",
                        "zebra.JPG"};
  bool sorted = true;
  printf("      ordem: ");
  for (int i = 0; i < cat.count(); ++i) {
    printf("%s%s", i ? " | " : "", cat.name(i));
    if (strcmp(cat.name(i), want[i]) != 0)
      sorted = false;
  }
  printf("\n");
  check(sorted, "ordenado por nome, sem diferenciar caixa");

  // O nome guardado tem de ser o CRU (com o acento em UTF-8); a normalizacao
  // so acontece na hora de desenhar.
  check(strcmp(cat.name(1), "F\xc3\xa9rias.jpg") == 0, "nome guardado e o cru (abre o arquivo)");
  char shown[32];
  ascii::normalizeUpper(shown, sizeof(shown), cat.name(1));
  printf("      desenhado como: \"%s\"\n", shown);
  check(strcmp(shown, "FERIAS.JPG") == 0, "desenhado em ASCII maiusculo");

  char p[128];
  check(cat.path(0, "/M5RETRO/fotos", p, sizeof(p)) &&
            strcmp(p, "/M5RETRO/fotos/Banana.JPEG") == 0,
        "path() monta o caminho completo so na hora");
  printf("      path[0] = %s\n", p);

  // Teto de entradas: com maxEntries=4 a quinta e descartada e contabilizada.
  photo::Catalog small;
  static char spool[128];
  static uint16_t soffs[4];
  small.attachStorage(spool, sizeof(spool), soffs, 4);
  for (int i = 0; i < 9; ++i) {
    char n[16];
    snprintf(n, sizeof(n), "p%d.jpg", i);
    small.add(n);
  }
  checkInt(small.count(), 4, "teto de entradas respeitado");
  checkInt(small.dropped(), 5, "descartadas contabilizadas");
}

// ---------------------------------------------------------------------------
//  2. Fator de reducao
// ---------------------------------------------------------------------------
static void testFit() {
  printf("\n[2] escolha do fator de reducao (JPEGDEC: 1, 1/2, 1/4, 1/8)\n");
  struct Case {
    int w, h;
    int scale;
    int outW, outH;
    bool fits;
    const char *what;
  };
  const Case cases[] = {
      {64, 64, 1, 64, 64, true, "miniatura 64x64"},
      {320, 240, 1, 320, 240, true, "ja no tamanho do quadro"},
      // Antes esta linha esperava 1/2 = 161x120. Nao da: a mesma tabela pede
      // 1/4 (341x270, recortada) para 1364x1080, e as duas regras se excluem.
      // fitFor() reduz enquanto a imagem cobrir o quadro e recorta o excesso,
      // entao 321x240 fica em 1/1 e perde 1 px de largura, em vez de encolher
      // para pouco mais de um quarto do quadro.
      {321, 240, 1, 321, 240, false, "1 px maior fica em 1/1 e recorta"},
      {640, 480, 2, 320, 240, true, "VGA -> 1/2"},
      {1280, 960, 4, 320, 240, true, "1280x960 -> 1/4"},
      {1364, 1080, 4, 341, 270, false, "1364x1080 -> 1/4, ainda estoura"},
      {2560, 1920, 8, 320, 240, true, "2560x1920 -> 1/8 exato"},
      {4000, 3000, 8, 500, 375, false, "foto de camera 4000x3000"},
      {4000, 3168, 8, 500, 396, false, "foto de camera 4000x3168 (medida)"},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    const photo::Fit f = photo::fitFor(cases[i].w, cases[i].h);
    const bool ok = f.scale == cases[i].scale && f.w == cases[i].outW && f.h == cases[i].outH &&
                    f.fits == cases[i].fits;
    printf("  %s  %-34s %4dx%-4d -> 1/%d = %3dx%-3d  option=%d  cabe=%s\n", ok ? "ok  " : "FALHA",
           cases[i].what, cases[i].w, cases[i].h, f.scale, f.w, f.h, f.option,
           f.fits ? "sim" : "NAO (recorta no centro)");
    if (!ok)
      ++failures;
  }
  // A opcao passada ao decode() tem de bater com as constantes do JPEGDEC:
  // JPEG_SCALE_HALF=2, QUARTER=4, EIGHTH=8, e escala 1 = opcao 0.
  checkInt(photo::fitFor(640, 480).option, 2, "option == JPEG_SCALE_HALF");
  checkInt(photo::fitFor(1280, 960).option, 4, "option == JPEG_SCALE_QUARTER");
  checkInt(photo::fitFor(4000, 3000).option, 8, "option == JPEG_SCALE_EIGHTH");
  checkInt(photo::fitFor(200, 100).option, 0, "sem reducao == opcao 0");

  // Origem do desenho para o caso que nao cabe: negativa, e o callback/pushImage
  // e que recortam. E a conta que o decodeInto() faz.
  const photo::Fit big = photo::fitFor(4000, 3168);
  const int x0 = (crt::W - big.w) / 2, y0 = (crt::H - big.h) / 2;
  printf("      4000x3168: origem do blit = (%d, %d), bloco maximo em x=%d y=%d\n", x0, y0,
         x0 + big.w, y0 + big.h);
  check(x0 < 0 && y0 < 0, "foto maior que o quadro entra com origem negativa");
}

// ---------------------------------------------------------------------------
//  3. Politica de formato
// ---------------------------------------------------------------------------
static void testSupport() {
  printf("\n[3] formatos que ESTE JPEGDEC decodifica\n");
  // getJPEGType(): 0 = baseline, 1 = progressivo. getSubSample(): 0 = cinza,
  // 0x11 = 4:4:4, 0x21/0x12 = 4:2:2, 0x22 = 4:2:0.
  check(photo::supportFor(0, 0x22) == photo::SUP_OK, "baseline 4:2:0 aceito (o formato do cartao)");
  check(photo::supportFor(0, 0x21) == photo::SUP_OK, "baseline 4:2:2 aceito");
  check(photo::supportFor(0, 0) == photo::SUP_OK, "escala de cinza aceita (subsample 0, nao 0x11)");
  check(photo::supportFor(0, 0x11) == photo::SUP_SUBSAMPLE, "4:4:4 recusado (decode morre no meio)");
  check(photo::supportFor(1, 0x22) == photo::SUP_PROGRESSIVE,
        "progressivo recusado (decodificaria so 1/8, sem erro)");
  check(photo::supportFor(1, 0x11) == photo::SUP_PROGRESSIVE, "progressivo tem precedencia");
  printf("      mensagens: \"%s\" / \"%s\" / \"%s\"\n", photo::message(photo::RES_PROGRESSIVE),
         photo::message(photo::RES_SUBSAMPLE), photo::message(photo::RES_TOO_BIG));
}

// ---------------------------------------------------------------------------
//  4. Navegacao
// ---------------------------------------------------------------------------
static char pool2[512];
static uint16_t offs2[64];

static void feed(photo::Show &s, int n) {
  s.catalog().attachStorage(pool2, sizeof(pool2), offs2, 64);
  for (int i = 0; i < n; ++i) {
    char nm[24];
    snprintf(nm, sizeof(nm), "IMG_%04d.JPG", n - i); // de tras para frente, de proposito
    s.catalog().add(nm);
  }
  s.afterScan(1000);
}

static void testNavigation() {
  printf("\n[4] navegacao: avanca, volta e da a volta\n");
  photo::Show s;
  feed(s, 3);
  checkInt(s.count(), 3, "catalogo com 3 fotos");
  check(strcmp(s.rawName(), "IMG_0001.JPG") == 0, "afterScan() ordena e comeca na primeira");

  s.advance(1, 1000);
  checkInt(s.index(), 1, "proxima");
  s.advance(1, 1000);
  checkInt(s.index(), 2, "proxima");
  s.advance(1, 1000);
  checkInt(s.index(), 0, "da a volta no fim");
  s.advance(-1, 1000);
  checkInt(s.index(), 2, "anterior da a volta no comeco (sem indice negativo)");
  s.advance(-5, 1000);
  checkInt(s.index(), 0, "salto negativo grande continua na faixa");
  s.advance(7, 1000);
  checkInt(s.index(), 1, "salto positivo grande continua na faixa");

  checkInt(s.onButton(2, 1000), photo::Show::ACT_LOAD, "botao PROXIMA pede leitura do cartao");
  checkInt(s.index(), 2, "botao PROXIMA avancou");
  checkInt(s.onButton(0, 1000), photo::Show::ACT_LOAD, "botao ANTERIOR pede leitura do cartao");
  checkInt(s.index(), 1, "botao ANTERIOR voltou");
  checkInt(s.onButton(1, 1000), photo::Show::ACT_REPAINT, "botao TEMPO so repinta");

  // Catalogo vazio: nao pode estourar indice nem pedir avanco eterno.
  photo::Show empty;
  empty.afterScan(0);
  checkInt(empty.count(), 0, "catalogo vazio");
  empty.advance(1, 0);
  checkInt(empty.index(), 0, "avancar com zero foto nao mexe no indice");
  check(!empty.dueForNext(999999), "com zero foto o prazo nunca vence (nao gira a toa)");
  check(strcmp(empty.statusText(), "SEM FOTOS NO CARTAO") == 0, "mensagem da pasta vazia");
}

// ---------------------------------------------------------------------------
//  5. Prazo e a volta do millis()
// ---------------------------------------------------------------------------
static void testDeadline() {
  printf("\n[5] prazo com millis() dando a volta\n");
  photo::Show s;
  feed(s, 3);

  checkInt((long)s.dwellMs(), 5000, "prazo inicial (5 s)");
  s.cycleDwell(0);
  checkInt((long)s.dwellMs(), 10000, "TEMPO -> 10 s");
  s.cycleDwell(0);
  checkInt((long)s.dwellMs(), 30000, "TEMPO -> 30 s");
  s.cycleDwell(0);
  checkInt((long)s.dwellMs(), 3000, "TEMPO -> 3 s (volta ao comeco)");
  s.cycleDwell(0);
  checkInt((long)s.dwellMs(), 5000, "TEMPO -> 5 s");

  // Caso normal.
  s.resetTimer(1000);
  checkInt((long)s.deadline(), 6000, "prazo = agora + 5000");
  check(!s.dueForNext(5999), "antes do prazo nao avanca");
  check(s.dueForNext(6000), "no prazo avanca");
  check(s.dueForNext(6001), "depois do prazo avanca");

  // A volta: 0xFFFFF000 esta a 0x1000 = 4096 ms do fim do contador, entao um
  // prazo de 5000 ms cai 5000 - 4096 = 904 ms DEPOIS da virada, em 0x00000388.
  // (A conta anterior aqui dizia 0x00000E88, que nao fecha: 0xE88 = 3720.)
  const uint32_t nearWrap = 0xFFFFF000u;
  s.resetTimer(nearWrap);
  const uint32_t deadline = s.deadline();
  printf("      agora=0x%08X  prazo=0x%08X (deu a volta)\n", nearWrap, deadline);
  checkInt((long)deadline, 0x00000388, "o prazo deu a volta, como o millis()");

  // 2 s depois da virada do relogio, mas ainda 3 s ANTES do prazo.
  const uint32_t before = nearWrap + 2000; // 0x000007D0
  const bool naive = before >= deadline;   // a comparacao errada
  printf("      teste ingenuo `agora >= prazo` em 0x%08X: %s\n", before,
         naive ? "true (ERRADO)" : "false");
  check(!s.dueForNext(before), "timeReached() nao avanca cedo demais depois da volta");
  check(s.dueForNext(deadline), "timeReached() avanca no prazo, ja do outro lado da volta");
  check(s.dueForNext(deadline + 1), "timeReached() avanca depois do prazo");
  check(!s.dueForNext(nearWrap + 1), "1 ms depois de armar o prazo, ainda nao");
}

// ---------------------------------------------------------------------------
//  6. Desenho: legenda e faixa de botoes dentro da area segura
// ---------------------------------------------------------------------------
static lgfx::Panel_sdl panel;
static M5GFX rca;

static void testPaint() {
  printf("\n[6] desenho (sem JPEGDEC no desktop: sai a tela de recado)\n");
  check(photo::CAPTION_Y >= crt::SAFE_T, "legenda comeca dentro da area segura");
  check(photo::CAPTION_Y + photo::CAPTION_H <= crt::SAFE_B, "legenda termina dentro da area segura");
  check(crt::BAR_Y + crt::BAR_H <= crt::SAFE_B, "faixa de botoes dentro da area segura");
  check(photo::CAPTION_Y + photo::CAPTION_H <= crt::BAR_Y, "legenda nao invade a faixa de botoes");
  check(photo::ACCENT != 0x07FF, "o acento nao e ciano saturado (dot crawl)");

  panel.setScaling(2, 2);
  rca.setPanel(&panel);
  if (!rca.init()) {
    printf("  (sem video SDL: PNG nao gravado, o resto dos testes vale)\n");
    return;
  }
  rca.setColorDepth(16); // o framebuffer da RCA agora e RGB565

  photo::Show s;
  feed(s, 87);
  s.setIndex(11, 0);
  s.paint(&rca, 0, 0);

  size_t len = 0;
  uint8_t *png = (uint8_t *)rca.createPng(&len, 0, 0, crt::W, crt::H);
  if (png) {
    FILE *f = fopen("build/probe_photo_show.png", "wb");
    if (f) {
      fwrite(png, 1, len, f);
      fclose(f);
      printf("  ok    build/probe_photo_show.png (%d bytes)\n", (int)len);
    }
    free(png);
  }
}

int main(int, char **) {
  printf("probe_photo_show — include/PhotoShow.h\n");
  testCatalog();
  testFit();
  testSupport();
  testNavigation();
  testDeadline();
  testPaint();
  printf("\n%s (%d falha%s)\n", failures ? "FALHOU" : "TUDO CERTO", failures,
         failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
