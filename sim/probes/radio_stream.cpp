// ============================================================================
//  Probe da logica de rede do radio (include/RadioStream.h).
//
//  Bancada NATIVA: nao usa SDL, nao usa LovyanGFX, nao usa Arduino. So a parte
//  pura do header — parse de URL (http e https), parse de cabecalho,
//  redirecionamento, de-interleave do icy-metaint, parse do StreamTitle, anel e
//  backoff. E justamente essa parte que nao da
//  para conferir no aparelho sem uma TV, um roteador e paciencia.
//
//  O teste que importa e o do icy-metaint: a stream sintetica tem audio com
//  conteudo CONHECIDO (uma rampa) e blocos de metadado em posicoes conhecidas.
//  Se o de-interleave errar um byte que seja, a rampa quebra e o probe acusa.
//  Cada stream e alimentada em VARIOS tamanhos de leitura, inclusive tamanhos
//  primos, para garantir que blocos partidos entre duas leituras sejam
//  retomados no ponto exato.
//
//    make -C sim probes && ./sim/build/probe_radio_stream
//
//  Ou, sem o simulador (e assim que o C++11 do firmware e conferido):
//
//    g++ -std=gnu++11 -I include -o /tmp/probe_radio_stream sim/probes/radio_stream.cpp
//    /tmp/probe_radio_stream
//
//  Sai com 1 se algum caso falhar, para servir de porta em CI.
// ============================================================================

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "RadioStream.h"

static int g_fail = 0;
static int g_pass = 0;

static void check(bool cond, const char *what) {
  if (cond) {
    ++g_pass;
  } else {
    ++g_fail;
    printf("  FALHOU: %s\n", what);
  }
}

static void checkStr(const char *got, const char *want, const char *what) {
  const bool ok = strcmp(got, want) == 0;
  if (!ok)
    printf("  FALHOU: %s\n    esperado [%s]\n    obtido   [%s]\n", what, want, got);
  else
    ++g_pass;
  if (!ok)
    ++g_fail;
}

// ---------------------------------------------------------------------------
//  1. Backoff
// ---------------------------------------------------------------------------

static void testBackoff() {
  printf("\n[1] backoff\n");
  const uint32_t want[] = {1000, 2000, 4000, 8000, 16000, 30000, 30000, 30000};
  for (uint32_t i = 0; i < 8; ++i) {
    char msg[64];
    snprintf(msg, sizeof(msg), "backoffDelayMs(%u) == %u", i, want[i]);
    check(radio::backoffDelayMs(i) == want[i], msg);
  }
  // Nunca zero: um backoff de 0 ms e exatamente o laco apertado que a
  // armadilha 4 do AGENTS.md descreve.
  for (uint32_t i = 0; i < 40; ++i)
    check(radio::backoffDelayMs(i) >= radio::BACKOFF_MIN_MS, "backoff nunca abaixo do minimo");
  // Monotonico ate o teto.
  for (uint32_t i = 1; i < 40; ++i)
    check(radio::backoffDelayMs(i) >= radio::backoffDelayMs(i - 1), "backoff nao regride");
  printf("  tabela: ");
  for (uint32_t i = 0; i < 8; ++i)
    printf("%u ", radio::backoffDelayMs(i));
  printf("(ms)\n");
}

// ---------------------------------------------------------------------------
//  2. Parse da URL
// ---------------------------------------------------------------------------

