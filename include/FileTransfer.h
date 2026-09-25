#pragma once
// ============================================================================
// FileTransfer.h — servidor HTTP embarcado para mandar video, musica e foto do
// computador para o cartao microSD pela rede local.
//
// Substitui o vaivem do cartao no leitor: a tela dedicada mostra IP, usuario e
// senha; o navegador do PC abre esse IP e envia os arquivos. Enquanto a tela
// esta ativa o resto do aparelho fica suspenso, entao este modulo pode usar a
// banda do cartao inteira — mas *nao* pode monopolizar o `sdMutex`, porque a
// tarefa de audio continua viva (ver secao "Disciplina do cartao").
//
// ---------------------------------------------------------------------------
// DECISOES DE DESENHO, com o porque
// ---------------------------------------------------------------------------
//
// 1. HTTP puro, sem TLS. No Fruit Jam nem ha escolha: a rede e o ESP32-C6 com
//    firmware NINA (WiFiNINA, por SPI1), e o NINA so faz TLS do lado CLIENTE.
//    O trafego fica dentro da LAN e a senha e descartavel, valida so enquanto
//    a tela estiver aberta. Quem precisar de sigilo de verdade continua com o
//    leitor de cartao.
//
// 2. Sem tarefa FreeRTOS: `handle()` e chamado do `loop()` e trabalha por
//    fatias curtas (SLICE_MS), sem `delay()` e sem laco bloqueante. Toda
//    chamada WiFiNINA roda com `net::Lock` tomado, UMA operacao por vez (um
//    read, um write de ate WRITE_CHUNK, um status), nunca a fatia inteira: o
//    SPI1 e dividido com as outras tarefas de rede. O custo disso e que o
//    `loop()` espera se outra tarefa estiver no meio de uma transacao longa
//    (um connect HTTPS da previsao) — com a tela de transferencia aberta o
//    resto do aparelho fica suspenso, entao isso e raro.
//
//    Limites do NINA que moldam o codigo abaixo: poucos sockets no C6
//    (CONFIG_LWIP_MAX_SOCKETS, divididos com radio, previsao e radar) — aqui
//    sao dois, o de escuta e UM cliente por vez; leitura de no maximo 1500 B
//    por transacao (o WiFiSocketBuffer do WiFiNINA); escrita de no maximo ~4 KB
//    por transacao (o buffer de comando do C6), por isso WRITE_CHUNK; e o
//    socket de escuta nunca fecha (ver detail::Listener).
//
// 3. Buffer de transferencia em PSRAM (`ps_malloc`), nunca na pilha e nunca na
//    SRAM interna. O upstream ja estourou pilha duas vezes exatamente assim
//    (AGENTS 2.3). Se a PSRAM faltar, `begin()` falha em vez de cair para a
//    SRAM — o framebuffer do DVI precisa dos 153.600 bytes dele.
//
// 4. `PUT /upload/<caminho>` com o corpo cru, sem multipart. Multipart exigiria
//    procurar a fronteira dentro do fluxo, guardar resto entre blocos e
//    reconstruir o nome do arquivo a partir de um cabecalho que o cliente
//    escolhe — mais estado, mais superficie de ataque e um buffer a mais, para
//    nada: com PUT o corpo *e* o arquivo, e cada byte que sai do socket vai
//    direto para o cartao. A pagina embutida usa `fetch(..., {method:'PUT'})`.
//
// 5. Tamanho do bloco: 8 KiB (`BLOCK`). O cartao escreve em setores de 512 B, e
//    8 KiB sao 16 setores inteiros — nao ha leitura-modificacao-escrita no
//    SdFat, e como a gravacao e sequencial dentro do cluster (tipicamente
//    32 KiB no FAT32) tambem nao ha desperdicio de cluster. Pelo outro lado:
//    8 KiB a ~1,5 MB/s sao ~5 ms segurando o `sdMutex`, folgado dentro do prazo
//    da tarefa de audio; 32 KiB seriam ~20 ms e ja arriscariam um buraco no
//    som. Menor que 8 KiB multiplicaria as idas ao mutex sem ganhar nada. Como
//    o socket do NINA entrega no maximo 1500 B por vez, os pedacos se acumulam
//    no buffer ate fechar um bloco (ver pumpBody).
//
// 6. Instalacao atomica por arquivo: grava em `<destino>.part` e so renomeia
//    quando o ultimo byte chegou. Uma queda no meio deixa um `.part`, que o
//    player ignora, e nunca um `video.mjpeg` pela metade.
//
//    LIMITE CONHECIDO: a atomicidade e *por arquivo*, nao por programa. Um
//    programa de video sao tres arquivos (meta.json, video.mjpeg, audio.wav) e
//    o scanner da biblioteca aceita a pasta assim que enxerga o meta.json. Por
//    isso a pagina embutida envia o `meta.json` por ultimo: ate ele chegar, a
//    pasta nao parece um programa valido. Um cliente escrito a mao pode furar
//    essa ordem — esta anotado no relatorio como ponto de revisao.
//
// 7. Retomada por `Range: bytes=<inicio>-`. Um arquivo de 45 MB nao pode
//    recomecar por causa de uma queda de Wi-Fi. `HEAD /upload/<caminho>`
//    responde `X-Resume-Offset` com o tamanho atual do `.part`, e o `.part`
//    sobrevive a um `stop()` justamente para permitir a retomada depois.
//
//    A pagina embutida manda *tambem* `X-Upload-Offset` com o mesmo valor: o
//    `Range` so saiu da lista de cabecalhos proibidos do `fetch` em 2023, e um
//    navegador antigo que o descarte em silencio faria o servidor gravar um
//    pedaco no inicio do arquivo. Com o cabecalho proprio esse buraco fecha.
//
// 8. Tres categorias, nao duas: videos/, music/ e fotos/. A categoria e o
//    PRIMEIRO SEGMENTO do caminho, validado contra uma lista fechada — e a
//    mesma trava que impede escrever em config/ ou fora do /M5RETRO. A pasta
//    fotos/ e a unica com filtro de conteudo, por um motivo concreto: o
//    slideshow le .jpg/.jpeg e o JPEGDEC do firmware nao abre tudo o que e
//    JPEG valido. Entao a recusa acontece em tres alturas, da mais barata para
//    a mais cara:
//      a) extensao, no `sanitizePath` — nem chega a abrir socket de arquivo;
//      b) tamanho, no `startBody` — antes de gravar o primeiro byte, porque uma
//         foto de celular tem de 500 KB a 5 MB e o teto e 128 KB;
//      c) cabecalho JPEG, no `completeBody` — antes do rename, lendo de volta
//         o `.part` ja fechado. Progressivo e 4:4:4 sao os dois casos que o
//         decodificador erra em silencio (ver `inspectJpegHeader`).
//    Nenhuma delas afrouxa nada: sao filtros A MAIS sobre o caminho que ja
//    passou pela validacao inteira.
//
// ---------------------------------------------------------------------------
// Disciplina do cartao
// ---------------------------------------------------------------------------
// O `sdMutex` e tomado e devolvido **por bloco**, nunca durante o arquivo
// inteiro — mesma disciplina do `runVideoBenchmark` em `src/main.cpp`. Segurar
// o mutex por 45 MB mataria a tarefa de audio de fome. O handle do mutex chega
// por `begin()` (injecao de dependencia) porque ele mora em `main.cpp` e este
// modulo nao pode ver aquele arquivo.
//
// ---------------------------------------------------------------------------
// Custo de memoria
// ---------------------------------------------------------------------------
// Medido no ESP32 original (xtensa-esp32-elf-nm): objeto FileTransfer com
// ~1,2 KB de .bss; no Fruit Jam soma ~40 B de estado de rede. Alem disso:
//   TRANSFER_PAGE       : ~1,3 KB de .rodata (flash, nao SRAM)
//   buffer de bloco     : 8 KiB de PSRAM, alocado em begin(), liberado em stop()
//   WiFiServer          : um objeto de poucos bytes, criado uma vez e nunca
//                         liberado (o socket do C6 tambem nao fecha)
//   WiFiNINA            : 1500 B de SRAM (malloc do WiFiSocketBuffer) por
//                         socket em uso, liberados no stop() do socket
//   pilha               : nenhuma alocacao grande; tudo o que e grande e membro
//
// ---------------------------------------------------------------------------
// Como testar
// ---------------------------------------------------------------------------
// Toda a logica que da para errar em silencio — validacao de caminho, base64,
// Basic, Range, instalacao atomica — esta na primeira metade do arquivo e nao
// depende de Arduino, WiFi nem SD. `tests/test_transfer.cpp` define
// `FILETRANSFER_NO_NETWORK` antes de incluir este header e exercita so essa
// parte, que e como o resto do repositorio separa logica pura de I/O.
// ============================================================================

#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace xfer {

// Capacidade dos buffers de caminho. Ja inclui espaco para o sufixo ".part":
// quem chama `sanitizePath` com um buffer de `PATH_CAP` pode chamar `partPath`
// em seguida sem checar de novo.
enum {
  PATH_CAP = 160,     // "/M5RETRO/videos/" + pasta + "/" + arquivo + ".part"
  MAX_SEGMENT = 64,   // limite do FAT para um nome de arquivo ou pasta
  MAX_SECRET = 64,    // usuario e senha do Basic
  MAX_CRED = 132,     // "usuario:senha" decodificado do base64
  PART_SUFFIX_LEN = 5 // strlen(".part")
};

// O FAT32 nao guarda arquivo de 4 GiB ou mais; tudo aqui e limitado por isso.
static const uint64_t MAX_FILE = 0xFFFFFFFFull;

