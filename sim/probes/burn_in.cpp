// ============================================================================
//  probe_burn_in — bancada do include/BurnIn.h
//
//  O BurnIn.h nao desenha nada: e relogio, maquina de estagios e geometria. E
//  exatamente isso que um PNG nao pega e que so aparece na TV depois de horas.
//  Entao aqui nao ha SDL nem M5GFX — so um relogio sintetico e asserts.
//
//  O que e verificado:
//    1. a geometria da deriva sai mesmo de crt::SAFE_* e a caixa interna
//       aguenta qualquer deslocamento sem vazar da area segura;
//    2. o passeio da deriva cobre as 35 posicoes e nunca anda mais de 1 px;
//    3. a escalada dos estagios nos limiares configurados;
//    4. o retorno ao ativo em qualquer acao do usuario, de qualquer estagio;
//    5. uma corrida longa (7 dias simulados) com o deslocamento sempre dentro
//       dos limites derivados;
//    6. a bandeira de repintura disparando exatamente quando o deslocamento
//       (ou o estagio) muda — nem mais, nem menos;
//    7. a volta do millis() em 2^32, atravessada durante a ociosidade;
//    8. dim565 (com a armadilha do uint32_t) e os limites do protetor de tela.
//
//  Compila junto com os demais:
//    make -C sim probes && cd sim && ./build/probe_burn_in
//
//  Nao grava arquivo nenhum, entao pode rodar de qualquer diretorio.
// ============================================================================

#include "BurnIn.h"

#include <cstdio>
#include <cstring>

static int checagens = 0, falhas = 0;

static void check(bool ok, const char *nome) {
  checagens++;
  if (!ok)
    falhas++;
  std::printf("%-62s %s\n", nome, ok ? "ok" : "FALHOU");
}

static void checkEq(long long got, long long want, const char *nome) {
  checagens++;
  const bool ok = got == want;
  if (!ok)
    falhas++;
  std::printf("%-62s %s", nome, ok ? "ok" : "FALHOU");
  if (!ok)
    std::printf("  (obtido %lld, esperado %lld)", got, want);
  std::printf("\n");
}

static int iabs(int v) { return v < 0 ? -v : v; }