static void testUrl() {
  printf("\n[2] parse de URL\n");
  radio::Url u;

  check(radio::parseUrl(radio::STATIONS[0].url, u), "URL da estacao e valida");
  checkStr(u.host, "51-222-26-208.webnow.com.br", "host da estacao");
  checkStr(u.path, "/diario.mp3", "caminho da estacao");
  check(u.port == 80, "porta padrao 80");
  check(!u.tls, "estacao da tabela e http puro");

  check(radio::parseUrl("http://exemplo.com:8000/live", u), "com porta");
  checkStr(u.host, "exemplo.com", "host com porta");
  check(u.port == 8000, "porta 8000");
  checkStr(u.path, "/live", "caminho com porta");

  check(radio::parseUrl("http://exemplo.com", u), "sem caminho");
  checkStr(u.path, "/", "caminho vazio vira /");

  check(radio::parseUrl("exemplo.com/x", u), "sem esquema e aceito");
  checkStr(u.host, "exemplo.com", "host sem esquema");

  check(radio::parseUrl("http://a.b/c?d=1", u), "com query");
  checkStr(u.path, "/c?d=1", "query vai junto no caminho");

  // No Fruit Jam o TLS roda no ESP32-C6: https passa a valer, com porta 443.
  check(radio::parseUrl("https://exemplo.com/x", u), "https e aceito (TLS no C6)");
  check(u.tls, "https liga o TLS");
  check(u.port == 443, "https usa a porta 443");
  checkStr(u.path, "/x", "caminho do https");
  check(radio::parseUrl("https://exemplo.com:8443/y", u) && u.tls && u.port == 8443,
        "https com porta propria");
  check(radio::parseUrl("http://exemplo.com/x", u) && !u.tls, "http desliga o TLS");
  check(!radio::parseUrl("ftp://exemplo.com/x", u), "esquema desconhecido e recusado");
  check(!radio::parseUrl("", u), "string vazia e recusada");
  check(!radio::parseUrl(0, u), "nulo e recusado");
  check(!radio::parseUrl("http://exemplo.com:0/x", u), "porta 0 e recusada");
  check(!radio::parseUrl("http://exemplo.com:99999/x", u), "porta fora da faixa e recusada");

  // Host mais longo que o buffer nao pode estourar nada.
  std::string big = "http://";
  big.append(400, 'z');
  big += "/x";
  radio::parseUrl(big.c_str(), u);
  check(strlen(u.host) < sizeof(u.host), "host longo e truncado sem estouro");
  printf("  estacao: %s:%u%s\n", "51-222-26-208.webnow.com.br", 80u, "/diario.mp3");
}

// ---------------------------------------------------------------------------
//  2b. Redirecionamento (3xx + Location)
// ---------------------------------------------------------------------------

static void testRedirect() {
  printf("\n[2b] redirecionamento\n");
  // Cabecalho 302 tipico de balanceador de Icecast, em varios cortes de leitura.
  const char *h302 = "HTTP/1.1 302 Found\r\n"
                     "Location: https://edge2.exemplo.com.br:8443/diario.mp3?tok=abc\r\n"
                     "Content-Length: 0\r\n\r\n";
  const size_t cortes[] = {1, 5, 64, 4096};
  for (size_t ci = 0; ci < 4; ++ci) {
    radio::HeaderParser hp;
    hp.reset();
    size_t bodyStart = 0;
    for (size_t pos = 0; pos < strlen(h302);) {
      size_t take = cortes[ci];
      if (pos + take > strlen(h302))
        take = strlen(h302) - pos;
      const size_t used = hp.feed((const uint8_t *)h302 + pos, take);
      pos += used;
      bodyStart = pos;
      if (used < take || hp.done())
        break;
    }
    check(hp.done(), "302 termina o cabecalho");
    check(!hp.ok(), "302 nao e audio");
    check(hp.redirect(), "302 com Location pede redirecionamento");
    check(bodyStart == strlen(h302), "302 consumido inteiro");
    checkStr(hp.info().location, "https://edge2.exemplo.com.br:8443/diario.mp3?tok=abc",
             "Location capturado cru (sem normalizar)");
  }

  radio::Url cur, next;
  radio::parseUrl(radio::STATIONS[0].url, cur);

  // Absoluto, com troca para https: o TLS e do C6.
  check(radio::resolveRedirect(cur, "https://edge2.exemplo.com.br:8443/diario.mp3?tok=abc", next),
        "absoluto https resolvido");
  checkStr(next.host, "edge2.exemplo.com.br", "host do destino");
  check(next.port == 8443 && next.tls, "porta e TLS do destino");
  checkStr(next.path, "/diario.mp3?tok=abc", "caminho com token");

  // Caminho absoluto: mantem host, porta e esquema.
  check(radio::resolveRedirect(cur, "/outro.mp3", next), "caminho absoluto resolvido");
  checkStr(next.host, cur.host, "caminho absoluto mantem o host");
  check(next.port == cur.port && next.tls == cur.tls, "caminho absoluto mantem porta e esquema");
  checkStr(next.path, "/outro.mp3", "caminho novo");

  // Recusas: seguir errado e pior que cair no backoff.
  check(!radio::resolveRedirect(cur, "", next), "Location vazio recusado");
  check(!radio::resolveRedirect(cur, "outro.mp3", next), "relativo sem barra recusado");
  check(!radio::resolveRedirect(cur, "//cdn.exemplo.com/x", next), "sem esquema recusado");
  check(!radio::resolveRedirect(cur, "ftp://x/y", next), "esquema estranho recusado");
  std::string longo = "/";
  longo.append(300, 'a');
  check(!radio::resolveRedirect(cur, longo.c_str(), next), "caminho que nao cabe recusado");

  // Location que cabe segue; o que nao cabe (na linha OU no campo) fica vazio,
  // e sem Location nao ha redirecionamento: o chamador cai no backoff em vez de
  // seguir uma URL truncada, que seria conectar no lugar errado.
  {
    std::string h = "HTTP/1.1 301 Moved\r\nLocation: http://x.com/";
    h.append(150, 'b');
    h += "\r\n\r\n";
    radio::HeaderParser hp;
    hp.reset();
    hp.feed((const uint8_t *)h.data(), h.size());
    check(hp.done() && hp.redirect(), "301 com Location de 164 bytes segue");
  }
  {
    std::string h = "HTTP/1.1 301 Moved\r\nLocation: http://x.com/";
    h.append(400, 'b'); // estoura a linha do parser
    h += "\r\nContent-Length: 0\r\n\r\n";
    radio::HeaderParser hp;
    hp.reset();
    const size_t used = hp.feed((const uint8_t *)h.data(), h.size());
    check(hp.done() && !hp.redirect(), "Location truncado nao e seguido");
    check(used == h.size(), "linha longa consumida inteira mesmo assim");
  }
  {
    radio::HeaderParser hp;
    hp.reset();
    const char *h = "HTTP/1.1 302 Found\r\nContent-Type: text/html\r\n\r\n";
    hp.feed((const uint8_t *)h, strlen(h));
    check(hp.done() && !hp.redirect() && !hp.ok(), "302 sem Location nao redireciona");
  }
  {
    radio::HeaderParser hp;
    hp.reset();
    const char *h = "HTTP/1.1 304 Not Modified\r\nLocation: /x\r\n\r\n";
    hp.feed((const uint8_t *)h, strlen(h));
    check(hp.done() && !hp.redirect(), "304 nao e redirecionamento");
  }
  check(radio::isRedirectStatus(307) && radio::isRedirectStatus(308), "307 e 308 seguem");
  check(!radio::isRedirectStatus(200) && !radio::isRedirectStatus(300), "200 e 300 nao seguem");
}

