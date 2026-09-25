// ============================================================================
//  Probe do include/Subtitles.h — bancada do parser .srt e do desenho do CC.
//
//  O ponto e provar, sem cartao SD e sem aparelho, que:
//    * o parser tolera BOM, CRLF, numero de sequencia ausente/lixo, etiquetas
//      <i>/{\an8}, acento portugues e a ultima linha sem quebra;
//    * cue malformada, de duracao zero ou fora de ordem e DESCARTADA, e o
//      arquivo continua sendo lido depois dela;
//    * a cue certa esta no ar em cada instante do relogio de audio;
//    * linha maior que a largura segura quebra na palavra e o excesso vira "...";
//    * o desenho fica dentro da area segura e ACIMA de crt::BAR_Y;
//    * lixo binario nao trava nem derruba o parser;
//    * o mesmo header serve a um buffer de memoria E a um std::istream — e a
//      razao de o `Stream` ser template, igual ao playback::MjpegReader.
//
//  Compila nos dois dialetos que importam:
//    g++ -std=gnu++11 ...   (o do firmware; e o que este arquivo roda)
//    make -C sim probes     (C++17, junto com os outros probes)
//
//  Nao abre janela: desenha num LGFX_Sprite e le os pixels de volta.
// ============================================================================

#include "fj/Gfx.h" // LovyanGFX + backend SDL, a mesma porta de entrada do firmware

#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>

#include "SafeArea.h"
#include "Subtitles.h"

// ---------------------------------------------------------------------------
//  Asserts que contam
// ---------------------------------------------------------------------------
static int failures = 0, checks = 0;

static void ok(bool cond, const char *what) {
  ++checks;
  if (!cond) {
    ++failures;
    printf("  FALHOU: %s\n", what);
  }
}

static void okStr(const char *got, const char *want, const char *what) {
  ++checks;
  if (strcmp(got, want)) {
    ++failures;
    printf("  FALHOU: %s\n    esperado [%s]\n    obtido   [%s]\n", what, want, got);
  }
}

// ---------------------------------------------------------------------------
//  Duas fontes de bytes com o mesmo contrato do File do Arduino:
//  size_t read(uint8_t *, size_t). E so isso que o Subtitles exige.
// ---------------------------------------------------------------------------
class MemStream {
  const uint8_t *data_;
  size_t size_, pos_;

public:
  MemStream(const void *d, size_t n) : data_((const uint8_t *)d), size_(n), pos_(0) {}
  size_t read(uint8_t *dst, size_t n) {
    const size_t left = size_ - pos_;
    if (n > left)
      n = left;
    memcpy(dst, data_ + pos_, n);
    pos_ += n;
    return n;
  }
  bool seek(size_t p) {
    pos_ = p;
    return true;
  }
};

class IStreamSource {
  std::istream &in_;

public:
  explicit IStreamSource(std::istream &in) : in_(in) {}
  size_t read(uint8_t *dst, size_t n) {
    in_.read((char *)dst, (std::streamsize)n);
    return (size_t)in_.gcount();
  }
};