// ---------------------------------------------------------------------------
// 1. Geometria derivada da area segura
// ---------------------------------------------------------------------------
static void testGeometria() {
  std::printf("\n-- 1. alcance da deriva derivado de crt::SAFE_* --\n");
  std::printf("   raster %dx%d  area segura L/T/R/B = %d/%d/%d/%d\n", crt::W, crt::H, crt::SAFE_L,
              crt::SAFE_T, crt::SAFE_R, crt::SAFE_B);
  std::printf("   margem de overscan  X=%d  Y=%d   (divisor %d, teto %d)\n", burnin::MARGIN_X,
              burnin::MARGIN_Y, burnin::DRIFT_DIVISOR, burnin::DRIFT_CAP);
  std::printf("   DERIVA  X=+-%d  Y=+-%d  ->  %d x %d = %d posicoes\n", burnin::DRIFT_X,
              burnin::DRIFT_Y, burnin::DRIFT_COLS, burnin::DRIFT_ROWS,
              burnin::DRIFT_COLS * burnin::DRIFT_ROWS);
  std::printf("   caixa interna L/T/R/B = %d/%d/%d/%d  (%dx%d)\n", burnin::INNER_L, burnin::INNER_T,
              burnin::INNER_R, burnin::INNER_B, burnin::INNER_W, burnin::INNER_H);

  checkEq(burnin::DRIFT_X, 3, "DRIFT_X = min(24/8, 3)");
  checkEq(burnin::DRIFT_Y, 2, "DRIFT_Y = min(18/8, 3)");
  check(burnin::DRIFT_X <= burnin::MARGIN_X && burnin::DRIFT_Y <= burnin::MARGIN_Y,
        "deriva cabe na margem de overscan");
  checkEq(burnin::INNER_L, crt::SAFE_L + burnin::DRIFT_X, "INNER_L = SAFE_L + DRIFT_X");
  checkEq(burnin::INNER_B, crt::SAFE_B - burnin::DRIFT_Y, "INNER_B = SAFE_B - DRIFT_Y");
  checkEq(burnin::DRIFT_PHASE, 17, "fase inicial cai no centro da malha");
  checkEq(burnin::driftX(0), 0, "passo 0 nao desloca em X");
  checkEq(burnin::driftY(0), 0, "passo 0 nao desloca em Y");

  // A promessa forte: caixa interna + qualquer deslocamento continua dentro de
  // crt::SAFE_*. O static_assert do header ja garante, mas repetimos por
  // enumeracao para o caso de alguem mexer nas constantes.
  bool dentro = true;
  for (int dx = -burnin::DRIFT_X; dx <= burnin::DRIFT_X; ++dx)
    for (int dy = -burnin::DRIFT_Y; dy <= burnin::DRIFT_Y; ++dy)
      if (burnin::INNER_L + dx < crt::SAFE_L || burnin::INNER_T + dy < crt::SAFE_T ||
          burnin::INNER_R + dx > crt::SAFE_R || burnin::INNER_B + dy > crt::SAFE_B)
        dentro = false;
  check(dentro, "INNER_* + qualquer deslocamento fica dentro da area segura");

  // E o caso degradado, para telas que ainda desenham ate SAFE_*: elas saem da
  // caixa segura mas nunca do raster.
  bool noRaster = true;
  for (int dx = -burnin::DRIFT_X; dx <= burnin::DRIFT_X; ++dx)
    for (int dy = -burnin::DRIFT_Y; dy <= burnin::DRIFT_Y; ++dy)
      if (crt::SAFE_L + dx < 0 || crt::SAFE_T + dy < 0 || crt::SAFE_R + dx > crt::W ||
          crt::SAFE_B + dy > crt::H)
        noRaster = false;
  check(noRaster, "SAFE_* + qualquer deslocamento fica dentro do raster");
}

// ---------------------------------------------------------------------------
// 2. O passeio da malha
// ---------------------------------------------------------------------------
static void testPasseio() {
  std::printf("\n-- 2. passeio da deriva (serpentina de %d passos) --\n", burnin::DRIFT_CYCLE);

  int visitas[burnin::DRIFT_COLS][burnin::DRIFT_ROWS];
  std::memset(visitas, 0, sizeof(visitas));

  int maiorSalto = 0, eixosPorPasso = 0, foraFaixa = 0;
  int px = burnin::driftX(0), py = burnin::driftY(0);
  for (uint32_t k = 0; k < (uint32_t)burnin::DRIFT_CYCLE; ++k) {
    const int x = burnin::driftX(k), y = burnin::driftY(k);
    if (iabs(x) > burnin::DRIFT_X || iabs(y) > burnin::DRIFT_Y)
      foraFaixa++;
    else
      visitas[x + burnin::DRIFT_X][y + burnin::DRIFT_Y]++;
    if (k) {
      const int d = iabs(x - px) + iabs(y - py);
      if (d > maiorSalto)
        maiorSalto = d;
      if ((x != px) && (y != py))
        eixosPorPasso++;
    }
    px = x;
    py = y;
  }
  // E a virada do ciclo: passo CYCLE tem de voltar ao passo 0 andando 1 px.
  const int volta = iabs(burnin::driftX(burnin::DRIFT_CYCLE) - px) +
                    iabs(burnin::driftY(burnin::DRIFT_CYCLE) - py);

  int naoVisitadas = 0, minVis = 9999, maxVis = 0;
  for (int i = 0; i < burnin::DRIFT_COLS; ++i)
    for (int j = 0; j < burnin::DRIFT_ROWS; ++j) {
      if (!visitas[i][j])
        naoVisitadas++;
      if (visitas[i][j] < minVis)
        minVis = visitas[i][j];
      if (visitas[i][j] > maxVis)
        maxVis = visitas[i][j];
    }

  // Mapa de permanencia, para inspecao a olho.
  std::printf("   visitas por posicao (linhas = dy %+d..%+d):\n", -burnin::DRIFT_Y, burnin::DRIFT_Y);
  for (int j = 0; j < burnin::DRIFT_ROWS; ++j) {
    std::printf("     dy=%+d ", j - burnin::DRIFT_Y);
    for (int i = 0; i < burnin::DRIFT_COLS; ++i)
      std::printf(" %d", visitas[i][j]);
    std::printf("\n");
  }

  checkEq(foraFaixa, 0, "nenhum passo fora de [-DRIFT_X..X] x [-DRIFT_Y..Y]");
  checkEq(naoVisitadas, 0, "as 35 posicoes da malha sao visitadas");
  checkEq(maiorSalto, 1, "cada passo anda exatamente 1 px");
  checkEq(eixosPorPasso, 0, "nenhum passo mexe nos dois eixos ao mesmo tempo");
  checkEq(volta, 1, "a virada do ciclo tambem anda so 1 px");
  std::printf("   permanencia min/max por posicao: %d/%d por ciclo de %d passos\n", minVis, maxVis,
              burnin::DRIFT_CYCLE);
}