// ---------------------------------------------------------------------------
//  3. Parse do cabecalho (resposta real da DIARIO FM)
// ---------------------------------------------------------------------------

// Cabecalho conforme verificado no ar: HTTP/1.1 200, Server: Icecast,
// Content-Type: audio/mpeg, icy-br 96, icy-name DIARIO FM, icy-genre ADULTO.
static const char kRealHeader[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: audio/mpeg\r\n"
    "Server: Icecast 2.4.0-kh10\r\n"
    "Cache-Control: no-cache, no-store\r\n"
    "Pragma: no-cache\r\n"
    "Access-Control-Allow-Origin: *\r\n"
    "icy-br: 96\r\n"
    "icy-description: A radio da cidade\r\n"
    "icy-genre: ADULTO\r\n"
    "icy-name: DIARIO FM\r\n"
    "icy-pub: 1\r\n"
    "icy-url: http://diariofm.com.br\r\n"
    "icy-metaint: 16000\r\n"
    "\r\n";

static void feedHeader(radio::HeaderParser &hp, const char *text, size_t chunk, size_t &bodyStart) {
  const size_t n = strlen(text);
  size_t pos = 0;
  while (pos < n && !hp.done()) {
    const size_t take = (n - pos) < chunk ? (n - pos) : chunk;
    const size_t used = hp.feed((const uint8_t *)text + pos, take);
    pos += used;
    if (used < take)
      break; // acabou o cabecalho no meio deste pedaco
  }
  bodyStart = pos;
}

static void testHeader() {
  printf("\n[3] parse do cabecalho\n");
  // Varios tamanhos de leitura, inclusive 1 byte e um primo: o cabecalho tem de
  // sair igual nao importa onde o socket cortou.
  const size_t chunks[] = {1, 7, 13, 64, 256, 4096};
  for (size_t ci = 0; ci < sizeof(chunks) / sizeof(chunks[0]); ++ci) {
    radio::HeaderParser hp;
    hp.reset();
    size_t bodyStart = 0;
    feedHeader(hp, kRealHeader, chunks[ci], bodyStart);
    char msg[96];
    snprintf(msg, sizeof(msg), "cabecalho completo em pedacos de %zu", chunks[ci]);
    check(hp.done(), msg);
    snprintf(msg, sizeof(msg), "cabecalho aceito em pedacos de %zu", chunks[ci]);
    check(hp.ok(), msg);
    const radio::IcyInfo &i = hp.info();
    check(i.status == 200, "status 200");
    check(i.audio, "content-type audio/mpeg reconhecido");
    check(i.metaint == 16000, "icy-metaint 16000");
    checkStr(i.name, "DIARIO FM", "icy-name");
    checkStr(i.desc, "A radio da cidade", "icy-description");
    check(bodyStart == strlen(kRealHeader), "consumiu exatamente o cabecalho");
    if (ci == 0)
      printf("  status:%d metaint:%d name:[%s] desc:[%s]\n", (int)i.status, (int)i.metaint, i.name,
             i.desc);
  }

  // O primeiro byte de audio costuma chegar no MESMO read do fim do cabecalho.
  // Se o parser consumir demais, o fluxo comeca deslocado e o metaint erra.
  {
    std::string blob = kRealHeader;
    const char *body = "\x01\x02\x03\x04";
    blob.append(body, 4);
    radio::HeaderParser hp;
    hp.reset();
    const size_t used = hp.feed((const uint8_t *)blob.data(), blob.size());
    check(hp.done() && hp.ok(), "cabecalho + corpo no mesmo read");
    check(used == strlen(kRealHeader), "devolveu o limite exato do corpo");
    check(blob.size() - used == 4, "4 bytes de corpo restantes");
    check(memcmp(blob.data() + used, body, 4) == 0, "corpo intacto");
  }

  // Sem icy-metaint: metaint tem de sair -1 (fluxo direto).
  {
    radio::HeaderParser hp;
    hp.reset();
    const char *h = "HTTP/1.1 200 OK\r\nContent-Type: audio/mpeg\r\n\r\n";
    hp.feed((const uint8_t *)h, strlen(h));
    check(hp.done() && hp.ok(), "cabecalho minimo aceito");
    check(hp.info().metaint == -1, "sem icy-metaint -> -1");
  }

  // Shoutcast antigo responde "ICY 200 OK" em vez de HTTP/1.1.
  {
    radio::HeaderParser hp;
    hp.reset();
    const char *h = "ICY 200 OK\r\ncontent-type:audio/mpeg\r\nicy-metaint:8192\r\n\r\n";
    hp.feed((const uint8_t *)h, strlen(h));
    check(hp.ok(), "linha de status ICY 200 aceita");
    check(hp.info().metaint == 8192, "metaint em minusculas sem espaco");
  }

  // Recusas.
  {
    radio::HeaderParser hp;
    hp.reset();
    const char *h = "HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\n\r\n";
    hp.feed((const uint8_t *)h, strlen(h));
    check(hp.done() && !hp.ok(), "404 e recusado");
  }
  {
    radio::HeaderParser hp;
    hp.reset();
    const char *h = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n";
    hp.feed((const uint8_t *)h, strlen(h));
    check(hp.done() && !hp.ok(), "200 com HTML e recusado");
  }
  // metaint absurdo vira "sem metadado" em vez de picotar o fluxo.
  {
    radio::HeaderParser hp;
    hp.reset();
    const char *h = "HTTP/1.1 200 OK\r\nContent-Type: audio/mpeg\r\nicy-metaint: 0\r\n\r\n";
    hp.feed((const uint8_t *)h, strlen(h));
    check(hp.info().metaint == -1, "icy-metaint 0 tratado como ausente");
  }
  // Linha absurdamente longa nao pode desalinhar o resto do cabecalho.
  {
    radio::HeaderParser hp;
    hp.reset();
    std::string h = "HTTP/1.1 200 OK\r\nX-Longa: ";
    h.append(2000, 'q');
    h += "\r\nContent-Type: audio/mpeg\r\nicy-metaint: 16000\r\n\r\n";
    const size_t used = hp.feed((const uint8_t *)h.data(), h.size());
    check(hp.done() && hp.ok(), "linha gigante nao quebra o parser");
    check(hp.info().metaint == 16000, "metaint depois da linha gigante");
    check(used == h.size(), "consumiu o cabecalho inteiro");
  }
}

// ---------------------------------------------------------------------------
//  4. StreamTitle
// ---------------------------------------------------------------------------

static void testStreamTitle() {
  printf("\n[4] StreamTitle\n");
  char t[128];

  check(radio::parseStreamTitle("StreamTitle='FULANO - CANCAO';StreamUrl='';", t, sizeof(t)),
        "caso normal encontrado");
  checkStr(t, "FULANO - CANCAO", "titulo normal");

  // Aspa simples DENTRO do titulo: o valor so termina em "';".
  check(radio::parseStreamTitle("StreamTitle='ROCK N' ROLL';StreamUrl='';", t, sizeof(t)),
        "aspa no meio encontrado");
  checkStr(t, "ROCK N' ROLL", "aspa simples no meio do titulo e conteudo");

  check(radio::parseStreamTitle("StreamTitle='A ';B'; C';StreamUrl='';", t, sizeof(t)),
        "aspa+; interno: corta no primeiro");
  checkStr(t, "A ", "corta no primeiro '; (regra do Icecast)");

  // Sem o ';' final (servidores existem assim).
  check(radio::parseStreamTitle("StreamTitle='SEM PONTO E VIRGULA'", t, sizeof(t)),
        "sem terminador encontrado");
  checkStr(t, "SEM PONTO E VIRGULA", "sem terminador vale ate o fim");

  // Titulo vazio.
  check(radio::parseStreamTitle("StreamTitle='';StreamUrl='';", t, sizeof(t)), "titulo vazio achado");
  checkStr(t, "", "titulo vazio vira string vazia");

  // Campo ausente.
  check(!radio::parseStreamTitle("StreamUrl='http://x';", t, sizeof(t)), "sem StreamTitle -> false");
  checkStr(t, "", "sem StreamTitle limpa o destino");
  check(!radio::parseStreamTitle("", t, sizeof(t)), "bloco vazio -> false");
  check(!radio::parseStreamTitle(0, t, sizeof(t)), "nulo -> false");

  // Acento tem de virar ASCII: a fonte bitmap nao tem glifo e em UTF-8 cada
  // letra acentuada sao DOIS bytes sem glifo (AGENTS.md 2.7).
  check(radio::parseStreamTitle("StreamTitle='CORAÇÃO SERTANEJO';", t, sizeof(t)),
        "titulo com acento achado");
  checkStr(t, "CORACAO SERTANEJO", "acento normalizado para ASCII");

  // Titulo mais longo que o destino nao pode estourar.
  {
    std::string big = "StreamTitle='";
    big.append(500, 'X');
    big += "';";
    char small[32];
    radio::parseStreamTitle(big.c_str(), small, sizeof(small));
    check(strlen(small) < sizeof(small), "titulo longo truncado sem estouro");
  }
}

// ---------------------------------------------------------------------------
//  5. De-interleave do icy-metaint — o teste central
// ---------------------------------------------------------------------------

// Monta um bloco ICY: 1 byte de comprimento (em unidades de 16) + padding.
static void appendMeta(std::vector<uint8_t> &out, const std::string &text) {
  if (text.empty()) {
    out.push_back(0); // bloco vazio: "nada mudou"
    return;
  }
  const size_t units = (text.size() + 15) / 16;
  out.push_back((uint8_t)units);
  for (size_t i = 0; i < units * 16; ++i)
    out.push_back(i < text.size() ? (uint8_t)text[i] : 0);
}

// Roda a stream por um MetaSplitter em pedacos de `chunk` bytes, in-place
// (out == in), que e exatamente como a tarefa de rede a usa.
static std::vector<uint8_t> run(const std::vector<uint8_t> &stream, int32_t metaint, size_t chunk,
                                radio::MetaSplitter &sp) {
  sp.begin(metaint);
  std::vector<uint8_t> audio;
  std::vector<uint8_t> buf(chunk);
  size_t pos = 0;
  while (pos < stream.size()) {
    const size_t take = (stream.size() - pos) < chunk ? (stream.size() - pos) : chunk;
    memcpy(buf.data(), stream.data() + pos, take);
    const size_t got = sp.split(buf.data(), take, buf.data()); // IN-PLACE
    audio.insert(audio.end(), buf.data(), buf.data() + got);
    pos += take;
  }
  return audio;
}

static void testSplitter() {
  printf("\n[5] de-interleave do icy-metaint\n");

  const int32_t METAINT = 400; // pequeno de proposito: muitos blocos no teste

  // Audio com conteudo conhecido: rampa de 0..255 repetida. Qualquer byte a
  // mais ou a menos quebra a sequencia e o probe acusa.
  std::vector<uint8_t> audio;
  for (size_t i = 0; i < METAINT * 7 + 137; ++i)
    audio.push_back((uint8_t)(i & 0xFF));

  // Blocos de metadado nas posicoes conhecidas: um titulo, um vazio, outro
  // titulo com aspa interna, um vazio, e assim por diante.
  const char *metas[] = {
      "StreamTitle='PRIMEIRA - MUSICA';StreamUrl='';",
      "", // vazio: nada mudou
      "StreamTitle='ROCK N' ROLL';StreamUrl='';",
      "",
      "",
      "StreamTitle='TERCEIRA';StreamUrl='';",
      "",
  };
  const size_t metaCount = sizeof(metas) / sizeof(metas[0]);

  std::vector<uint8_t> stream;
  size_t consumed = 0, mi = 0;
  while (consumed < audio.size()) {
    const size_t take = (audio.size() - consumed) < (size_t)METAINT ? (audio.size() - consumed)
                                                                    : (size_t)METAINT;
    stream.insert(stream.end(), audio.begin() + consumed, audio.begin() + consumed + take);
    consumed += take;
    if (take == (size_t)METAINT) {
      appendMeta(stream, metas[mi % metaCount]);
      ++mi;
    }
  }
  printf("  stream sintetica: %zu bytes de audio, %zu blocos de metadado, "
         "%zu bytes no fio\n",
         audio.size(), mi, stream.size());

  // Tamanhos de leitura variados. 1 garante o pior caso; 7, 13 e 1021 sao
  // primos e caem no meio do byte de comprimento e no meio do texto; 400 e
  // 401 caem exatamente na fronteira e um byte depois dela.
  const size_t chunks[] = {1, 3, 7, 13, 16, 64, 100, 256, 399, 400, 401, 512, 1021, 4096, 65536};
  for (size_t ci = 0; ci < sizeof(chunks) / sizeof(chunks[0]); ++ci) {
    radio::MetaSplitter sp;
    const std::vector<uint8_t> got = run(stream, METAINT, chunks[ci], sp);
    char msg[128];
    snprintf(msg, sizeof(msg), "tamanho %zu: saiu %zu bytes de audio (esperado %zu)", chunks[ci],
             got.size(), audio.size());
    check(got.size() == audio.size(), msg);
    snprintf(msg, sizeof(msg), "tamanho %zu: bytes de audio identicos e contiguos", chunks[ci]);
    check(got.size() == audio.size() && memcmp(got.data(), audio.data(), audio.size()) == 0, msg);
    snprintf(msg, sizeof(msg), "tamanho %zu: contou %u blocos (esperado %zu)", chunks[ci],
             (unsigned)sp.blocks(), mi);
    check(sp.blocks() == mi, msg);
    // O ultimo titulo visto tem de ser o da TERCEIRA (os vazios nao apagam).
    snprintf(msg, sizeof(msg), "tamanho %zu: ultimo titulo preservado", chunks[ci]);
    check(strcmp(sp.title(), "TERCEIRA") == 0, msg);
  }

  // Um bloco que ATRAVESSA a fronteira de leitura, isolado e explicito: le-se
  // ate o meio do texto do metadado e so depois o resto.
  {
    radio::MetaSplitter sp;
    sp.begin(METAINT);
    std::vector<uint8_t> out(stream.size());
    // Primeira leitura: os 400 de audio + o byte de comprimento + 5 do texto.
    size_t got1 = sp.split(stream.data(), METAINT + 1 + 5, out.data());
    check(got1 == (size_t)METAINT, "leitura 1 parou no fim do audio");
    check(memcmp(out.data(), audio.data(), METAINT) == 0, "leitura 1: audio correto");
    check(strlen(sp.title()) == 0, "titulo ainda nao publicado (bloco partido)");
    // Segunda leitura: o resto do bloco + mais audio.
    const size_t rest = 1 + 5;
    const size_t metaBytes = 1 + ((strlen(metas[0]) + 15) / 16) * 16;
    const size_t second = (metaBytes - rest) + 120;
    size_t got2 = sp.split(stream.data() + METAINT + rest, second, out.data());
    check(got2 == 120, "leitura 2 devolveu so os 120 bytes de audio");
    check(memcmp(out.data(), audio.data() + METAINT, 120) == 0, "leitura 2: audio correto");
    checkStr(sp.title(), "PRIMEIRA - MUSICA", "titulo remontado do bloco partido");
  }

  // Um bloco cujo BYTE DE COMPRIMENTO fica sozinho no fim de uma leitura.
  {
    radio::MetaSplitter sp;
    sp.begin(METAINT);
    std::vector<uint8_t> out(stream.size());
    size_t got = sp.split(stream.data(), METAINT + 1, out.data()); // audio + so o byte de tamanho
    check(got == (size_t)METAINT, "byte de comprimento isolado nao virou audio");
    const size_t metaBytes = ((strlen(metas[0]) + 15) / 16) * 16;
    got = sp.split(stream.data() + METAINT + 1, metaBytes + 64, out.data());
    check(got == 64, "resto do bloco consumido, 64 de audio devolvidos");
    checkStr(sp.title(), "PRIMEIRA - MUSICA", "titulo lido apos comprimento isolado");
  }

  // metaint ausente: tudo passa direto, byte por byte igual.
  {
    radio::MetaSplitter sp;
    const std::vector<uint8_t> got = run(audio, -1, 333, sp);
    check(got.size() == audio.size() && memcmp(got.data(), audio.data(), audio.size()) == 0,
          "sem metaint: fluxo passa intacto");
    check(sp.blocks() == 0, "sem metaint: nenhum bloco contado");
  }

  // Todos os blocos vazios: o caso mais comum no ar (o titulo so muda quando a
  // musica troca). Nenhum byte de audio pode se perder.
  {
    std::vector<uint8_t> s2;
    size_t c = 0;
    size_t blocks = 0;
    while (c < audio.size()) {
      const size_t take = (audio.size() - c) < (size_t)METAINT ? (audio.size() - c) : (size_t)METAINT;
      s2.insert(s2.end(), audio.begin() + c, audio.begin() + c + take);
      c += take;
      if (take == (size_t)METAINT) {
        appendMeta(s2, "");
        ++blocks;
      }
    }
    for (size_t chunk : {size_t(1), size_t(17), size_t(400), size_t(4096)}) {
      radio::MetaSplitter sp;
      const std::vector<uint8_t> got = run(s2, METAINT, chunk, sp);
      char msg[96];
      snprintf(msg, sizeof(msg), "blocos vazios, tamanho %zu: audio intacto", chunk);
      check(got.size() == audio.size() && memcmp(got.data(), audio.data(), audio.size()) == 0, msg);
      snprintf(msg, sizeof(msg), "blocos vazios, tamanho %zu: %zu blocos contados", chunk, blocks);
      check(sp.blocks() == blocks, msg);
      check(strlen(sp.title()) == 0, "blocos vazios nao inventam titulo");
    }
  }

  // Bloco de metadado MAIOR que a janela de captura (224 B): o titulo sai
  // truncado, mas o audio NAO pode perder um byte — e esse o ponto.
  {
    std::string huge = "StreamTitle='";
    huge.append(600, 'A');
    huge += "';StreamUrl='http://exemplo';";
    std::vector<uint8_t> s3;
    s3.insert(s3.end(), audio.begin(), audio.begin() + METAINT);
    appendMeta(s3, huge);
    s3.insert(s3.end(), audio.begin() + METAINT, audio.begin() + METAINT * 2);
    appendMeta(s3, "");
    for (size_t chunk : {size_t(1), size_t(29), size_t(4096)}) {
      radio::MetaSplitter sp;
      const std::vector<uint8_t> got = run(s3, METAINT, chunk, sp);
      char msg[112];
      snprintf(msg, sizeof(msg), "bloco gigante (%zu B), tamanho %zu: audio intacto",
               huge.size(), chunk);
      check(got.size() == (size_t)METAINT * 2 &&
                memcmp(got.data(), audio.data(), METAINT * 2) == 0,
            msg);
    }
  }

  // Titulo com acento chegando pelo fio: sai ASCII do splitter.
  {
    std::vector<uint8_t> s4;
    s4.insert(s4.end(), audio.begin(), audio.begin() + METAINT);
    appendMeta(s4, "StreamTitle='SERTANEJO DO CORAÇÃO';StreamUrl='';");
    s4.insert(s4.end(), audio.begin() + METAINT, audio.begin() + METAINT * 2);
    radio::MetaSplitter sp;
    const std::vector<uint8_t> got = run(s4, METAINT, 37, sp);
    check(got.size() == (size_t)METAINT * 2, "acento: audio intacto");
    checkStr(sp.title(), "SERTANEJO DO CORACAO", "acento normalizado ja no splitter");
  }

  // A bandeira de titulo novo tem de disparar uma vez por MUDANCA, nao por bloco.
  {
    radio::MetaSplitter sp;
    sp.begin(METAINT);
    std::vector<uint8_t> out(stream.size());
    int flags = 0;
    size_t pos = 0;
    while (pos < stream.size()) {
      const size_t take = stream.size() - pos < 64 ? stream.size() - pos : 64;
      memcpy(out.data(), stream.data() + pos, take);
      sp.split(out.data(), take, out.data());
      if (sp.takeTitleFlag())
        ++flags;
      pos += take;
    }
    // Tres titulos distintos na tabela (PRIMEIRA, ROCK N' ROLL, TERCEIRA),
    // repetidos ciclicamente: 7 blocos no total nesta stream.
    char msg[80];
    snprintf(msg, sizeof(msg), "bandeira de titulo novo disparou %d vez(es)", flags);
    check(flags == 3, msg);
    printf("  %s\n", msg);
  }
}

// ---------------------------------------------------------------------------
//  6. Anel
// ---------------------------------------------------------------------------

static void testRing() {
  printf("\n[6] anel produtor-consumidor\n");
  uint8_t mem[64];
  radio::Ring r;
  r.attach(mem, sizeof(mem));
  check(r.capacity() == 64, "capacidade");
  check(r.used() == 0, "comeca vazio");
  check(r.space() == 63, "uma posicao reservada para distinguir cheio de vazio");

  uint8_t in[100], out[100];
  for (int i = 0; i < 100; ++i)
    in[i] = (uint8_t)i;

  check(r.write(in, 100) == 63, "escrita satura no espaco livre");
  check(r.used() == 63, "63 em uso");
  check(r.read(out, 100) == 63, "leitura devolve os 63");
  check(memcmp(out, in, 63) == 0, "conteudo preservado");
  check(r.used() == 0, "vazio de novo");

  // Da a volta muitas vezes com tamanhos que nao dividem a capacidade: e aqui
  // que um anel mal escrito embaralha bytes.
  uint32_t seq = 0, expect = 0;
  bool ok = true;
  for (int round = 0; round < 500 && ok; ++round) {
    uint8_t w[17];
    for (int i = 0; i < 17; ++i)
      w[i] = (uint8_t)(seq++ & 0xFF);
    const size_t wrote = r.write(w, 17);
    for (size_t i = 0; i < wrote; ++i)
      ; // ja contabilizado
    seq -= (17 - wrote); // devolve o que nao coube
    uint8_t rd[11];
    const size_t got = r.read(rd, 11);
    for (size_t i = 0; i < got; ++i) {
      if (rd[i] != (uint8_t)(expect & 0xFF)) {
        ok = false;
        break;
      }
      ++expect;
    }
  }
  check(ok, "500 voltas com escrita de 17 e leitura de 11 mantem a ordem");

  r.clear();
  check(r.used() == 0, "clear esvazia");
  check(r.read(out, 10) == 0, "leitura em anel vazio devolve 0");
}

// ---------------------------------------------------------------------------
//  7. Tabela de estacoes e rotulos
// ---------------------------------------------------------------------------

static void testStations() {
  printf("\n[7] tabela de estacoes e rotulos\n");
  check(radio::STATION_COUNT == 1, "uma estacao na tabela");
  checkStr(radio::STATIONS[0].name, "DIARIO FM", "nome da estacao");
  checkStr(radio::STATIONS[0].url, "http://51-222-26-208.webnow.com.br/diario.mp3", "URL");
  // Rotulos: ASCII sem acento, obrigatorio para as fontes bitmap.
  const radio::Conn all[] = {radio::Conn::Idle, radio::Conn::Connecting, radio::Conn::OnAir,
                             radio::Conn::NoSignal, radio::Conn::NoMemory};
  for (size_t i = 0; i < 5; ++i) {
    const char *l = radio::connLabel(all[i]);
    bool pure = true;
    for (const char *p = l; *p; ++p)
      if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7E)
        pure = false;
    check(pure, "rotulo e ASCII imprimivel");
  }
  checkStr(radio::connLabel(radio::Conn::OnAir), "NO AR", "rotulo NO AR");
  checkStr(radio::connLabel(radio::Conn::Connecting), "CONECTANDO", "rotulo CONECTANDO");
  checkStr(radio::connLabel(radio::Conn::NoSignal), "SEM SINAL", "rotulo SEM SINAL");

  printf("  orcamento de PSRAM: anel %zu + entrada MP3 %zu + PCM %zu = %zu bytes\n",
         radio::RING_BYTES, radio::MP3_IN_BYTES, radio::MP3_PCM_SHORTS * 2, radio::PSRAM_BYTES);
  check(radio::PSRAM_BYTES == 74240, "PSRAM total = 74240 B");
  printf("  anel = %.2f s de audio a 96 kbps; pre-buffer = %.2f s\n",
         radio::RING_BYTES / 12000.0, radio::PREBUFFER_BYTES / 12000.0);
  // SRAM estatica do modulo: o que NAO esta na PSRAM.
  printf("  sizeof(MetaSplitter)=%zu sizeof(HeaderParser)=%zu sizeof(Ring)=%zu (SRAM)\n",
         sizeof(radio::MetaSplitter), sizeof(radio::HeaderParser), sizeof(radio::Ring));
}

int main() {
  printf("=== probe_radio_stream — logica de rede do radio ===\n");
  testBackoff();
  testUrl();
  testRedirect();
  testHeader();
  testStreamTitle();
  testSplitter();
  testRing();
  testStations();
  printf("\n%d verificacoes, %d falha(s)\n", g_pass + g_fail, g_fail);
  return g_fail ? 1 : 0;
}