// Prefixo obrigatorio. As tres unicas pastas que aceitam envio sao
// /M5RETRO/videos/, /M5RETRO/music/ e /M5RETRO/fotos/ — o resto do cartao
// (config, secrets, cache) fica fora do alcance de quem tem a senha da sessao.
static const char CARD_ROOT[] = "/M5RETRO/";
static const char PART_SUFFIX[] = ".part";

// Pasta de fotos, ja com o prefixo do cartao. A tela de slideshow le daqui, e
// `isPhotoPath` usa esta string para reconhecer a categoria depois que o
// caminho ja foi sanitizado — sem reanalisar a entrada crua uma segunda vez.
static const char FOTOS_DIR[] = "/M5RETRO/fotos/";

// Teto de tamanho de uma foto. ESPELHA `MAX_JPEG` em src/main.cpp (128 KiB de
// PSRAM para o buffer de decodificacao): acima disso o aparelho nunca conseguira
// abrir o arquivo, entao recusar no envio e mais honesto do que gravar 3 MB no
// cartao para o slideshow pular depois em silencio. Se o MAX_JPEG de la mudar,
// este valor tem de mudar junto — sao dois arquivos que nao se enxergam.
static const uint64_t MAX_PHOTO = 128u * 1024u;

enum class PathError : uint8_t {
  OK,
  VAZIO,      // nada depois de /upload/, ou barra dupla, ou barra no fim
  ABSOLUTO,   // comecava com '/'
  ESCAPA,     // segmento "." ou ".."
  CARACTERE,  // controle, nao-ASCII, escape % quebrado ou caractere proibido no FAT
  LONGO,      // estourou PATH_CAP ou MAX_SEGMENT
  RAIZ,       // primeiro segmento nao e "videos", "music" nem "fotos"
  INCOMPLETO, // so a pasta raiz, sem nome de arquivo
  EXTENSAO    // extensao que a categoria nao aceita (hoje so fotos/ filtra)
};

// Texto curto e ASCII para mandar ao navegador e mostrar na TV.
inline const char *pathErrorText(PathError error) {
  switch (error) {
  case PathError::OK:
    return "ok";
  case PathError::VAZIO:
    return "caminho vazio";
  case PathError::ABSOLUTO:
    return "caminho absoluto recusado";
  case PathError::ESCAPA:
    return "segmento . ou .. recusado";
  case PathError::CARACTERE:
    return "caractere invalido no nome";
  case PathError::LONGO:
    return "caminho longo demais";
  case PathError::RAIZ:
    return "use videos/, music/ ou fotos/";
  case PathError::INCOMPLETO:
    return "falta o nome do arquivo";
  case PathError::EXTENSAO:
    return "fotos aceita so .jpg ou .jpeg";
  }
  return "caminho invalido";
}

// strlen com teto: o texto vem do socket e pode nao ter terminador dentro do
// buffer se alguem errar em outro lugar.
inline size_t boundedLen(const char *text, size_t cap) {
  size_t n = 0;
  while (n < cap && text[n])
    ++n;
  return n;
}

// Fica aqui em cima, e nao junto do parser de cabecalho, porque a validacao de
// caminho tambem precisa dela para comparar extensao sem sensibilidade a caixa.
inline char lowerAscii(char c) {
  return (c >= 'A' && c <= 'Z') ? char(c + 32) : c;
}

