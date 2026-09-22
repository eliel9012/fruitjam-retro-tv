#pragma once
// ============================================================================
// FileTransfer.h — servidor HTTP embarcado para mandar video e musica do
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
// 1. HTTP puro, sem TLS. Decisao consciente. Um handshake mbedTLS custa 4 a 6 KB
//    de pilha (AGENTS 2.3) num aparelho que tem ~41 KB de heap interno livre, e
//    o AES por software do ESP32 derruba a vazao para perto da metade num link
//    que ja e o gargalo. O trafego fica dentro da LAN e a senha e descartavel,
//    valida so enquanto a tela estiver aberta. Quem precisar de sigilo de
//    verdade continua com o leitor de cartao.
//
// 2. Sem tarefa FreeRTOS. Uma pilha de 8 KB seria ~20% do heap interno livre
//    (medido: ~41 KB livres, minimo historico ~35 KB). Em vez disso `handle()`
//    e chamado do `loop()` e trabalha por fatias curtas (SLICE_MS), sem
//    `delay()` e sem laco bloqueante: o `loop()` continua girando.
//
// 3. Buffer de transferencia em PSRAM (`ps_malloc`), nunca na pilha e nunca na
//    SRAM interna. Este repositorio ja estourou pilha duas vezes exatamente
//    assim (AGENTS 2.3). Se a PSRAM faltar, `begin()` falha em vez de cair para
//    a SRAM — o framebuffer do CVBS precisa dos 153.600 bytes dele.
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
//    FatFs, e como a gravacao e sequencial dentro do cluster (tipicamente
//    32 KiB no FAT32) tambem nao ha desperdicio de cluster. Pelo outro lado:
//    8 KiB a ~1,5 MB/s sao ~5 ms segurando o `sdMutex`, folgado dentro do prazo
//    do I2S de audio; 32 KiB seriam ~20 ms e ja arriscariam um estouro de
//    buffer na tarefa RCA_PCM. Menor que 8 KiB multiplicaria as idas ao mutex
//    sem ganhar nada.
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
// Medido no objeto compilado para o alvo (xtensa-esp32-elf-nm), nao estimado:
//   objeto FileTransfer : 1.216 B de .bss (SRAM interna)
//   TRANSFER_PAGE       : 1.276 B de .rodata (flash, nao SRAM)
//   buffer de bloco     : 8 KiB de PSRAM, alocado em begin(), liberado em stop()
//   pilha               : nenhuma alocacao grande; tudo o que e grande e membro
// Ou seja: ~1,2 KB dos ~41 KB de heap interno livre, contra os ~8 KB que a
// pilha de uma tarefa FreeRTOS custaria.
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

// Prefixo obrigatorio. As duas unicas pastas que aceitam envio sao
// /M5RETRO/videos/ e /M5RETRO/music/ — o resto do cartao (config, secrets,
// cache) fica fora do alcance de quem tem a senha da sessao.
static const char CARD_ROOT[] = "/M5RETRO/";
static const char PART_SUFFIX[] = ".part";

enum class PathError : uint8_t {
  OK,
  VAZIO,      // nada depois de /upload/, ou barra dupla, ou barra no fim
  ABSOLUTO,   // comecava com '/'
  ESCAPA,     // segmento "." ou ".."
  CARACTERE,  // controle, nao-ASCII, escape % quebrado ou caractere proibido no FAT
  LONGO,      // estourou PATH_CAP ou MAX_SEGMENT
  RAIZ,       // primeiro segmento nao e "videos" nem "music"
  INCOMPLETO  // so "videos" ou "music", sem nome de arquivo
};

// Texto curto e ASCII para mandar ao navegador e mostrar no LCD.
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
    return "use videos/ ou music/";
  case PathError::INCOMPLETO:
    return "falta o nome do arquivo";
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

// Transforma o que veio depois de "/upload/" num caminho absoluto no cartao.
// Entrada NAO CONFIAVEL: tudo o que nao for explicitamente permitido e negado.
//
// Aceita: videos/<...>/<arquivo> e music/<...>/<arquivo>
// Nega  : caminho absoluto, "." e "..", barra dupla ou final, byte de controle,
//         qualquer coisa fora do ASCII imprimivel, caractere proibido no FAT,
//         ponto ou espaco no fim de um segmento (o FAT os descarta em silencio,
//         e "video.mjpeg." viraria "video.mjpeg" sem o chamador saber),
//         e qualquer primeiro segmento que nao seja videos ou music.
//
// `out` precisa ter PATH_CAP bytes; a folga do ".part" ja esta reservada.
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
      if (!videos && !music)
        return PathError::RAIZ;
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