// ---------------------------------------------------------------------------
// 3. Escalada dos estagios
// ---------------------------------------------------------------------------
// Avanca o relogio de `ms` em fatias de 50 ms, como faria o loop().
static burnin::Stage roda(burnin::Manager &m, uint32_t &t, uint32_t ms) {
  const uint32_t fim = t + ms;
  burnin::Stage s = m.tick(t);
  while ((int32_t)(fim - t) > 0) {
    t += 50;
    s = m.tick(t);
  }
  return s;
}

static void testEscalada() {
  std::printf("\n-- 3. escalada dos estagios --\n");
  std::printf("   limiares: deriva %u s, escuro %u s, apagado %u s, passo %u s\n",
              (unsigned)(burnin::DRIFT_AFTER_MS / 1000), (unsigned)(burnin::DIM_AFTER_MS / 1000),
              (unsigned)(burnin::BLANK_AFTER_MS / 1000), (unsigned)(burnin::DRIFT_STEP_MS / 1000));

  uint32_t t = 1000;
  burnin::Manager m;
  m.reset(t);
  check(m.tick(t) == burnin::STAGE_ACTIVE, "no reset: ATIVO");

  check(roda(m, t, burnin::DRIFT_AFTER_MS - 2000) == burnin::STAGE_ACTIVE,
        "2 s antes do limiar: ainda ATIVO");
  check(roda(m, t, 4000) == burnin::STAGE_DRIFT, "logo depois do limiar: DERIVA");
  check(roda(m, t, burnin::DIM_AFTER_MS - burnin::DRIFT_AFTER_MS - 8000) == burnin::STAGE_DRIFT,
        "antes dos 10 min: continua DERIVA");
  check(roda(m, t, 12000) == burnin::STAGE_DIM, "passados 10 min: ESCURO");
  check(roda(m, t, burnin::BLANK_AFTER_MS - burnin::DIM_AFTER_MS - 8000) == burnin::STAGE_DIM,
        "antes dos 30 min: continua ESCURO");
  check(roda(m, t, 12000) == burnin::STAGE_BLANK, "passados 30 min: APAGADO");
  check(roda(m, t, 6UL * 3600UL * 1000UL) == burnin::STAGE_BLANK, "6 h depois: segue APAGADO");
  // A soma dos trechos acima da 30 min + 10 s, mais 6 h. O roda() passa do
  // alvo por ate 49 ms por chamada, entao a comparacao e por faixa.
  {
    const uint32_t esperado = burnin::BLANK_AFTER_MS + 10000UL + 6UL * 3600UL * 1000UL;
    const uint32_t obtido = m.idleMs(t);
    std::printf("   idleMs = %u ms (esperado ~%u ms)\n", (unsigned)obtido, (unsigned)esperado);
    check(obtido >= esperado && obtido - esperado < 500,
          "idleMs acompanha o tempo parado (subtracao sem sinal)");
  }

  // Um salto unico maior que todos os limiares (loop() preso, relogio pulando)
  // tem de cair direto no estagio final, sem passar pelos intermediarios.
  burnin::Manager salto;
  uint32_t ts = 500;
  salto.reset(ts);
  ts += burnin::BLANK_AFTER_MS + 1;
  check(salto.tick(ts) == burnin::STAGE_BLANK, "salto de 30 min num tick so vai direto a APAGADO");
}