inline int hexDigit(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

// Decodifica %XX. Devolve o comprimento, -1 para escape quebrado e -2 quando
// nao cabe. Para na '?' e no '#': query e fragmento nao fazem parte do caminho.
// Decodificar ANTES de validar e o que faz "%2e%2e%2f" ser pego pela regra do
// ".." em vez de passar disfarcado.
inline int percentDecode(const char *src, char *out, size_t cap) {
  size_t o = 0;
  for (size_t i = 0; src[i]; ++i) {
    char c = src[i];
    if (c == '?' || c == '#')
      break;
    if (c == '%') {
      const int hi = hexDigit(src[i + 1]);
      const int lo = hi < 0 ? -1 : hexDigit(src[i + 2]);
      if (hi < 0 || lo < 0)
        return -1;
      c = char((hi << 4) | lo);
      if (c == '\0')
        return -1; // %00 truncaria o caminho depois da validacao
      i += 2;
    }
    if (o + 1 >= cap)
      return -2;
    out[o++] = c;
  }
  out[o] = '\0';
  return int(o);
}

// Caracteres que o FAT nao aceita num nome (a barra e separador e e tratada
// fora daqui).
inline bool forbiddenInName(char c) {
  return strchr("\\:*?\"<>|", c) != NULL;
}

// Sufixo sem sensibilidade a caixa. `len > suffixLen` de proposito: um arquivo
// chamado so ".jpg" nao tem nome, e o FAT o esconderia.
inline bool endsWithNoCase(const char *seg, size_t len, const char *suffix, size_t suffixLen) {
  if (len <= suffixLen)
    return false;
  for (size_t i = 0; i < suffixLen; ++i)
    if (lowerAscii(seg[len - suffixLen + i]) != suffix[i])
      return false;
  return true;
}

// Nome aceito na pasta de fotos. So JPEG, porque e o unico formato que o
// JPEGDEC do firmware abre; PNG, HEIC ou .exe ali so ocupariam cartao e
// apareceriam como item quebrado no slideshow. Camera e celular gravam ".JPG"
// tanto quanto ".jpg", entao a comparacao ignora a caixa.
inline bool isPhotoName(const char *seg, size_t len) {
  return endsWithNoCase(seg, len, ".jpg", 4) || endsWithNoCase(seg, len, ".jpeg", 5);
}

// Um caminho JA SANITIZADO cai na pasta de fotos? Fica separado de
// `sanitizePath` para nao mexer na assinatura que os testes nativos e o
// servidor ja usam — quem precisa da categoria depois (o teto de tamanho, a
// conferencia do cabecalho JPEG) pergunta aqui.
inline bool isPhotoPath(const char *cardPath) {
  return cardPath && strncmp(cardPath, FOTOS_DIR, sizeof(FOTOS_DIR) - 1) == 0;
}

// Cabe no teto de decodificacao do aparelho? Puro para ser testavel sem rede.
// Zero nao e tratado aqui: um arquivo vazio cai na conferencia do cabecalho,
// que devolve um motivo mais exato ("arquivo nao e jpeg").
inline bool fitsPhotoBudget(uint64_t totalBytes) {
  return totalBytes <= MAX_PHOTO;
}

// Transforma o que veio depois de "/upload/" num caminho absoluto no cartao.
// Entrada NAO CONFIAVEL: tudo o que nao for explicitamente permitido e negado.
//
// Aceita: videos/<...>/<arquivo>, music/<...>/<arquivo> e fotos/<...>/<arquivo>
// Nega  : caminho absoluto, "." e "..", barra dupla ou final, byte de controle,
//         qualquer coisa fora do ASCII imprimivel, caractere proibido no FAT,
//         ponto ou espaco no fim de um segmento (o FAT os descarta em silencio,
//         e "video.mjpeg." viraria "video.mjpeg" sem o chamador saber),
//         qualquer primeiro segmento que nao seja videos, music ou fotos, e —
//         so em fotos/ — qualquer arquivo que nao termine em .jpg ou .jpeg.
//
// A checagem de extensao vale para a categoria inteira, inclusive em subpasta
// (fotos/viagem/praia.jpg), e acontece DEPOIS da mesma varredura de caracteres
// e de ".." que as outras categorias — a categoria nova nao ganha atalho nenhum
// na validacao de caminho.
//
// `out` precisa ter PATH_CAP bytes; a folga do ".part" ja esta reservada.
// Valida a acao do controle remoto vinda da URL: so letras minusculas, curta,
// terminada. Fica aqui, na metade sem rede, porque e logica que erra em
// silencio e por isso precisa de teste -- a mesma razao que separou o
// sanitizePath. Devolve false e esvazia `out` para qualquer coisa fora disso.
inline bool sanitizeAction(const char *raw, char *out, size_t cap) {
  if (!out || cap == 0)
    return false;
  out[0] = '\0';
  if (!raw || !*raw)
    return false;
  // Valida ANTES de escrever. Escrevendo durante a varredura, um "ok/" deixava
  // "ok" em `out` ao recusar, e quem ignorasse o retorno veria uma acao
  // plausivel montada a partir de uma entrada invalida.
  size_t n = 0;
  for (; raw[n]; ++n) {
    if (raw[n] < 'a' || raw[n] > 'z')
      return false; // inclusive '/', '.', '%', espaco e maiuscula
    if (n >= cap - 1)
      return false; // longa demais: recusa, nao trunca
  }
  if (!n)
    return false;
  memcpy(out, raw, n);
  out[n] = '\0';
  return true;
}

inline PathError sanitizePath(const char *raw, char *out, size_t cap) {
  if (!raw || !out || cap < sizeof(CARD_ROOT) + PART_SUFFIX_LEN + 2)
    return PathError::LONGO;

  char decoded[PATH_CAP];
  const int decodedLen = percentDecode(raw, decoded, sizeof(decoded));
  if (decodedLen == -2)
    return PathError::LONGO;
  if (decodedLen < 0)
    return PathError::CARACTERE;
  if (decodedLen == 0)
    return PathError::VAZIO;
  if (decoded[0] == '/')
    return PathError::ABSOLUTO;

  size_t start = 0, segments = 0;
  bool photoRoot = false;
  for (size_t i = 0; i <= size_t(decodedLen); ++i) {
    const char c = decoded[i];
    if (c != '/' && c != '\0') {
      const unsigned char u = (unsigned char)c;
      // Fora do ASCII imprimivel nao ha glifo nas fontes bitmap (AGENTS 2.7) e
      // um nome acentuado viraria buraco duplo na tela da biblioteca.
      if (u < 0x20 || u >= 0x7f || forbiddenInName(c))
        return PathError::CARACTERE;
      continue;
    }
    const size_t len = i - start;
    if (len == 0)
      return PathError::VAZIO;
    if (len > MAX_SEGMENT)
      return PathError::LONGO;
    if (len == 1 && decoded[start] == '.')
      return PathError::ESCAPA;
    if (len == 2 && decoded[start] == '.' && decoded[start + 1] == '.')
      return PathError::ESCAPA;
    if (decoded[i - 1] == ' ' || decoded[i - 1] == '.')
      return PathError::CARACTERE;
    if (segments == 0) {
      const bool videos = len == 6 && memcmp(decoded, "videos", 6) == 0;
      const bool music = len == 5 && memcmp(decoded, "music", 5) == 0;
      photoRoot = len == 5 && memcmp(decoded, "fotos", 5) == 0;
      if (!videos && !music && !photoRoot)
        return PathError::RAIZ;
    } else if (c == '\0' && photoRoot && !isPhotoName(decoded + start, len)) {
      // Ultimo segmento: e o nome do arquivo. So aqui a extensao importa — uma
      // subpasta chamada "verao" continua valendo dentro de fotos/.
      return PathError::EXTENSAO;
    }
    ++segments;
    start = i + 1;
    if (c == '\0')
      break;
  }
  if (segments < 2)
    return PathError::INCOMPLETO;

  const size_t prefix = sizeof(CARD_ROOT) - 1;
  if (prefix + size_t(decodedLen) + PART_SUFFIX_LEN + 1 > cap)
    return PathError::LONGO;
  memcpy(out, CARD_ROOT, prefix);
  memcpy(out + prefix, decoded, size_t(decodedLen) + 1);
  return PathError::OK;
}

// Nome curto para a tela mostrar. Sem alocar: copia o que vem depois da ultima
// barra, truncando se preciso.
inline void baseName(const char *path, char *out, size_t cap) {
  if (!cap)
    return;
  const size_t n = boundedLen(path, PATH_CAP);
  size_t start = 0;
  for (size_t i = 0; i < n; ++i)
    if (path[i] == '/')
      start = i + 1;
  size_t len = n - start;
  if (len > cap - 1)
    len = cap - 1;
  memcpy(out, path + start, len);
  out[len] = '\0';
}

inline bool partPath(const char *finalPath, char *out, size_t cap) {
  const size_t n = boundedLen(finalPath, cap);
  if (n == 0 || n + PART_SUFFIX_LEN + 1 > cap)
    return false;
  memcpy(out, finalPath, n);
  memcpy(out + n, PART_SUFFIX, PART_SUFFIX_LEN + 1);
  return true;
}

// Comparacao em tempo constante. Nao ha saida antecipada e o numero de voltas
// nao depende do conteudo: o atacante nao aprende o prefixo certo medindo o
// tempo da resposta. O XOR dos comprimentos entra no acumulador para que
// "SENHA" e "SENHAX" tambem difiram.
inline bool secretEquals(const char *expected, const char *provided) {
  if (!expected || !provided)
    return false;
  const size_t le = boundedLen(expected, MAX_SECRET);
  const size_t lp = boundedLen(provided, MAX_SECRET);
  unsigned diff = (unsigned)(le ^ lp);
  for (size_t i = 0; i < size_t(MAX_SECRET); ++i) {
    const uint8_t x = i < le ? (uint8_t)expected[i] : 0u;
    const uint8_t y = i < lp ? (uint8_t)provided[i] : 0u;
    diff |= (unsigned)(x ^ y);
  }
  return diff == 0;
}

inline int base64Value(char c) {
  if (c >= 'A' && c <= 'Z')
    return c - 'A';
  if (c >= 'a' && c <= 'z')
    return c - 'a' + 26;
  if (c >= '0' && c <= '9')
    return c - '0' + 52;
  if (c == '+')
    return 62;
  if (c == '/')
    return 63;
  return -1;
}

// base64 estrito: comprimento multiplo de 4, '=' so no ultimo grupo, nenhum
// caractere fora do alfabeto. Estrito de proposito — aceitar lixo aqui so
// serviria para dar mais caminhos a quem esta sondando a autenticacao.
// Devolve o numero de bytes decodificados ou -1.
inline int base64Decode(const char *src, size_t len, uint8_t *out, size_t cap) {
  if (len % 4 != 0)
    return -1;
  size_t o = 0;
  for (size_t i = 0; i < len; i += 4) {
    int value[4];
    int padding = 0;
    for (int k = 0; k < 4; ++k) {
      const char c = src[i + k];
      if (c == '=') {
        if (i + 4 < len || k < 2)
          return -1; // padding fora do ultimo grupo, ou grupo so de padding
        value[k] = 0;
        ++padding;
      } else {
        if (padding)
          return -1; // dado depois do padding
        value[k] = base64Value(c);
        if (value[k] < 0)
          return -1;
      }
    }
    const uint32_t triple = (uint32_t(value[0]) << 18) | (uint32_t(value[1]) << 12) |
                            (uint32_t(value[2]) << 6) | uint32_t(value[3]);
    for (int k = 0; k < 3 - padding; ++k) {
      if (o >= cap)
        return -1;
      out[o++] = uint8_t(triple >> (16 - 8 * k));
    }
  }
  return int(o);
}

inline bool isSpace(char c) {
  return c == ' ' || c == '\t';
}

inline bool matchesToken(const char *text, const char *lowerToken, size_t len) {
  for (size_t i = 0; i < len; ++i)
    if (lowerAscii(text[i]) != lowerToken[i])
      return false;
  return true;
}

// Quebra o valor de "Authorization: Basic <base64>" em usuario e senha.
// Nao compara nada — quem compara e `secretEquals`, para que a comparacao
// continue em tempo constante mesmo quando o cabecalho vem malformado.
inline bool parseBasicAuth(const char *header, char *user, size_t userCap, char *pass, size_t passCap) {
  if (!header || !userCap || !passCap)
    return false;
  while (isSpace(*header))
    ++header;
  if (!matchesToken(header, "basic", 5))
    return false;
  header += 5;
  if (!isSpace(*header))
    return false;
  while (isSpace(*header))
    ++header;
  size_t len = boundedLen(header, 4 * MAX_SECRET);
  while (len && isSpace(header[len - 1]))
    --len;
  if (len == 0)
    return false;

  uint8_t raw[MAX_CRED];
  const int n = base64Decode(header, len, raw, sizeof(raw));
  if (n <= 0)
    return false;
  int colon = -1;
  for (int i = 0; i < n; ++i) {
    if (raw[i] < 0x20 || raw[i] >= 0x7f)
      return false;
    if (raw[i] == ':' && colon < 0)
      colon = i;
  }
  if (colon < 0)
    return false;
  const size_t userLen = size_t(colon), passLen = size_t(n - colon - 1);
  if (userLen + 1 > userCap || passLen + 1 > passCap)
    return false;
  memcpy(user, raw, userLen);
  user[userLen] = '\0';
  memcpy(pass, raw + colon + 1, passLen);
  pass[passLen] = '\0';
  return true;
}

// Le "Range: bytes=<inicio>-[<fim>]" e devolve so o inicio, que e o unico
// numero que um envio precisa. Recusa de proposito:
//   - "bytes=-500" (sufixo): nao existe "os ultimos 500 bytes" de um envio;
//   - varios intervalos separados por virgula: nao ha semantica de envio;
//   - valor >= 4 GiB: o FAT32 nao guarda arquivo desse tamanho.
inline bool parseRange(const char *value, uint64_t &start) {
  if (!value)
    return false;
  const char *p = value;
  while (isSpace(*p))
    ++p;
  if (!matchesToken(p, "bytes", 5))
    return false;
  p += 5;
  while (isSpace(*p))
    ++p;
  if (*p != '=')
    return false;
  ++p;
  while (isSpace(*p))
    ++p;
  if (*p < '0' || *p > '9')
    return false;
  uint64_t begin = 0;
  while (*p >= '0' && *p <= '9') {
    begin = begin * 10 + uint64_t(*p - '0');
    if (begin > MAX_FILE)
      return false;
    ++p;
  }
  if (*p != '-')
    return false;
  ++p;
  uint64_t last = 0;
  int endDigits = 0;
  while (*p >= '0' && *p <= '9') {
    ++endDigits;
    last = last * 10 + uint64_t(*p - '0');
    if (last > MAX_FILE)
      return false;
    ++p;
  }
  if (endDigits && last < begin)
    return false;
  while (isSpace(*p))
    ++p;
  if (*p != '\0')
    return false;
  start = begin;
  return true;
}

// Espaco livre antes de comecar. A reserva existe porque o FAT precisa de
// cluster para a propria tabela e porque encher o cartao ate o ultimo byte
// impede ate a gravacao do settings.json depois.
inline bool fitsOnCard(uint64_t freeBytes, uint64_t needed, uint64_t reserve) {
  if (needed > MAX_FILE)
    return false;
  const uint64_t required = needed + reserve;
  if (required < needed)
    return false;
  return freeBytes >= required;
}

// ---------------------------------------------------------------------------
// Conferencia do cabecalho JPEG
// ---------------------------------------------------------------------------
// O JPEGDEC do firmware nao abre tudo o que e JPEG valido, e os dois casos que
// ele erra erram CALADOS — e por isso que a checagem vale a pena aqui, onde
// ainda da para devolver um texto ao navegador:
//
//   * progressivo (SOF2/SOF6/SOF10): `decode()` devolve SUCCESS e desenha so
//     40x30 pixels, os coeficientes DC da primeira varredura. A maioria dos
//     exportadores web emite progressivo por padrao, entao isto e comum.
//   * 4:4:4 (SOF com o Y em 1x1): `decode()` devolve JPEG_DECODE_ERROR.
//
// Isto olha SO o cabecalho, ate o SOS: nao decodifica nada e nao aloca nada.
// Quando os bytes oferecidos acabam antes do SOF a resposta e INDETERMINADO, e
// a regra do chamador e **nao acusar sem prova** — um arquivo de EXIF gigante
// passa e o slideshow que se defenda, o que e melhor do que recusar uma foto
// boa.
enum class JpegVerdict : uint8_t {
  OK,            // baseline, subamostragem que o decodificador aceita
  INDETERMINADO, // o SOF nao apareceu nos bytes oferecidos
  NAO_E_JPEG,    // nem comeca com SOI
  PROGRESSIVO,   // SOF2/SOF6/SOF10
  CHROMA_444     // SOF de 3 componentes com o Y em 1x1
};

inline JpegVerdict inspectJpegHeader(const uint8_t *data, size_t len) {
  // Menos de dois bytes nao pode ser JPEG — inclusive o arquivo vazio, que e a
  // unica prova negativa que este tamanho permite.
  if (!data || len < 2)
    return JpegVerdict::NAO_E_JPEG;
  if (data[0] != 0xFF || data[1] != 0xD8)
    return JpegVerdict::NAO_E_JPEG;

  size_t i = 2;
  while (i + 3 < len) {
    if (data[i] != 0xFF)
      return JpegVerdict::INDETERMINADO; // fora de sincronia: nao da para acusar
    const uint8_t marker = data[i + 1];
    if (marker == 0xFF) { // preenchimento entre marcadores e legal
      ++i;
      continue;
    }
    // Marcadores sem payload: TEM, RSTn, SOI, EOI.
    if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD9)) {
      i += 2;
      continue;
    }
    if (marker == 0xC2 || marker == 0xC6 || marker == 0xCA)
      return JpegVerdict::PROGRESSIVO;
    if (marker == 0xC0 || marker == 0xC1) {
      // Payload do SOF: precisao(1) altura(2) largura(2) componentes(1) e
      // depois 3 bytes por componente — id, amostragem, tabela de quantizacao.
      const size_t sof = i + 4;
      if (sof + 5 >= len)
        return JpegVerdict::INDETERMINADO;
      if (data[sof + 5] == 3) { // Y/Cb/Cr
        if (sof + 7 >= len)
          return JpegVerdict::INDETERMINADO;
        const uint8_t sampling = data[sof + 7]; // do primeiro componente (Y)
        if ((sampling >> 4) == 1 && (sampling & 0x0F) == 1)
          return JpegVerdict::CHROMA_444;
      }
      return JpegVerdict::OK;
    }
    if (marker == 0xDA) // SOS: acabou o cabecalho sem SOF reconhecido
      return JpegVerdict::INDETERMINADO;
    const size_t seg = (size_t(data[i + 2]) << 8) | size_t(data[i + 3]);
    if (seg < 2)
      return JpegVerdict::INDETERMINADO; // comprimento impossivel
    i += 2 + seg;
  }
  return JpegVerdict::INDETERMINADO;
}