// ---------------------------------------------------------------------------
//  O .srt de teste. Construido em std::string so para o literal ficar legivel:
//  o parser nunca ve um std::string, so os bytes.
//
//  \xEF\xBB\xBF   BOM UTF-8
//  \r\n           terminadores do Windows na maior parte do arquivo
//  acentos        UTF-8 de verdade (a, c, a, e -> A, C, A, E)
// ---------------------------------------------------------------------------
static std::string sampleSrt() {
  std::string s;
  s += "\xEF\xBB\xBF"; // BOM
  s += "1\r\n";
  s += "00:00:01,000 --> 00:00:03,500\r\n";
  s += "Bom dia, voc\xC3\xAA est\xC3\xA1\r\n"; // "Bom dia, você está"
  s += "assistindo \xC3\xA0 TV.\r\n";         // "assistindo à TV."
  s += "\r\n";
  // Sem numero de sequencia, com etiqueta de posicao e de italico.
  s += "00:00:04,000 --> 00:00:06,000\r\n";
  s += "{\\an8}<i>A\xC3\xA7\xC3\xA3o e cora\xC3\xA7\xC3\xA3o</i>\r\n"; // "Ação e coração"
  s += "\r\n";
  s += "\r\n"; // linha em branco de sobra
  // Cue malformada: tem a seta, mas o tempo nao parseia.
  s += "3\r\n";
  s += "00:00:0X,000 --> lixo\r\n";
  s += "Nunca deve aparecer\r\n";
  s += "\r\n";
  // Cue fora de ordem: comeca antes da cue 2, que ja passou. Leitor so-para-a-
  // frente nunca poderia exibi-la, entao tem que ser descartada.
  s += "00:00:02,000 --> 00:00:02,500\r\n";
  s += "Fora de ordem\r\n";
  s += "\r\n";
  // Duracao zero.
  s += "00:00:08,000 --> 00:00:08,000\r\n";
  s += "Duracao zero\r\n";
  s += "\r\n";
  // Linha unica bem maior que a largura segura: quebra + corte.
  s += "4\r\n";
  s += "00:00:10,000 --> 00:00:14,000\r\n";
  s += "Esta legenda \xC3\xA9 propositalmente muito mais longa do que cabe na largura "
       "segura da tela e precisa ser quebrada\r\n";
  s += "\r\n";
  // LF puro e sem quebra de linha no fim do arquivo.
  s += "5\n";
  s += "00:00:15,000 --> 00:00:17,000\n";
  s += "FIM";
  return s;
}

// ---------------------------------------------------------------------------
//  Roda o relogio de 0 a 18 s em passos de 100 ms, como o loop() faria, e
//  confere a cue no ar nos instantes que importam.
// ---------------------------------------------------------------------------
template <class Stream> static void timeline(Stream &src, const char *label) {
  printf("[timeline: %s]\n", label);
  subs::Subtitles sub;
  sub.reset();

  struct Expect {
    uint32_t ms;
    bool active;
    const char *l0;
    const char *l1;
  };
  static const Expect kExpect[] = {
      {0, false, "", ""},
      {999, false, "", ""},
      {1000, true, "BOM DIA, VOCE ESTA", "ASSISTINDO A TV."},
      {2500, true, "BOM DIA, VOCE ESTA", "ASSISTINDO A TV."},
      {3499, true, "BOM DIA, VOCE ESTA", "ASSISTINDO A TV."},
      {3500, false, "", ""}, // fim exclusivo
      {4000, true, "ACAO E CORACAO", ""},
      {5900, true, "ACAO E CORACAO", ""},
      {6000, false, "", ""},
      {6500, false, "", ""}, // buraco: a malformada nao pode preencher
      {8100, false, "", ""}, // duracao zero descartada
      {9999, false, "", ""},
      {10000, true, "ESTA LEGENDA E", "PROPOSITALMENTE MU..."},
      {13900, true, "ESTA LEGENDA E", "PROPOSITALMENTE MU..."},
      {14000, false, "", ""},
      {15000, true, "FIM", ""},
      {16999, true, "FIM", ""},
      {17000, false, "", ""},
  };
  const size_t kN = sizeof(kExpect) / sizeof(kExpect[0]);
  size_t e = 0;
  uint32_t lastRev = 0;
  int revChanges = 0;

  for (uint32_t ms = 0; ms <= 18000; ms += 100) {
    sub.update(src, ms); // <- no firmware, com o sdMutex na mao
    const uint32_t rev = sub.revision(ms);
    if (rev != lastRev) {
      ++revChanges;
      lastRev = rev;
    }
    while (e < kN && kExpect[e].ms < ms + 100 && kExpect[e].ms >= ms) {
      const Expect &x = kExpect[e++];
      // O relogio anda de 100 em 100 ms; avalia no instante exato pedido.
      char what[96];
      snprintf(what, sizeof(what), "%u ms: atividade", (unsigned)x.ms);
      ok(sub.active(x.ms) == x.active, what);
      if (x.active) {
        snprintf(what, sizeof(what), "%u ms: linha 0", (unsigned)x.ms);
        okStr(sub.line(0), x.l0, what);
        snprintf(what, sizeof(what), "%u ms: linha 1", (unsigned)x.ms);
        okStr(sub.line(1), x.l1, what);
        snprintf(what, sizeof(what), "%u ms: revision != 0", (unsigned)x.ms);
        ok(sub.revision(x.ms) != 0, what);
      } else {
        snprintf(what, sizeof(what), "%u ms: revision == 0", (unsigned)x.ms);
        ok(sub.revision(x.ms) == 0, what);
      }
    }
  }
  ok(e == kN, "todos os instantes esperados foram avaliados");
  ok(sub.finished(), "chegou ao fim do arquivo");
  // 4 cues validas -> 4 subidas + 4 descidas de revision.
  printf("  cues aceitas (trocas de revision): %d\n", revChanges);
  ok(revChanges == 8, "exatamente 4 cues validas entraram e sairam do ar");
}