// ---------------------------------------------------------------------------
// 4. Retorno ao ativo
// ---------------------------------------------------------------------------
static void testAtividade() {
  std::printf("\n-- 4. acao do usuario volta ao ATIVO --\n");
  const uint32_t marcos[3] = {burnin::DRIFT_AFTER_MS + 5000, burnin::DIM_AFTER_MS + 5000,
                              burnin::BLANK_AFTER_MS + 5000};
  const char *nomes[3] = {"de DERIVA", "de ESCURO", "de APAGADO"};
  const bool pedeRepintura[3] = {false, true, true}; // ATIVO e DERIVA desenham igual
  for (int i = 0; i < 3; ++i) {
    uint32_t t = 7777;
    burnin::Manager m;
    m.reset(t);
    roda(m, t, marcos[i]);
    const int ox = m.ox(), oy = m.oy();
    m.takeRepaint(); // consome o que a deriva ja pediu: aqui so interessa a acao
    const bool mudou = m.notifyActivity(t);
    char nome[96];
    std::snprintf(nome, sizeof(nome), "%s volta a ATIVO", nomes[i]);
    check(m.tick(t) == burnin::STAGE_ACTIVE, nome);
    // O deslocamento e PRESERVADO: zera-lo daria salto de ate 3 px e repinte a
    // cada botao, e enviesaria o passeio para uma celula so da malha.
    std::snprintf(nome, sizeof(nome), "%s preserva o deslocamento (%+d,%+d)", nomes[i], ox, oy);
    check(m.ox() == ox && m.oy() == oy, nome);
    std::snprintf(nome, sizeof(nome), "%s pede repintura: %s", nomes[i],
                  pedeRepintura[i] ? "sim" : "nao");
    check(mudou == pedeRepintura[i] && m.takeRepaint() == pedeRepintura[i], nome);
  }
  // Atividade com o aparelho ja ativo nao pode pedir repintura: senao o menu
  // piscaria a cada toque.
  uint32_t t = 100;
  burnin::Manager m;
  m.reset(t);
  m.tick(t);
  check(!m.notifyActivity(t + 10) && !m.takeRepaint(), "atividade em ATIVO nao pede repintura");
}