// Texto do motivo, ou NULL quando a foto pode ser instalada. Curto porque a
// mesma string vai para o corpo HTTP e para a faixa de estado da TV, onde
// cabem ~44 caracteres antes de o texto ser truncado.
inline const char *jpegRejectText(JpegVerdict verdict) {
  switch (verdict) {
  case JpegVerdict::NAO_E_JPEG:
    return "arquivo nao e jpeg";
  case JpegVerdict::PROGRESSIVO:
    return "jpeg progressivo: salve como baseline";
  case JpegVerdict::CHROMA_444:
    return "jpeg 4:4:4: salve com 4:2:0";
  case JpegVerdict::OK:
  case JpegVerdict::INDETERMINADO:
    break;
  }
  return NULL;
}

// Instalacao atomica: o `.part` completo vira o arquivo final num rename, que
// no FAT e uma troca de entrada de diretorio. Ou o arquivo esta inteiro, ou
// nao existe.
//
// O FAT nao deixa o rename sobrescrever um destino, entao o antigo e removido
// antes. A janela entre o remove e o rename e de uma entrada de diretorio; nao
// ha backup como em `SafeStorage.h` porque duplicar um video de 45 MB pediria
// espaco que o cartao normalmente nao tem — e, ao contrario do settings.json,
// um video perdido se reenvia.
//
// Se o rename falhar, o `.part` FICA: uma nova tentativa retoma de onde parou
// em vez de reenviar tudo.
template <class FS> bool installFile(FS &fs, const char *part, const char *finalPath) {
  if (!fs.exists(part))
    return false;
  if (fs.exists(finalPath) && !fs.remove(finalPath))
    return false;
  return fs.rename(part, finalPath);
}

// Cria as pastas intermediarias de `path` (o ultimo segmento e o arquivo).
// Usa o buffer do chamador para nao alocar; `scratch` precisa de `cap` bytes.
template <class FS> bool ensureParents(FS &fs, const char *path, char *scratch, size_t cap) {
  const size_t n = boundedLen(path, cap);
  if (n == 0 || n >= cap)
    return false;
  memcpy(scratch, path, n + 1);
  for (size_t i = 1; i < n; ++i) {
    if (scratch[i] != '/')
      continue;
    scratch[i] = '\0';
    const bool ok = fs.exists(scratch) || fs.mkdir(scratch);
    scratch[i] = '/';
    if (!ok)
      return false;
  }
  return true;
}

} // namespace xfer

#ifndef FILETRANSFER_NO_NETWORK
// ============================================================================
// Metade de rede. Os testes nativos nao compilam nada daqui para baixo.
// ============================================================================
#include <Arduino.h>
#include <stdio.h>
#include <stdlib.h>
#include <SD.h> // SD do arduino-pico (SDIO, fj/Storage.h); FILE_WRITE aqui ANEXA

// NUNCA <WiFi.h>: o arduino-pico tem uma WiFi propria (lwIP/CYW43) com classes
// de mesmo nome, e as duas juntas quebram o link. A rede do Fruit Jam e o
// WiFiNINA, que chega por fj/Net.h junto com o net::Lock.
#include "fj/Net.h"
#include "fj/Platform.h" // FreeRTOS (semaforo do sdMutex), ps_malloc, esp_random