inline char lowerAscii(char c) {
  return (c >= 'A' && c <= 'Z') ? char(c + 32) : c;
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
#include <WiFi.h>
#include <SD.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace xfer {

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
static const char TRANSFER_PAGE[] =
    "<!doctype html><meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>M5 RETRO TV</title><style>body{font:15px system-ui;margin:2em auto;max-width:32em;padding:0 1em}"
    "input,select,button{font:inherit;padding:.4em}#l div{margin:.3em 0;font-family:monospace}</style>"
    "<h2>M5 RETRO TV - enviar para o cartao</h2>"
    "<p>Destino <select id=k><option>videos<option>music</select> "
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
  String ip() const { return WiFi.localIP().toString(); }
  const char *user() const { return "m5retro"; }
  const char *password() const { return _password; }
  const char *fileName() const { return _name; }   // nome curto do arquivo em curso
  uint64_t received() const { return _received; }
  uint64_t total() const { return _total; }
  uint16_t filesInstalled() const { return _installed; }
  const char *lastError() const { return _error; }
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
  static const uint32_t SD_WAIT_MS = 1000;   // espera pelo sdMutex por bloco
  static const uint32_t MAX_HEADER = 2048;   // teto do bloco de cabecalhos
  static const size_t LINE_CAP = 512;        // linha de pedido / cabecalho
  static const size_t PASSWORD_LEN = 8;
  static const uint64_t RESERVE = 1024u * 1024u; // folga exigida no cartao

private:
  enum class Phase : uint8_t { OCIOSA, CABECALHO, CORPO };
  enum class Method : uint8_t { OUTRO, GET, HEAD, PUT };
  enum class Route : uint8_t { DESCONHECIDA, RAIZ, UPLOAD };

  WiFiServer _server{80};
  WiFiClient _client;
  File _file;
  SemaphoreHandle_t _sd = nullptr;
  uint8_t *_buffer = nullptr;

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

  // ---- conexao -------------------------------------------------------------
  bool connectionAlive() { return _client && (_client.connected() || _client.available() > 0); }
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
  void respondResume();
  void makePassword();
  uint32_t partSize();
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
  if (WiFi.status() != WL_CONNECTED) {
    _error = "sem Wi-Fi";
    _status = TransferStatus::FALHA;
    return false;
  }
  // PSRAM, nunca SRAM: os 153.600 bytes do framebuffer do CVBS precisam da
  // interna e este repositorio ja estourou pilha duas vezes com buffer grande
  // no lugar errado. Sem PSRAM, falhar e mais honesto do que roubar SRAM.
  if (!_buffer)
    _buffer = (uint8_t *)ps_malloc(BLOCK);
  if (!_buffer) {
    _error = "sem PSRAM para o buffer";
    _status = TransferStatus::FALHA;
    return false;
  }
  makePassword();
  _installed = 0;
  _received = _total = 0;
  _name[0] = '\0';
  _phase = Phase::OCIOSA;
  _server.begin(_port);
  _server.setNoDelay(true);
  _active = true;
  _status = TransferStatus::AGUARDANDO;
  return true;
}

inline void FileTransfer::stop() {
  // O `.part` sobrevive de proposito: sair da tela e voltar depois retoma o
  // arquivo de onde parou em vez de reenviar 45 MB.
  closeConnection(false);
  _server.end();
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
  if (_client)
    _client.stop();
  _client = WiFiClient();
  _phase = Phase::OCIOSA;
  if (_status == TransferStatus::RECEBENDO)
    _status = TransferStatus::AGUARDANDO;
}