// ---------------------------------------------------------------------------
// 5 + 6. Corrida longa: limites do deslocamento e bandeira de repintura
// ---------------------------------------------------------------------------
// `usoACada` = 0 roda o aparelho abandonado (vai ate APAGADO e fica la);
// diferente de 0 simula alguem mexendo de tempos em tempos, que e o caso em que
// a deriva realmente trabalha por horas.
static void testCorridaLonga(uint32_t t0, const char *rotulo, unsigned horas, uint32_t usoACada) {
  std::printf("\n-- corrida de %u h %s --\n", horas, rotulo);
  uint32_t t = t0;
  burnin::Manager m;
  m.reset(t);

  const uint32_t passo = 137; // nao divide os limiares: pega borda de comparacao
  const uint32_t total = horas * 3600UL * 1000UL;

  int visitas[burnin::DRIFT_COLS][burnin::DRIFT_ROWS];
  std::memset(visitas, 0, sizeof(visitas));

  int px = 0, py = 0;
  uint32_t proximoUso = usoACada ? t + usoACada : 0;
  burnin::Stage pst = burnin::STAGE_ACTIVE;
  long long ticks = 0, mudancasOffset = 0, repinturas = 0, repinturaSemMotivo = 0,
            motivoSemRepintura = 0, fora = 0, saltoGrande = 0;
  int maxAbsX = 0, maxAbsY = 0;

  for (uint32_t d = 0; d < total; d += passo) {
    t += passo;
    if (usoACada && timeReached(t, proximoUso)) {
      proximoUso = t + usoACada;
      m.notifyActivity(t);
    }
    const burnin::Stage st = m.tick(t);
    ++ticks;
    const int x = m.ox(), y = m.oy();
    if (iabs(x) <= burnin::DRIFT_X && iabs(y) <= burnin::DRIFT_Y)
      visitas[x + burnin::DRIFT_X][y + burnin::DRIFT_Y]++;

    if (iabs(x) > burnin::DRIFT_X || iabs(y) > burnin::DRIFT_Y)
      ++fora;
    if (iabs(x) > maxAbsX)
      maxAbsX = iabs(x);
    if (iabs(y) > maxAbsY)
      maxAbsY = iabs(y);
    // O conteudo desenhado na caixa interna nunca pode sair da area segura.
    if (burnin::INNER_L + x < crt::SAFE_L || burnin::INNER_R + x > crt::SAFE_R ||
        burnin::INNER_T + y < crt::SAFE_T || burnin::INNER_B + y > crt::SAFE_B)
      ++fora;

    const bool offMudou = (x != px || y != py);
    const bool stMudou = burnin::visualDiffers(st, pst);
    if (offMudou) {
      ++mudancasOffset;
      if (iabs(x - px) + iabs(y - py) != 1)
        ++saltoGrande;
    }

    const bool rep = m.takeRepaint();
    if (rep)
      ++repinturas;
    // A bandeira tem de disparar exatamente quando ha motivo: deslocamento
    // novo ou estagio novo. Nada de repintar a tela a esmo (a TV pisca) nem de
    // deixar de repintar (o desenho antigo fica no lugar errado).
    if (rep && !offMudou && !stMudou)
      ++repinturaSemMotivo;
    if (!rep && (offMudou || stMudou))
      ++motivoSemRepintura;

    px = x;
    py = y;
    pst = st;
  }

  std::printf("   %lld ticks, %lld mudancas de deslocamento, %lld repinturas\n", ticks,
              mudancasOffset, repinturas);
  std::printf("   maior |ox| = %d (limite %d), maior |oy| = %d (limite %d)\n", maxAbsX,
              burnin::DRIFT_X, maxAbsY, burnin::DRIFT_Y);
  checkEq(fora, 0, "deslocamento sempre dentro dos limites derivados");
  checkEq(saltoGrande, 0, "nenhuma mudanca de deslocamento maior que 1 px");
  checkEq(repinturaSemMotivo, 0, "nenhuma repintura sem deslocamento/estagio novo");
  checkEq(motivoSemRepintura, 0, "nenhuma mudanca sem pedir repintura");
  check(mudancasOffset > 0, "houve deriva de verdade na corrida");

  if (usoACada) {
    // Com uso intermitente a deriva trabalha o tempo todo; a malha inteira tem
    // de ser percorrida, senao a mitigacao fica presa num canto so.
    int naoVisitadas = 0;
    for (int i = 0; i < burnin::DRIFT_COLS; ++i)
      for (int j = 0; j < burnin::DRIFT_ROWS; ++j)
        if (!visitas[i][j])
          naoVisitadas++;
    checkEq(naoVisitadas, 0, "as 35 posicoes sao ocupadas mesmo com uso intermitente");
  }
}