namespace xfer {

namespace detail {
// O socket de escuta do NINA NAO FECHA. O WiFiNINA nao tem WiFiServer::end(), e
// no firmware NINA o comando de fechar socket (stopClientTcp) so fecha o lado
// cliente: o NetworkServer do slot continua escutando. Criar um WiFiServer novo
// a cada visita a tela gastaria um slot do C6 por visita (sao poucos, divididos
// com radio, previsao e radar) e ainda deixaria dois servidores disputando a
// mesma porta. Entao o servidor e um so por programa, criado na primeira visita
// e REUSADO nas seguintes; fora da tela ele so deixa de ser atendido.
// Estado ESTABLISHED do `wl_tcp_state` do WiFiNINA (utility/wifi_spi.h). Aquele
// header nao e publico e traz uma penca de #define de comando SPI; o valor e
// fixo no protocolo e o firmware NINA so devolve 4 ou 0 (getClientStateTcp).
static const uint8_t NINA_TCP_ESTABLISHED = 4;

struct Listener {
  WiFiServer *server;
  uint16_t port;
};
inline Listener &listener() {
  static Listener l = {nullptr, 0};
  return l;
}
} // namespace detail

enum class TransferStatus : uint8_t {
  PARADO,    // begin() ainda nao foi chamado, ou stop() ja foi
  AGUARDANDO, // servidor no ar, ninguem enviando
  RECEBENDO,  // arquivo em curso; received()/total() valem
  CONCLUIDO,  // ultimo arquivo instalado
  FALHA       // lastError() explica
};

// Pagina de envio, em flash. Minima de proposito: quanto menor, menos flash e
// menos tempo de socket. Sem framework, sem CSS externo, sem favicon.
//
// Envia com fetch+PUT (ver decisao 4) e manda o meta.json por ultimo (decisao
// 6), para que a pasta so pareca um programa valido quando o video ja estiver
// inteiro no cartao.
//
// O `<input type=file multiple>` e o laco sequencial ja atendem o caso de mandar
// 40 fotos de uma vez: e UM PUT POR ARQUIVO, um de cada vez (`await` dentro do
// laco), que e exatamente o que o servidor aceita — uma conexao por vez, porque
// o cartao e um recurso serial. Nao houve nada a acrescentar aqui para fotos
// alem da opcao no seletor.
// Quem trata um comando do controle remoto. A acao chega como texto curto
// ("ok", "acima", ...) e quem implementa traduz para NavAction -- este header
// nao conhece a maquina de telas. false = acao desconhecida, vira 400.
typedef bool (*CommandFn)(const char *acao, void *user);

// Pagina do controle remoto. Os botoes mandam fetch() e nao recarregam: o
// aparelho tem uma conexao de cada vez e um recarregamento inteiro por toque
// atrasaria o comando seguinte. Sem imagem, sem fonte externa, sem script de
// fora: a pagina sai inteira do flash e funciona sem internet, que e o caso de
// quem esta na mesma Wi-Fi do aparelho.
static const char CONTROL_PAGE[] =
    "<!doctype html><meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>M5 RETRO TV - controle</title><style>"
    "body{font:15px system-ui;margin:0;padding:1em;background:#101828;color:#e8edf7;"
    "display:flex;flex-direction:column;align-items:center;gap:1em}"
    "h2{font-size:1.1em;letter-spacing:.08em;margin:.2em 0 0;color:#96bcff}"
    "#p{display:grid;grid-template-columns:repeat(3,4.6em);gap:.6em}"
    "button{font:inherit;height:4.6em;border:0;border-radius:.6em;background:#24324c;"
    "color:#e8edf7;cursor:pointer;-webkit-tap-highlight-color:transparent}"
    "button:active{background:#96bcff;color:#101828}"
    "button.w{grid-column:span 3;height:3em}"
    "#s{font:13px monospace;color:#8fa3c4;min-height:1.2em}"
    "</style><h2>M5 RETRO TV</h2><div id=p>"
    "<button class=w onclick=c('acima')>ACIMA</button>"
    "<button onclick=c('voltar')>VOLTAR</button>"
    "<button onclick=c('ok')>OK</button>"
    "<button onclick=c('inicio')>INICIO</button>"
    "<button class=w onclick=c('abaixo')>ABAIXO</button>"
    "</div><div id=s></div><script>"
    "async function c(a){var s=document.getElementById('s');s.textContent=a;"
    "try{var r=await fetch('/cmd/'+a,{method:'PUT'});"
    "s.textContent=r.ok?a+' ok':a+' falhou ('+r.status+')'}"
    "catch(e){s.textContent='sem conexao'}}"
    "</script>";

static const char TRANSFER_PAGE[] =
    "<!doctype html><meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>M5 RETRO TV</title><style>body{font:15px system-ui;margin:2em auto;max-width:32em;padding:0 1em}"
    "input,select,button{font:inherit;padding:.4em}#l div{margin:.3em 0;font-family:monospace}</style>"
    "<h2>M5 RETRO TV - enviar para o cartao</h2>"
    "<p>Destino <select id=k><option>videos<option>music<option>fotos</select> "
    "pasta <input id=p size=14 placeholder=meu-filme></p>"
    "<p><input type=file id=f multiple></p><button onclick=go()>Enviar</button><div id=l></div>"
    "<script>async function go(){var a=[].slice.call(f.files);"
    "a.sort(function(x,y){return (x.name=='meta.json')-(y.name=='meta.json')});l.textContent='';"
    "for(var i=0;i<a.length;i++){var x=a[i],d=document.createElement('div');"
    "d.textContent=x.name+': enviando';l.appendChild(d);"
    "var u='/upload/'+k.value+'/'+(p.value?encodeURIComponent(p.value)+'/':'')+encodeURIComponent(x.name);"
    "try{var h=await fetch(u,{method:'HEAD'}),o=0;"
    "if(h.ok)o=parseInt(h.headers.get('X-Resume-Offset')||'0',10)||0;if(o>x.size)o=0;"
    "var g=o?{'Range':'bytes='+o+'-','X-Upload-Offset':''+o}:{};"
    "var r=await fetch(u,{method:'PUT',headers:g,body:o?x.slice(o):x});"
    "d.textContent=x.name+': '+(r.ok?'ok':'ERRO '+r.status+' '+await r.text());}"
    "catch(e){d.textContent=x.name+': falhou';}}}</script>";

class FileTransfer {
public:
  // ---- ciclo de vida -------------------------------------------------------

  // `sdMutex` vem de fora porque mora em main.cpp. Passar nullptr e aceito (o
  // acesso ao cartao fica sem protecao), mas nao e o que o firmware faz.
  bool begin(SemaphoreHandle_t sdMutex, uint16_t port = 80);
  void stop();

  // Nao bloqueante. Chamar do loop(); trabalha no maximo SLICE_MS por chamada.
  void handle();

  // ---- getters para a tela desenhar sem conhecer o interior ---------------
  bool active() const { return _active; }
  TransferStatus status() const { return _status; }
  uint16_t port() const { return _port; }
  // Guardado, e nao perguntado ao NINA a cada chamada: a tela chama isto a cada
  // repintura (4x/s recebendo), e cada pergunta seria uma transacao no SPI1
  // tomando o net::Lock do loop(). handle() refresca de tempos em tempos.
  String ip() const { return String(_ip); }
  const char *user() const { return "m5retro"; }
  const char *password() const { return _password; }
  const char *fileName() const { return _name; }   // nome curto do arquivo em curso
  uint64_t received() const { return _received; }
  uint64_t total() const { return _total; }
  uint16_t filesInstalled() const { return _installed; }
  const char *lastError() const { return _error; }
  // Diagnóstico: o socket de escuta realmente subiu? (lido do NINA no begin())
  bool listening() const { return _listening; }
  uint8_t percent() const {
    if (!_total)
      return 0;
    if (_received >= _total)
      return 100;
    return uint8_t((_received * 100) / _total);
  }

  static const size_t BLOCK = 8192;          // ver decisao 5
  static const uint32_t SLICE_MS = 12;       // fatia maxima por handle()
  static const uint32_t IDLE_MS = 20000;     // conexao parada e abandonada
  // Conexao aceita que nao mandou NENHUM byte. Navegadores abrem conexoes
  // especulativas e nao usam; como aqui ha uma conexao por vez, esperar os 20 s
  // do IDLE_MS por uma delas travaria o envio de verdade que esta na fila.
  static const uint32_t FIRST_BYTE_MS = 3000;
  // De quanto em quanto tempo perguntar ao NINA se o socket ainda esta de pe
  // quando nao chega nada. Cada pergunta e uma transacao no SPI1.
  static const uint32_t ALIVE_CHECK_MS = 250;
  // Escrita por transacao SPI. O buffer de comando do NINA e de ~4 KB (o maximo
  // do DMA do SPI escravo do C6); 2 KB deixam folga para o cabecalho do comando
  // e ainda mandam a pagina inteira em uma ou duas idas.
  static const size_t WRITE_CHUNK = 2048;
  static const uint32_t IP_REFRESH_MS = 10000; // DHCP pode trocar o endereco
  static const uint32_t SD_WAIT_MS = 1000;   // espera pelo sdMutex por bloco
  static const uint32_t MAX_HEADER = 2048;   // teto do bloco de cabecalhos
  static const size_t LINE_CAP = 512;        // linha de pedido / cabecalho
  static const size_t PASSWORD_LEN = 8;
  static const uint64_t RESERVE = 1024u * 1024u; // folga exigida no cartao

private:
  enum class Phase : uint8_t { OCIOSA, CABECALHO, CORPO };
  enum class Method : uint8_t { OUTRO, GET, HEAD, PUT };
  enum class Route : uint8_t { DESCONHECIDA, RAIZ, UPLOAD, CONTROLE, COMANDO };

  WiFiServer *_server = nullptr; // de detail::listener(); nao e nosso
  WiFiClient _client;
  File _file;
  SemaphoreHandle_t _sd = nullptr;
  // PSRAM, BLOCK bytes. Tres usos que nunca se sobrepoem: bytes crus do
  // cabecalho (_hdrPos.._hdrLen), acumulador do corpo ate um bloco inteiro
  // (_fill) e leitura de volta do cabecalho JPEG em completeBody().
  uint8_t *_buffer = nullptr;
  size_t _hdrPos = 0, _hdrLen = 0;
  size_t _fill = 0;
  char _ip[16] = {0};
  uint32_t _ipCheckMs = 0;
  uint32_t _aliveCheckMs = 0;
  uint32_t _acceptedMs = 0;
  bool _listening = false;

  bool _active = false;
  TransferStatus _status = TransferStatus::PARADO;
  Phase _phase = Phase::OCIOSA;
  const char *_error = "";
  uint16_t _port = 80;
  uint16_t _installed = 0;

  char _password[PASSWORD_LEN + 1];
  char _path[PATH_CAP];
  char _part[PATH_CAP];
  char _name[MAX_SEGMENT + 1];
  char _line[LINE_CAP];
  char _authUser[MAX_SECRET + 1];
  char _authPass[MAX_SECRET + 1];

  size_t _lineLen = 0;
  uint32_t _headerBytes = 0;
  bool _haveRequestLine = false, _hasAuth = false, _hasLength = false, _hasOffset = false;
  Method _method = Method::OUTRO;
  Route _route = Route::DESCONHECIDA;
  PathError _pathError = PathError::VAZIO;
  uint64_t _length = 0, _offset = 0, _received = 0, _total = 0;
  uint32_t _deadline = 0;

  // ---- cartao --------------------------------------------------------------
  bool sdTake(uint32_t ms) {
    if (!_sd)
      return true;
    return xSemaphoreTake(_sd, pdMS_TO_TICKS(ms)) == pdTRUE;
  }
  void sdGive() {
    if (_sd)
      xSemaphoreGive(_sd);
  }

  // ---- rede (toda chamada WiFiNINA com net::Lock, uma operacao por vez) ------
  //
  // Nada de WiFiClient::connected() aqui. No WiFiNINA ele poe o socket em 255
  // quando ve a conexao caida — e depois disso stop() nao faz nada. Para um
  // socket ACEITO pelo servidor o firmware NINA nao libera o slot sozinho
  // (so libera os abertos por connect), entao o slot vazaria a cada navegador
  // que fechasse primeiro, e em poucas conexoes o C6 pararia de aceitar. Por
  // isso a vida do socket e lida com status() e o fim e sempre stop().
  bool peerAlive();
  int netRead(uint8_t *dst, size_t cap);
  bool netWriteAll(const uint8_t *data, size_t len);
  void refreshIp();
  bool flushFill();

  // ---- conexao -------------------------------------------------------------
  void beginRequest();
  void closeConnection(bool discardPartial);
  bool expired() const { return int32_t(millis() - _deadline) >= 0; }
  void touch() { _deadline = millis() + IDLE_MS; }