// ---------------------------------------------------------------------------
//  Etiquetas, acentos e a ultima linha sem quebra, isolados.
// ---------------------------------------------------------------------------
static void tagsAreStripped() {
  printf("[etiquetas e acentos]\n");
  static const char kSrt[] = "1\n"
                             "00:00:00,000 --> 00:00:05,000\n"
                             "<i>Cora\xC3\xA7\xC3\xA3o</i> {\\an8}de <b>a\xC3\xA7\xC3\xBA""car</b>\n";
  MemStream src(kSrt, sizeof(kSrt) - 1);
  subs::Subtitles sub;
  sub.update(src, 0);
  ok(sub.active(0), "cue sem linha em branco final entra no ar");
  okStr(sub.line(0), "CORACAO DE ACUCAR", "etiquetas removidas e acentos rebaixados");
  ok(sub.lineCount() == 1, "uma linha so");
}

// ---------------------------------------------------------------------------
//  Tolerancia: numero de sequencia colado no carimbo, fracao de 1 e 2 casas,
//  ponto no lugar da virgula.
// ---------------------------------------------------------------------------
static void toleratesOddTimestamps() {
  printf("[carimbos tortos]\n");
  static const char kSrt[] = "7 00:00:01.5 --> 00:00:02,05\n"
                             "OI\n"
                             "\n";
  MemStream src(kSrt, sizeof(kSrt) - 1);
  subs::Subtitles sub;
  sub.update(src, 1500);
  ok(sub.active(1500), "1,5 s: no ar");
  ok(sub.cueStartMs() == 1500, "fracao de 1 casa vira 500 ms");
  ok(sub.cueEndMs() == 2050, "fracao de 2 casas vira 50 ms");
  ok(!sub.active(2050), "fim exclusivo respeitado");
}

// ---------------------------------------------------------------------------
//  Falhar em silencio: lixo binario, arquivo vazio, so BOM.
// ---------------------------------------------------------------------------
static void garbageFailsSafe() {
  printf("[entrada invalida]\n");
  uint8_t noise[3000];
  for (size_t i = 0; i < sizeof(noise); ++i)
    noise[i] = (uint8_t)(i * 37u + (i >> 3)); // determinista, sem \n regular
  {
    MemStream src(noise, sizeof(noise));
    subs::Subtitles sub;
    for (uint32_t ms = 0; ms < 5000; ms += 100)
      sub.update(src, ms);
    ok(!sub.active(1000), "lixo binario nao produz legenda");
    ok(sub.finished(), "lixo binario chega ao fim sem travar");
  }
  {
    MemStream src("", 0);
    subs::Subtitles sub;
    sub.update(src, 0);
    ok(!sub.active(0), "arquivo vazio: sem legenda");
    ok(sub.finished(), "arquivo vazio termina de imediato");
  }
  {
    MemStream src("\xEF\xBB\xBF", 3);
    subs::Subtitles sub;
    sub.update(src, 0);
    ok(!sub.active(0), "arquivo so com BOM: sem legenda");
    ok(sub.finished(), "arquivo so com BOM termina");
  }
  {
    // Linha unica gigantesca, sem quebra: tem que ser consumida, nao acumulada.
    std::string huge(9000, 'X');
    huge += "\n00:00:01,000 --> 00:00:02,000\nDEPOIS\n";
    MemStream src(huge.data(), huge.size());
    subs::Subtitles sub;
    for (uint32_t ms = 0; ms < 2000; ms += 100)
      sub.update(src, ms);
    ok(sub.active(1500), "cue depois de uma linha de 9 KB ainda e encontrada");
    okStr(sub.line(0), "DEPOIS", "texto da cue depois da linha gigante");
  }
}