// ---------------------------------------------------------------------------
// 7. A volta do millis()
// ---------------------------------------------------------------------------
static void testWrap() {
  std::printf("\n-- 7. volta do millis() em 2^32 --\n");
  // Comeca 20 s antes da virada e fica parado atravessando os tres limiares:
  // com comparacao crua (now > prazo) a soma last_+BLANK_AFTER_MS estoura, o
  // prazo vira um numero pequeno e o estagio dispararia na hora errada.
  uint32_t t = 0xFFFFFFFFUL - 20000UL;
  burnin::Manager m;
  m.reset(t);
  check(m.tick(t) == burnin::STAGE_ACTIVE, "antes da virada: ATIVO");
  check(roda(m, t, 10000) == burnin::STAGE_ACTIVE, "10 s (ainda antes de 2^32): ATIVO");
  check(roda(m, t, 20000) == burnin::STAGE_ACTIVE,
        "30 s no total, ja depois da virada: continua ATIVO");
  std::printf("   relogio agora = %u (virou)\n", (unsigned)t);
  check(t < 100000UL, "o relogio de fato deu a volta");
  check(roda(m, t, burnin::DRIFT_AFTER_MS) == burnin::STAGE_DRIFT, "DERIVA no limiar, pos-virada");
  check(roda(m, t, burnin::DIM_AFTER_MS) == burnin::STAGE_DIM, "ESCURO no limiar, pos-virada");
  check(roda(m, t, burnin::BLANK_AFTER_MS) == burnin::STAGE_BLANK, "APAGADO no limiar, pos-virada");

  // E a atividade logo apos a virada, com last_ do lado de la.
  burnin::Manager m2;
  uint32_t t2 = 0xFFFFFFFFUL - 5000UL;
  m2.reset(t2);
  roda(m2, t2, 10000); // atravessa a virada
  m2.notifyActivity(t2);
  check(roda(m2, t2, burnin::DRIFT_AFTER_MS - 5000) == burnin::STAGE_ACTIVE,
        "atividade depois da virada reancorou o prazo");
  check(roda(m2, t2, 10000) == burnin::STAGE_DRIFT, "e o limiar segue valendo a partir dali");
}

// ---------------------------------------------------------------------------
// 8. Escurecimento e protetor de tela
// ---------------------------------------------------------------------------
static void testDimESaver() {
  std::printf("\n-- 8. escurecimento por redesenho e protetor de tela --\n");
  const uint16_t navy = 0x000F, branco = 0xFFFF, acento = 0x96BC;
  std::printf("   NAVY  0x%04X -> 0x%04X\n", navy, burnin::dim565(navy));
  std::printf("   BRANCO 0x%04X -> 0x%04X\n", branco, burnin::dim565(branco));
  std::printf("   ACENTO 0x%04X -> 0x%04X   (%u/%u)\n", acento, burnin::dim565(acento),
              burnin::DIM_NUM, burnin::DIM_DEN);

  checkEq(burnin::dim565(0x0000), 0x0000, "preto escurecido continua preto");
  check(burnin::dim565(branco) < branco, "branco escurece");
  // Componente a componente: 31*3/8 = 11, 63*3/8 = 23.
  checkEq((burnin::dim565(branco) >> 11) & 0x1F, 11, "R do branco vai a 11/31");
  checkEq((burnin::dim565(branco) >> 5) & 0x3F, 23, "G do branco vai a 23/63");
  checkEq(burnin::dim565(branco) & 0x1F, 11, "B do branco vai a 11/31");
  check(burnin::dim565(acento) != 0x07FF, "o acento escurecido nao vira ciano saturado");
  // Nenhuma cor pode virar ciano saturado (dot crawl do NTSC).
  bool ciano = false;
  for (uint32_t c = 0; c <= 0xFFFF; ++c)
    if (burnin::dim565((uint16_t)c) == 0x07FF)
      ciano = true;
  check(!ciano, "nenhuma das 65536 cores escurecidas cai em 0x07FF");
  check(burnin::SAVER_COLOR != 0x07FF, "SAVER_COLOR nao e ciano saturado");

  checkEq(burnin::shade(burnin::STAGE_ACTIVE, acento), acento, "shade em ATIVO nao mexe na cor");
  checkEq(burnin::shade(burnin::STAGE_DRIFT, acento), acento, "shade em DERIVA nao mexe na cor");
  checkEq(burnin::shade(burnin::STAGE_DIM, acento), burnin::dim565(acento), "shade em ESCURO");
  checkEq(burnin::shade(burnin::STAGE_BLANK, acento), 0, "shade em APAGADO e preto");

  // Protetor: 4 h de quique, sempre dentro do raster, e sempre andando.
  uint32_t t = 12345;
  burnin::Manager m;
  m.reset(t);
  roda(m, t, burnin::BLANK_AFTER_MS + 1000);
  check(m.tick(t) == burnin::STAGE_BLANK, "protetor no ar");
  long long passos = 0, foraRaster = 0, parado = 0;
  int minX = 9999, maxX = -9999, minY = 9999, maxY = -9999;
  const uint32_t horas4 = 4UL * 3600UL * 1000UL;
  for (uint32_t d = 0; d < horas4; d += 50) {
    t += 50;
    m.tick(t);
    if (!m.takeSaverStep())
      continue;
    ++passos;
    const int x = m.saverX(), y = m.saverY();
    if (x < 0 || y < 0 || x + burnin::SAVER_W > crt::W || y + burnin::SAVER_H > crt::H)
      ++foraRaster;
    if (x == m.saverPrevX() && y == m.saverPrevY())
      ++parado;
    if (x < minX) minX = x;
    if (x > maxX) maxX = x;
    if (y < minY) minY = y;
    if (y > maxY) maxY = y;
  }
  std::printf("   %lld passos em 4 h; x em [%d..%d], y em [%d..%d]\n", passos, minX, maxX, minY,
              maxY);
  checkEq(foraRaster, 0, "o protetor nunca sai do raster 320x240");
  checkEq(parado, 0, "todo passo do protetor move de fato");
  check(minX == 0 && maxX == crt::W - burnin::SAVER_W, "o protetor varre a largura toda");
  check(minY == 0 && maxY == crt::H - burnin::SAVER_H, "o protetor varre a altura toda");
  // Custo: dois retangulos por passo, a 5 Hz.
  std::printf("   custo do protetor: %d px apagados + %d px desenhados a cada %u ms\n",
              burnin::SAVER_W * burnin::SAVER_H, burnin::SAVER_W * burnin::SAVER_H,
              (unsigned)burnin::SAVER_STEP_MS);

  checkEq(m.outputLevel(), burnin::OUT_LEVEL_NORMAL,
          "outputLevel fica no normal (USE_OUTPUT_LEVEL_DIM desligado)");
}