  bool pumpHeader();
  bool pumpBody();
  void parseRequestLine();
  void parseHeaderLine();
  void finishHeaders();
  bool authorized() const;
  void startBody();
  void completeBody();
  void abortBody(const char *reason);

  void respond(int code, const char *reason, const char *type, const char *body, const char *extra = NULL);
  void respondPage(bool bodyToo);
  void respondControl(bool bodyToo);
  void respondResume();
  void makePassword();
  uint32_t partSize();

  // Quem trata o comando do controle remoto. Ponteiro de funcao de proposito:
  // este header nao conhece NavAction nem a maquina de telas, e nao deve
  // conhecer. Devolve false para comando desconhecido, o que vira 400.
  CommandFn _cmd = NULL;
  void *_cmdUser = NULL;
  char _acao[16] = {0};

public:
  // Liga o controle remoto. Sem isto as rotas /controle e /cmd respondem 404,
  // e o servidor se comporta exatamente como antes.
  void setCommandHandler(CommandFn fn, void *user) {
    _cmd = fn;
    _cmdUser = user;
  }
};

// ---------------------------------------------------------------------------
// Implementacao
// ---------------------------------------------------------------------------

inline bool FileTransfer::begin(SemaphoreHandle_t sdMutex, uint16_t port) {
  if (_active)
    return true;
  _sd = sdMutex;
  _port = port ? port : 80;
  _error = "";
  bool conectado;
  {
    net::Lock lock;
    conectado = WiFi.status() == WL_CONNECTED;
  }
  if (!conectado) {
    _error = "sem Wi-Fi";
    _status = TransferStatus::FALHA;
    return false;
  }
  // PSRAM, nunca SRAM: o framebuffer do DVI ja leva 153.600 B da interna e o
  // upstream ja estourou pilha duas vezes com buffer grande no lugar errado.
  // Sem PSRAM, falhar e mais honesto do que roubar SRAM.
  if (!_buffer)
    _buffer = (uint8_t *)ps_malloc(BLOCK);
  if (!_buffer) {
    _error = "sem PSRAM para o buffer";
    _status = TransferStatus::FALHA;
    return false;
  }
  {
    net::Lock lock;
    detail::Listener &l = detail::listener();
    if (!l.server || l.port != _port) {
      // Porta nova: o socket antigo fica orfao no C6 (nao ha como fecha-lo, ver
      // detail::Listener) e o objeto antigo tambem fica — sao poucos bytes, e
      // o WiFiServer nao tem destrutor virtual para um delete limpo. Na pratica
      // a porta e sempre 80 e isto roda uma vez por boot.
      l.server = new WiFiServer(_port);
      l.port = _port;
      l.server->begin();
    } else if (!l.server->status()) {
      // O C6 foi reiniciado (reset do GPIO 22) ou o servidor caiu: escuta de novo.
      l.server->begin();
    }
    _server = l.server;
    _listening = _server->status() != 0;
  }
  if (!_listening) {
    _error = "servidor HTTP nao subiu";
    _status = TransferStatus::FALHA;
    return false;
  }
  refreshIp();
  makePassword();
  _installed = 0;
  _received = _total = 0;
  _fill = _hdrPos = _hdrLen = 0;
  _name[0] = '\0';
  _phase = Phase::OCIOSA;
  _active = true;
  _status = TransferStatus::AGUARDANDO;
  return true;
}

inline void FileTransfer::stop() {
  // O `.part` sobrevive de proposito: sair da tela e voltar depois retoma o
  // arquivo de onde parou em vez de reenviar 45 MB. O que estava acumulado no
  // buffer vai para o cartao antes, para a retomada perder o minimo.
  if (_phase == Phase::CORPO && _fill)
    flushFill();
  closeConnection(false);
  // O servidor NAO e fechado (o NINA nao sabe fechar, ver detail::Listener):
  // so deixa de ser atendido. Conexoes que chegarem agora esperam na fila do C6
  // ate o navegador desistir.
  _server = nullptr;
  if (_buffer) {
    free(_buffer);
    _buffer = NULL;
  }
  _password[0] = '\0'; // a senha morre com a sessao
  _active = false;
  _status = TransferStatus::PARADO;
  _phase = Phase::OCIOSA;
}

inline void FileTransfer::makePassword() {
  // 32 simbolos sem 0/O/1/I/l: a senha vai ser lida da tela de um tubo e
  // digitada no computador. 8 caracteres = 40 bits, de sobra para uma sessao
  // que dura minutos numa LAN.
  static const char kAlphabet[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";
  for (size_t i = 0; i < PASSWORD_LEN; ++i)
    _password[i] = kAlphabet[esp_random() & 31u];
  _password[PASSWORD_LEN] = '\0';
}

inline void FileTransfer::refreshIp() {
  IPAddress addr;
  {
    net::Lock lock;
    addr = WiFi.localIP();
  }
  snprintf(_ip, sizeof(_ip), "%u.%u.%u.%u", (unsigned)addr[0], (unsigned)addr[1],
           (unsigned)addr[2], (unsigned)addr[3]);
  _ipCheckMs = millis();
}

inline bool FileTransfer::peerAlive() {
  net::Lock lock;
  if (!_client)
    return false;
  if (_client.available() > 0)
    return true; // ainda ha bytes a ler, mesmo que o outro lado ja tenha fechado
  return _client.status() == detail::NINA_TCP_ESTABLISHED;
}

inline int FileTransfer::netRead(uint8_t *dst, size_t cap) {
  net::Lock lock;
  if (!_client || !cap)
    return 0;
  // Traz no maximo o que o WiFiNINA ja tem no buffer dele (ate 1500 B por
  // transacao SPI); nao espera chegar mais.
  return _client.read(dst, cap);
}

// Escreve tudo ou devolve false. Uma transacao por pedaco de WRITE_CHUNK, com o
// lock so durante ela: a pagina de 2 KB nao segura o SPI1 de uma vez.
inline bool FileTransfer::netWriteAll(const uint8_t *data, size_t len) {
  while (len) {
    const size_t k = len > WRITE_CHUNK ? WRITE_CHUNK : len;
    size_t w;
    {
      net::Lock lock;
      if (!_client)
        return false;
      w = _client.write(data, k);
    }
    if (!w)
      return false; // o C6 recusou: conexao caiu ou buffer de envio cheio demais
    data += w;
    len -= w;
  }
  return true;
}

inline void FileTransfer::beginRequest() {
  _phase = Phase::CABECALHO;
  _lineLen = 0;
  _headerBytes = 0;
  _haveRequestLine = false;
  _hasAuth = _hasLength = _hasOffset = false;
  _method = Method::OUTRO;
  _route = Route::DESCONHECIDA;
  _pathError = PathError::VAZIO;
  _length = _offset = 0;
  _authUser[0] = _authPass[0] = '\0';
  _path[0] = _part[0] = '\0';
  _hdrPos = _hdrLen = 0;
  _fill = 0;
  _acceptedMs = _aliveCheckMs = millis();
  touch();
}

inline void FileTransfer::closeConnection(bool discardPartial) {
  if (_file) {
    // flush antes do close para que o tamanho do `.part` no cartao seja
    // exatamente `_received` — e o que faz a retomada bater.
    if (sdTake(SD_WAIT_MS)) {
      _file.flush();
      _file.close();
      if (discardPartial && _part[0])
        SD.remove(_part);
      sdGive();
    }
    _file = File();
  }
  {
    // stop() SEMPRE, mesmo com o outro lado ja fechado: e o que devolve o slot
    // do socket ao C6 (ver peerAlive). stop() do WiFiNINA espera o NINA
    // confirmar o fechamento, que no firmware atual e imediato.
    net::Lock lock;
    if (_client)
      _client.stop();
  }
  _client = WiFiClient();
  _fill = _hdrPos = _hdrLen = 0;
  _phase = Phase::OCIOSA;
  if (_status == TransferStatus::RECEBENDO)
    _status = TransferStatus::AGUARDANDO;
}

inline void FileTransfer::handle() {
  if (!_active)
    return;

  // Uma conexao por vez. O cartao e um recurso serial: atender dois envios
  // juntos so dividiria a mesma banda em dois e dobraria o estado a manter.
  if (_phase == Phase::OCIOSA) {
    if (uint32_t(millis() - _ipCheckMs) >= IP_REFRESH_MS)
      refreshIp();
    WiFiClient next;
    {
      net::Lock lock;
      next = _server ? _server->available() : WiFiClient();
    }
    if (!next)
      return;
    _client = next;
    beginRequest();
  }

  const uint32_t sliceStart = millis();
  for (;;) {
    if (millis() - sliceStart >= SLICE_MS)
      return; // devolve o loop(); o resto continua na proxima volta
    if (_phase == Phase::CABECALHO) {
      if (!pumpHeader())
        return;
    } else if (_phase == Phase::CORPO) {
      if (!pumpBody())
        return;
    } else {
      return; // ja respondeu e fechou
    }
  }
}

// Le o cabecalho em blocos para `_buffer` e interpreta byte a byte a partir
// dali. A sobra do ultimo bloco depois da linha vazia ja e CORPO: startBody()
// a aproveita como o comeco do acumulador, sem perder nem repetir byte.
inline bool FileTransfer::pumpHeader() {
  if (_hdrPos >= _hdrLen) {
    // Um bloco inteiro: o teto do cabecalho (MAX_HEADER) e conferido byte a
    // byte abaixo, e o que passar dele ja e corpo.
    const int n = netRead(_buffer, BLOCK);
    if (n <= 0) {
      const uint32_t now = millis();
      if (_headerBytes == 0 && now - _acceptedMs >= FIRST_BYTE_MS) {
        closeConnection(false); // conexao especulativa do navegador: libera a vez
        return false;
      }
      if (expired()) {
        closeConnection(false);
        return false;
      }
      if (now - _aliveCheckMs >= ALIVE_CHECK_MS) {
        _aliveCheckMs = now;
        if (!peerAlive())
          closeConnection(false);
      }
      return false;
    }
    _hdrPos = 0;
    _hdrLen = size_t(n);
  }
  while (_hdrPos < _hdrLen) {
    const char c = char(_buffer[_hdrPos++]);
    if (++_headerBytes > MAX_HEADER) {
      respond(431, "Request Header Fields Too Large", "text/plain", "cabecalho grande demais");
      return false;
    }
    if (c == '\r')
      continue;
    if (c != '\n') {
      if (_lineLen + 1 >= LINE_CAP) {
        respond(431, "Request Header Fields Too Large", "text/plain", "linha grande demais");
        return false;
      }
      _line[_lineLen++] = c;
      continue;
    }
    _line[_lineLen] = '\0';
    const size_t len = _lineLen;
    _lineLen = 0;
    if (len == 0) {
      finishHeaders();
      return _phase == Phase::CORPO;
    }
    if (!_haveRequestLine)
      parseRequestLine();
    else
      parseHeaderLine();
    if (_phase == Phase::OCIOSA)
      return false; // parseRequestLine ja respondeu e fechou
  }
  touch();
  return true;
}

inline void FileTransfer::parseRequestLine() {
  _haveRequestLine = true;
  char *space = strchr(_line, ' ');
  if (!space) {
    respond(400, "Bad Request", "text/plain", "pedido invalido");
    return;
  }
  *space = '\0';
  if (strcmp(_line, "GET") == 0)
    _method = Method::GET;
  else if (strcmp(_line, "HEAD") == 0)
    _method = Method::HEAD;
  else if (strcmp(_line, "PUT") == 0)
    _method = Method::PUT;
  else
    _method = Method::OUTRO;

  char *target = space + 1;
  char *second = strchr(target, ' ');
  if (second)
    *second = '\0';

  if (strcmp(target, "/") == 0 || strcmp(target, "/index.html") == 0) {
    _route = Route::RAIZ;
    return;
  }
  if (strcmp(target, "/controle") == 0) {
    _route = Route::CONTROLE;
    return;
  }
  if (strncmp(target, "/cmd/", 5) == 0) {
    _route = Route::COMANDO;
    sanitizeAction(target + 5, _acao, sizeof(_acao));
    return;
  }
  if (strncmp(target, "/upload/", 8) == 0) {
    _route = Route::UPLOAD;
    _pathError = sanitizePath(target + 8, _path, sizeof(_path));
    if (_pathError == PathError::OK) {
      partPath(_path, _part, sizeof(_part));
      baseName(_path, _name, sizeof(_name));
    }
    return;
  }
  _route = Route::DESCONHECIDA;
}

inline void FileTransfer::parseHeaderLine() {
  char *colon = strchr(_line, ':');
  if (!colon)
    return;
  *colon = '\0';
  char *value = colon + 1;
  while (isSpace(*value))
    ++value;
  const size_t nameLen = boundedLen(_line, LINE_CAP);

  if (nameLen == 13 && matchesToken(_line, "authorization", 13)) {
    _hasAuth = parseBasicAuth(value, _authUser, sizeof(_authUser), _authPass, sizeof(_authPass));
    return;
  }
  if (nameLen == 14 && matchesToken(_line, "content-length", 14)) {
    uint64_t n = 0;
    int digits = 0;
    for (const char *p = value; *p >= '0' && *p <= '9'; ++p) {
      n = n * 10 + uint64_t(*p - '0');
      if (++digits > 19 || n > MAX_FILE)
        return;
    }
    if (digits) {
      _length = n;
      _hasLength = true;
    }
    return;
  }
  if (nameLen == 5 && matchesToken(_line, "range", 5)) {
    uint64_t start = 0;
    if (parseRange(value, start)) {
      _offset = start;
      _hasOffset = true;
    }
    return;
  }
  // Rede de seguranca para o `Range` do fetch: ver decisao 7. Só e usado se o
  // `Range` nao chegou, para que o cabecalho padrao continue mandando.
  if (nameLen == 15 && matchesToken(_line, "x-upload-offset", 15) && !_hasOffset) {
    uint64_t n = 0;
    int digits = 0;
    for (const char *p = value; *p >= '0' && *p <= '9'; ++p) {
      n = n * 10 + uint64_t(*p - '0');
      if (++digits > 19 || n > MAX_FILE)
        return;
    }
    if (digits) {
      _offset = n;
      _hasOffset = true;
    }
  }
}

inline bool FileTransfer::authorized() const {
  if (!_hasAuth)
    return false;
  // Os dois lados sao comparados sempre, sem curto-circuito entre eles, para
  // nao vazar por tempo se foi o usuario ou a senha que errou.
  const bool userOk = secretEquals("m5retro", _authUser);
  const bool passOk = secretEquals(_password, _authPass);
  return userOk && passOk;
}

inline void FileTransfer::finishHeaders() {
  if (!authorized()) {
    respond(401, "Unauthorized", "text/plain", "autenticacao necessaria",
            "WWW-Authenticate: Basic realm=\"M5 RETRO TV\"\r\n");
    return;
  }
  if (_route == Route::RAIZ) {
    if (_method == Method::GET || _method == Method::HEAD)
      respondPage(_method == Method::GET);
    else
      respond(405, "Method Not Allowed", "text/plain", "use GET /");
    return;
  }
  if (_route == Route::CONTROLE) {
    if (!_cmd) {
      respond(404, "Not Found", "text/plain", "controle remoto desligado");
      return;
    }
    if (_method == Method::GET || _method == Method::HEAD)
      respondControl(_method == Method::GET);
    else
      respond(405, "Method Not Allowed", "text/plain", "use GET /controle");
    return;
  }
  if (_route == Route::COMANDO) {
    if (!_cmd) {
      respond(404, "Not Found", "text/plain", "controle remoto desligado");
      return;
    }
    // PUT, e nao GET: o comando muda o estado do aparelho. Um GET seria
    // disparado por qualquer pre-busca do navegador.
    if (_method != Method::PUT) {
      respond(405, "Method Not Allowed", "text/plain", "use PUT /cmd/<acao>");
      return;
    }
    if (!_acao[0] || !_cmd(_acao, _cmdUser))
      respond(400, "Bad Request", "text/plain", "acao desconhecida");
    else
      respond(200, "OK", "text/plain", "ok");
    return;
  }
  if (_route != Route::UPLOAD) {
    respond(404, "Not Found", "text/plain", "rota desconhecida");
    return;
  }
  if (_pathError != PathError::OK) {
    respond(403, "Forbidden", "text/plain", pathErrorText(_pathError));
    return;
  }
  if (_method == Method::HEAD) {
    respondResume();
    return;
  }
  if (_method != Method::PUT) {
    respond(405, "Method Not Allowed", "text/plain", "use PUT /upload/<caminho>");
    return;
  }
  startBody();
}

inline uint32_t FileTransfer::partSize() {
  uint32_t size = 0;
  if (!sdTake(SD_WAIT_MS))
    return 0;
  File f = SD.open(_part, FILE_READ);
  if (f) {
    size = uint32_t(f.size());
    f.close();
  }
  sdGive();
  return size;
}

inline void FileTransfer::respondResume() {
  char extra[48];
  snprintf(extra, sizeof(extra), "X-Resume-Offset: %lu\r\n", (unsigned long)partSize());
  respond(200, "OK", "text/plain", "", extra);
}

inline void FileTransfer::startBody() {
  if (!_hasLength) {
    // Sem Content-Length nao da para saber onde o arquivo acaba. Aceitar
    // `Transfer-Encoding: chunked` exigiria um decodificador de pedacos e mais
    // estado, para nada: a pagina embutida sempre sabe o tamanho.
    respond(411, "Length Required", "text/plain", "informe Content-Length");
    return;
  }
  const uint64_t start = _hasOffset ? _offset : 0;
  const uint64_t expected = start + _length;
  if (expected > MAX_FILE) {
    respond(413, "Payload Too Large", "text/plain", "o FAT32 nao guarda arquivo de 4 GiB");
    return;
  }
  // Foto acima do teto de decodificacao nunca sera exibida (MAX_PHOTO espelha o
  // MAX_JPEG de src/main.cpp). Recusar ANTES de abrir o `.part` evita gravar
  // megabytes que so seriam descartados — e o caso comum, porque uma foto de
  // celular tem de 500 KB a 5 MB.
  if (isPhotoPath(_path) && !fitsPhotoBudget(expected)) {
    respond(413, "Payload Too Large", "text/plain",
            "foto acima de 128 KB: reduza para 320x240");
    return;
  }

  if (!sdTake(SD_WAIT_MS)) {
    respond(503, "Service Unavailable", "text/plain", "cartao ocupado");
    return;
  }
  // O SD do arduino-pico nao tem totalBytes()/usedBytes(); o espaco vem do
  // SDFS.info(). Num cartao grande a primeira chamada varre a FAT inteira (o
  // SdFat guarda a contagem depois), entao pode segurar o sdMutex por um
  // instante — uma vez por arquivo, nao por bloco.
  FSInfo info;
  const bool infoOk = SDFS.info(info);
  sdGive();
  if (!infoOk) {
    respond(503, "Service Unavailable", "text/plain", "cartao nao respondeu");
    return;
  }
  const uint64_t freeBytes = info.totalBytes > info.usedBytes ? info.totalBytes - info.usedBytes : 0;
  if (!fitsOnCard(freeBytes, _length, RESERVE)) {
    respond(507, "Insufficient Storage", "text/plain", "sem espaco no cartao");
    return;
  }

  if (!sdTake(SD_WAIT_MS)) {
    respond(503, "Service Unavailable", "text/plain", "cartao ocupado");
    return;
  }
  char scratch[PATH_CAP];
  const bool dirs = ensureParents(SD, _path, scratch, sizeof(scratch));
  uint32_t existing = 0;
  if (dirs) {
    File probe = SD.open(_part, FILE_READ);
    if (probe) {
      existing = uint32_t(probe.size());
      probe.close();
    }
  }
  sdGive();
  if (!dirs) {
    respond(500, "Internal Server Error", "text/plain", "nao criou a pasta no cartao");
    return;
  }

  if (start && existing != start) {
    // O cliente pediu para retomar de um ponto que nao e o fim do `.part`.
    // Gravar ali deixaria um buraco no arquivo; melhor dizer onde ele esta.
    char extra[48];
    snprintf(extra, sizeof(extra), "X-Resume-Offset: %lu\r\n", (unsigned long)existing);
    respond(416, "Range Not Satisfiable", "text/plain", "retome do offset informado", extra);
    return;
  }

  if (!sdTake(SD_WAIT_MS)) {
    respond(503, "Service Unavailable", "text/plain", "cartao ocupado");
    return;
  }
  // "a" continua no fim (retomada); "w" trunca (envio do zero). Modo em TEXTO
  // de proposito: no arduino-pico FILE_WRITE e O_APPEND (PORTING.md 3.7), e um
  // envio "do zero" por cima de um `.part` velho viraria anexo — o arquivo
  // final sairia com o lixo antigo na frente.
  _file = SD.open(_part, start ? "a" : "w");
  sdGive();
  if (!_file) {
    respond(500, "Internal Server Error", "text/plain", "nao abriu o arquivo temporario");
    return;
  }

  _received = start;
  _total = expected;
  _status = TransferStatus::RECEBENDO;
  _phase = Phase::CORPO;
  // A sobra do ultimo bloco do cabecalho ja e o comeco do corpo: vira o comeco
  // do acumulador. Bytes alem do Content-Length sao de um pedido seguinte que
  // este servidor nao atende (Connection: close) e ficam de fora.
  size_t leftover = _hdrLen > _hdrPos ? _hdrLen - _hdrPos : 0;
  if (uint64_t(leftover) > _length)
    leftover = size_t(_length);
  if (leftover)
    memmove(_buffer, _buffer + _hdrPos, leftover);
  _fill = leftover;
  _hdrPos = _hdrLen = 0;
  touch();
}

// Grava o acumulador no cartao. Toma e devolve o mutex POR BLOCO — nunca
// durante o arquivo inteiro. Mesma disciplina do runVideoBenchmark: segurar
// durante 45 MB mataria de fome a tarefa de audio. Se o mutex nao vier, aborta:
// os bytes ja sairam do socket e grava-los fora de ordem corromperia o arquivo.
// O `.part` continua com exatamente `_received` bytes, entao a retomada bate.
inline bool FileTransfer::flushFill() {
  if (!_fill)
    return true;
  if (!sdTake(SD_WAIT_MS)) {
    abortBody("cartao ocupado tempo demais");
    return false;
  }
  const size_t written = _file.write(_buffer, _fill);
  sdGive();
  if (written != _fill) {
    abortBody("escrita no cartao falhou");
    return false;
  }
  _received += written;
  _fill = 0;
  return true;
}

// O socket entrega no maximo 1500 B por vez (o buffer do WiFiNINA), mas o
// cartao quer blocos de 8 KiB (decisao 5): um bloco de 16 setores vai numa
// escrita multibloco do SDIO, e 1500 B desalinhados virariam varias escritas
// de setor avulso, bem mais lentas. Entao os pedacos do socket se acumulam em
// `_buffer` e o cartao so e tocado com bloco cheio ou no fim do arquivo.
inline bool FileTransfer::pumpBody() {
  const uint64_t remaining = _total - _received; // o que ainda nao esta no cartao
  if (uint64_t(_fill) >= remaining) {
    if (!flushFill())
      return false;
    completeBody();
    return false;
  }
  if (_fill == BLOCK && !flushFill())
    return false;

  size_t want = BLOCK - _fill;
  if (uint64_t(want) > remaining - _fill)
    want = size_t(remaining - _fill);
  const int got = netRead(_buffer + _fill, want);
  if (got <= 0) {
    const uint32_t now = millis();
    bool dead = expired();
    if (!dead && now - _aliveCheckMs >= ALIVE_CHECK_MS) {
      _aliveCheckMs = now;
      dead = !peerAlive();
    }
    if (dead) {
      // Salva o que ja chegou antes de largar: a retomada perde o minimo.
      if (flushFill())
        abortBody("conexao parou no meio");
    }
    return false;
  }
  _fill += size_t(got);
  touch();
  if (_fill == BLOCK || uint64_t(_fill) == remaining) {
    if (!flushFill())
      return false;
    if (_received >= _total) {
      completeBody();
      return false;
    }
  }
  return true;
}

inline void FileTransfer::completeBody() {
  uint32_t onCard = 0;
  bool installed = false;
  const char *rejected = NULL;
  if (sdTake(SD_WAIT_MS)) {
    _file.flush();
    onCard = uint32_t(_file.size());
    _file.close();
    _file = File();
    // So renomeia se o cartao tem exatamente o que foi prometido. Renomear um
    // arquivo curto e o unico jeito de o player receber um video pela metade.
    if (uint64_t(onCard) == _total) {
      // Foto: confere o cabecalho ANTES do rename. A leitura e do `.part` ja
      // fechado, e nao do fluxo: um cabecalho JPEG pode ficar depois de um EXIF
      // de dezenas de KB e nao cabe num unico pedaco do socket, enquanto aqui o
      // arquivo inteiro esta no cartao e um `read` de BLOCK bytes ve tudo o que
      // interessa. Reusa `_buffer` (PSRAM, ja alocado) — nenhum buffer novo.
      if (isPhotoPath(_path)) {
        size_t head = 0;
        File probe = SD.open(_part, FILE_READ);
        if (probe) {
          const int n = probe.read(_buffer, BLOCK);
          if (n > 0)
            head = size_t(n);
          probe.close();
        }
        rejected = jpegRejectText(inspectJpegHeader(_buffer, head));
        // Some com o `.part` recusado: deixa-lo ali faria a proxima tentativa
        // "retomar" um arquivo que o aparelho ja disse que nao abre.
        if (rejected)
          SD.remove(_part);
      }
      if (!rejected)
        installed = installFile(SD, _part, _path);
    }
    sdGive();
  }
  _phase = Phase::OCIOSA;

  if (rejected) {
    _status = TransferStatus::FALHA;
    _error = rejected; // literal em flash: sobrevive a saida desta funcao
    respond(415, "Unsupported Media Type", "text/plain", rejected);
    return;
  }
  if (!installed) {
    _status = TransferStatus::FALHA;
    _error = uint64_t(onCard) == _total ? "rename falhou" : "arquivo incompleto no cartao";
    respond(500, "Internal Server Error", "text/plain", _error);
    return;
  }
  ++_installed;
  _status = TransferStatus::CONCLUIDO;
  _error = "";
  respond(201, "Created", "text/plain", "ok");
}

inline void FileTransfer::abortBody(const char *reason) {
  _error = reason;
  _status = TransferStatus::FALHA;
  // discardPartial = false: o `.part` fica no cartao com o que ja chegou, que e
  // o que permite retomar com Range em vez de reenviar tudo.
  closeConnection(false);
}

inline void FileTransfer::respond(int code, const char *reason, const char *type, const char *body,
                                  const char *extra) {
  const size_t len = body ? strlen(body) : 0;
  char head[224];
  const int n = snprintf(head, sizeof(head),
                         "HTTP/1.1 %d %s\r\nConnection: close\r\nContent-Type: %s\r\n"
                         "Content-Length: %u\r\n%s\r\n",
                         code, reason, type, (unsigned)len, extra ? extra : "");
  bool ok = n > 0 &&
            netWriteAll((const uint8_t *)head, size_t(n) < sizeof(head) ? size_t(n) : sizeof(head) - 1);
  if (ok && len)
    netWriteAll((const uint8_t *)body, len);
  closeConnection(false);
}

inline void FileTransfer::respondControl(bool bodyToo) {
  const size_t len = sizeof(CONTROL_PAGE) - 1;
  char head[160];
  const int n = snprintf(head, sizeof(head),
                         "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: text/html\r\n"
                         "Content-Length: %u\r\n\r\n",
                         (unsigned)len);
  const bool ok = n > 0 && size_t(n) < sizeof(head) && netWriteAll((const uint8_t *)head, size_t(n));
  if (ok && bodyToo)
    netWriteAll((const uint8_t *)CONTROL_PAGE, len);
  closeConnection(false);
}

inline void FileTransfer::respondPage(bool bodyToo) {
  const size_t len = sizeof(TRANSFER_PAGE) - 1;
  char head[160];
  const int n = snprintf(head, sizeof(head),
                         "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: text/html\r\n"
                         "Content-Length: %u\r\n\r\n",
                         (unsigned)len);
  const bool ok = n > 0 && size_t(n) < sizeof(head) && netWriteAll((const uint8_t *)head, size_t(n));
  if (ok && bodyToo)
    netWriteAll((const uint8_t *)TRANSFER_PAGE, len);
  closeConnection(false);
}

} // namespace xfer
#endif // FILETRANSFER_NO_NETWORK