// ---------------------------------------------------------------------------
//  reset() + seek(0): o unico jeito de voltar num leitor so-para-a-frente.
// ---------------------------------------------------------------------------
static void rewindNeedsReset() {
  printf("[voltar exige reset]\n");
  const std::string srt = sampleSrt();
  MemStream src(srt.data(), srt.size());
  subs::Subtitles sub;
  for (uint32_t ms = 0; ms <= 16000; ms += 100)
    sub.update(src, ms);
  ok(sub.active(16000), "no fim do arquivo, FIM esta no ar");
  sub.update(src, 1000);
  ok(!sub.active(1000), "sem reset, voltar no tempo nao ressuscita a cue 1");
  src.seek(0);
  sub.reset();
  for (uint32_t ms = 0; ms <= 1500; ms += 100)
    sub.update(src, ms);
  ok(sub.active(1000), "com seek(0)+reset(), a cue 1 volta");
  okStr(sub.line(0), "BOM DIA, VOCE ESTA", "texto da cue 1 depois do reset");
}

// ---------------------------------------------------------------------------
//  makeSrtPath
// ---------------------------------------------------------------------------
static void pathHelper() {
  printf("[makeSrtPath]\n");
  char out[64];
  ok(subs::makeSrtPath("/M5RETRO/videos/foo/video.mjpeg", out, sizeof(out)), "caminho normal");
  okStr(out, "/M5RETRO/videos/foo/video.srt", "extensao trocada");
  ok(subs::makeSrtPath("/M5RETRO/videos/sem.ponto/video", out, sizeof(out)), "sem extensao");
  okStr(out, "/M5RETRO/videos/sem.ponto/video.srt", "ponto de pasta nao conta como extensao");
  char tiny[8];
  ok(!subs::makeSrtPath("/M5RETRO/videos/foo/video.mjpeg", tiny, sizeof(tiny)), "buffer curto recusa");
}