// ---------------------------------------------------------------------------
static void testCusto() {
  std::printf("\n-- custo de SRAM --\n");
  std::printf("   sizeof(burnin::Manager) = %zu bytes\n", sizeof(burnin::Manager));
  check(sizeof(burnin::Manager) <= 48, "o gerente cabe em 48 bytes (sem sprite, sem alocacao)");
  bool ascii = true;
  const burnin::Stage todos[4] = {burnin::STAGE_ACTIVE, burnin::STAGE_DRIFT, burnin::STAGE_DIM,
                                  burnin::STAGE_BLANK};
  for (int i = 0; i < 4; ++i)
    for (const char *p = burnin::stageLabel(todos[i]); *p; ++p)
      if ((unsigned char)*p > 0x7f)
        ascii = false;
  check(ascii, "rotulos dos estagios sao ASCII puro");
}

int main(void) {
  std::printf("== bancada do BurnIn.h (logica pura, sem M5GFX) ==\n");
  testGeometria();
  testPasseio();
  testEscalada();
  testAtividade();
  testCorridaLonga(50000UL, "abandonado, a partir de t=50 s", 168, 0);
  testCorridaLonga(0xFFFFFFFFUL - 60000UL, "abandonado, atravessando a volta do millis()", 168, 0);
  testCorridaLonga(50000UL, "com uso a cada 20 min", 168, 20UL * 60UL * 1000UL);
  testCorridaLonga(0xFFFFFFFFUL - 60000UL, "com uso a cada 20 min, atravessando a volta", 168,
                   20UL * 60UL * 1000UL);
  testWrap();
  testDimESaver();
  testCusto();
  std::printf("\n%d checagens, %d falha(s)\n", checagens, falhas);
  return falhas ? 1 : 0;
}