inline void FileTransfer::handle() {
  if (!_active)
    return;

  // Uma conexao por vez. O cartao e um recurso serial: atender dois envios
  // juntos so dividiria a mesma banda em dois e dobraria o estado a manter.
  if (!connectionAlive()) {
    if (_phase != Phase::OCIOSA)
      closeConnection(false); // caiu no meio; o `.part` fica para retomada
    WiFiClient next = _server.available();
    if (!next)
      return;
    _client = next;
    _client.setNoDelay(true);
    beginRequest();
  }

  const uint32_t sliceStart = millis();
  while (connectionAlive()) {
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
  closeConnection(false);
}

inline bool FileTransfer::pumpHeader() {
  int avail = _client.available();
  if (avail <= 0) {
    if (expired())
      closeConnection(false);
    return false;
  }
  while (avail-- > 0) {
    const int c = _client.read();
    if (c < 0)
      break;
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
      _line[_lineLen++] = char(c);
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

  if (!sdTake(SD_WAIT_MS)) {
    respond(503, "Service Unavailable", "text/plain", "cartao ocupado");
    return;
  }
  const uint64_t freeBytes = SD.totalBytes() - SD.usedBytes();
  sdGive();
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
  // "a" continua no fim (retomada); "w" trunca (envio do zero).
  _file = SD.open(_part, start ? FILE_APPEND : FILE_WRITE);
  sdGive();
  if (!_file) {
    respond(500, "Internal Server Error", "text/plain", "nao abriu o arquivo temporario");
    return;
  }

  _received = start;
  _total = expected;
  _status = TransferStatus::RECEBENDO;
  _phase = Phase::CORPO;
  touch();
}

inline bool FileTransfer::pumpBody() {
  if (_received >= _total) {
    completeBody();
    return false;
  }
  const int avail = _client.available();
  if (avail <= 0) {
    if (expired())
      abortBody("conexao parou no meio");
    return false;
  }
  size_t want = size_t(avail) > BLOCK ? BLOCK : size_t(avail);
  const uint64_t remaining = _total - _received;
  if (uint64_t(want) > remaining)
    want = size_t(remaining);

  const int got = _client.read(_buffer, want);
  if (got <= 0) {
    if (expired())
      abortBody("conexao parou no meio");
    return false;
  }

  // Toma e devolve o mutex POR BLOCO — nunca durante o arquivo inteiro. Mesma
  // disciplina do runVideoBenchmark: segurar durante 45 MB mataria de fome a
  // tarefa de audio. Se o mutex nao vier, aborta: os bytes ja sairam do socket
  // e grava-los fora de ordem corromperia o arquivo. O `.part` continua com
  // exatamente `_received` bytes, entao a retomada ainda bate.
  if (!sdTake(SD_WAIT_MS)) {
    abortBody("cartao ocupado tempo demais");
    return false;
  }
  const size_t written = _file.write(_buffer, size_t(got));
  sdGive();
  if (written != size_t(got)) {
    abortBody("escrita no cartao falhou");
    return false;
  }

  _received += written;
  touch();
  if (_received >= _total) {
    completeBody();
    return false;
  }
  return true;
}

inline void FileTransfer::completeBody() {
  uint32_t onCard = 0;
  bool installed = false;
  if (sdTake(SD_WAIT_MS)) {
    _file.flush();
    onCard = uint32_t(_file.size());
    _file.close();
    _file = File();
    // So renomeia se o cartao tem exatamente o que foi prometido. Renomear um
    // arquivo curto e o unico jeito de o player receber um video pela metade.
    if (uint64_t(onCard) == _total)
      installed = installFile(SD, _part, _path);
    sdGive();
  }
  _phase = Phase::OCIOSA;

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
  if (_client) {
    const size_t len = body ? strlen(body) : 0;
    char head[224];
    const int n = snprintf(head, sizeof(head),
                           "HTTP/1.1 %d %s\r\nConnection: close\r\nContent-Type: %s\r\n"
                           "Content-Length: %u\r\n%s\r\n",
                           code, reason, type, (unsigned)len, extra ? extra : "");
    if (n > 0)
      _client.write((const uint8_t *)head, size_t(n) < sizeof(head) ? size_t(n) : sizeof(head) - 1);
    if (len)
      _client.write((const uint8_t *)body, len);
    _client.flush();
  }
  closeConnection(false);
}

inline void FileTransfer::respondPage(bool bodyToo) {
  if (_client) {
    const size_t len = sizeof(TRANSFER_PAGE) - 1;
    char head[160];
    const int n = snprintf(head, sizeof(head),
                           "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: text/html\r\n"
                           "Content-Length: %u\r\n\r\n",
                           (unsigned)len);
    if (n > 0)
      _client.write((const uint8_t *)head, size_t(n));
    if (bodyToo)
      _client.write((const uint8_t *)TRANSFER_PAGE, len);
    _client.flush();
  }
  closeConnection(false);
}

} // namespace xfer
#endif // FILETRANSFER_NO_NETWORK