// ---------------------------------------------------------------------------
//  Desenho: geometria, cores e o limite da barra dos botoes.
// ---------------------------------------------------------------------------
static void drawing() {
  printf("[desenho]\n");
  lgfx::LGFX_Sprite canvas;
  canvas.setColorDepth(16);
  if (!canvas.createSprite(crt::W, crt::H)) {
    printf("  FALHOU: sprite 320x240 nao alocou\n");
    ++failures;
    return;
  }
  canvas.fillSprite(0x39E7); // cinza medio: qualquer preto na tela veio de nos

  const std::string srt = sampleSrt();
  MemStream src(srt.data(), srt.size());
  subs::Subtitles sub;
  for (uint32_t ms = 0; ms <= 1000; ms += 100)
    sub.update(src, ms);
  ok(sub.active(1000), "cue 1 no ar para desenhar");

  sub.draw(&canvas, 0, 0, 1000);
  const subs::Rect d = sub.dirty();
  printf("  dirty = x%d y%d w%d h%d   (BAR_Y=%d SAFE_L=%d SAFE_R=%d SAFE_T=%d)\n", d.x, d.y, d.w, d.h,
         crt::BAR_Y, crt::SAFE_L, crt::SAFE_R, crt::SAFE_T);
  ok(d.w > 0 && d.h > 0, "retangulo sujo nao vazio");
  ok(d.y >= crt::SAFE_T, "topo dentro da area segura");
  ok(d.y + d.h <= crt::BAR_Y, "base acima da barra dos botoes");
  ok(d.x >= crt::SAFE_L, "esquerda dentro da area segura");
  ok(d.x + d.w <= crt::SAFE_R, "direita dentro da area segura");
  ok(d.h == 2 * subs::layout::kLineH, "duas linhas ocupam duas celulas");

  // Nenhum pixel pintado pode cair na faixa dos botoes nem fora da area segura.
  int painted = 0, ink = 0, box = 0, outside = 0, onBar = 0;
  for (int y = 0; y < crt::H; ++y)
    for (int x = 0; x < crt::W; ++x) {
      const uint16_t c = canvas.readPixel(x, y);
      if (c == 0x39E7)
        continue;
      ++painted;
      if (c == subs::kInk)
        ++ink;
      else if (c == subs::kBox)
        ++box;
      if (y >= crt::BAR_Y)
        ++onBar;
      if (x < crt::SAFE_L || x >= crt::SAFE_R || y < crt::SAFE_T || y >= crt::SAFE_B)
        ++outside;
    }
  printf("  pixels: total=%d branco=%d preto=%d  na barra=%d  fora da area segura=%d\n", painted, ink, box,
         onBar, outside);
  ok(painted > 0, "algo foi pintado");
  ok(ink > 0, "ha tinta branca");
  ok(box > 0, "ha caixa preta");
  ok(painted == ink + box, "so branco e preto — nenhum croma na saida composta");
  ok(onBar == 0, "nada invadiu a barra dos botoes");
  ok(outside == 0, "nada escapou da area segura");

  // clear() devolve o fundo e zera o retangulo sujo.
  sub.clear(&canvas, 0, 0, 0x39E7);
  int leftovers = 0;
  for (int y = 0; y < crt::H; ++y)
    for (int x = 0; x < crt::W; ++x)
      if (canvas.readPixel(x, y) != 0x39E7)
        ++leftovers;
  ok(leftovers == 0, "clear() apagou tudo que draw() pintou");
  ok(sub.dirty().w == 0, "clear() zerou o retangulo sujo");

  // Sem cue no ar, draw() nao pinta e NAO mexe no retangulo sujo.
  sub.draw(&canvas, 0, 0, 3500);
  ok(sub.dirty().w == 0, "draw() sem cue nao suja nada");
  int afterIdle = 0;
  for (int y = 0; y < crt::H; ++y)
    for (int x = 0; x < crt::W; ++x)
      if (canvas.readPixel(x, y) != 0x39E7)
        ++afterIdle;
  ok(afterIdle == 0, "draw() sem cue nao pinta nada");

  // Deslocamento ox/oy da convencao dos pintores.
  sub.draw(&canvas, 0, -8, 1000);
  const subs::Rect shifted = sub.dirty();
  ok(shifted.y == d.y - 8, "oy desloca o retangulo sujo");
  canvas.deleteSprite();
}

// ---------------------------------------------------------------------------
int main() {
  printf("=== probe Subtitles ===\n");
  printf("sizeof(subs::Subtitles) = %u bytes (teto 1024)\n", (unsigned)sizeof(subs::Subtitles));
  printf("kMaxCols=%d kLineH=%d kTop=%d kBottom=%d BAR_Y=%d\n\n", subs::layout::kMaxCols,
         subs::layout::kLineH, subs::layout::kTop, subs::layout::kBottom, crt::BAR_Y);
  ok(sizeof(subs::Subtitles) <= 1024, "footprint abaixo de 1 KB");

  const std::string srt = sampleSrt();
  {
    MemStream src(srt.data(), srt.size());
    timeline(src, "MemStream");
  }
  {
    std::istringstream in(srt);
    IStreamSource src(in);
    timeline(src, "std::istream");
  }
  tagsAreStripped();
  toleratesOddTimestamps();
  garbageFailsSafe();
  rewindNeedsReset();
  pathHelper();
  drawing();

  printf("\n%d verificacoes, %d falhas\n", checks, failures);
  return failures ? 1 : 0;
}
