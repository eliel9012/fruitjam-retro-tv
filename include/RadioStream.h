#pragma once
// ============================================================================
// RadioStream — cliente de rádio pela internet (Icecast/Shoutcast) do M5 RETRO TV.
//
// Entrega PCM 22050 Hz estéreo pelo MESMO contrato do mp3ReadPcm() do
// src/main.cpp, para que a tarefa de áudio (RCA_PCM, core 0) não precise de um
// segundo caminho de saída: quem integra troca a fonte do bloco e o resto do
// audioTask — escala de volume, i2s_write, ritmo pelo relógio de amostras —
// continua igual.
//
// ---------------------------------------------------------------------------
//  Por que cada decisão deste arquivo é assim
// ---------------------------------------------------------------------------
//
// 1. WiFiClient, NUNCA WiFiClientSecure. A estação é HTTP puro. Um cliente TLS
//    custa dezenas de KB de SRAM (buffers de record de 16 KB + contexto do
//    mbedTLS) e a SRAM é o recurso escasso deste aparelho: o framebuffer do
//    CVBS já leva 153.600 B e o mínimo de heap livre medido aqui foi de 28 a
//    38 KB. Uma alocação de 5 KB de sprite já causou falta de memória dura.
//
// 2. O buffer circular mora na PSRAM. São ~3,8 MB livres lá contra dezenas de
//    KB na SRAM. Ele é o ÚNICO bloco grande deste módulo e é alocado com
//    heap_caps_malloc(MALLOC_CAP_SPIRAM); se a alocação falhar o módulo entra
//    em Conn::NoMemory e NÃO tenta a SRAM — cair de pé é melhor do que derrubar
//    o aparelho inteiro.
//
// 3. O de-interleave do icy-metaint acontece ANTES do buffer circular. O que
//    entra no anel é fluxo MP3 puro; o decodificador nunca vê um byte de
//    metadado. Se o bloco de metadado fosse para o anel, o libhelix perderia o
//    sync e o áudio viraria estalo a cada ~8 segundos.
//
// 4. Reconexão com backoff exponencial (1, 2, 4, 8, 16, 30 s). A armadilha 4 do
//    AGENTS.md — "retentativa sem backoff" — já mordeu este projeto: um portão
//    que só avançava no sucesso virava laço apertado criando tarefas HTTP
//    quando a rede caía.
//
// 5. A leitura da rede roda numa tarefa própria PINADA NO CORE 0
//    (xTaskCreatePinnedToCore(..., 8192, ..., 0, nullptr, 1)), como as tarefas
//    WEATHER_HTTP e RADAR_HTTPS. O core 1 é do loop() e do desenho; um
//    client.read() bloqueante lá dentro travaria o raster. Duas tarefas já
//    estouraram 8 KB de pilha neste repositório (armadilha 2), então aqui o
//    maior local da tarefa tem 512 bytes e todo o resto é estado da classe.
//
// 6. A lógica pura (parse de URL, parse de cabeçalho, de-interleave, parse do
//    StreamTitle, backoff) vive fora do #if defined(ARDUINO) e não depende de
//    Arduino, WiFi nem FreeRTOS. É isso que permite testá-la nativamente em
//    sim/probes/radio_stream.cpp com g++ -std=gnu++11.
//
// Compila em -std=gnu++11 (arduino-esp32 2.x): todo corpo de constexpr aqui é
// um único return, sem laço e sem variável local (armadilha 1 do AGENTS.md).
// ============================================================================

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <atomic>

#include "Ascii.h"   // normalização para ASCII: as fontes bitmap só têm ASCII
#include "UiLogic.h" // timeReached(): millis() dá a volta em ~49 dias

namespace radio {

// ---------------------------------------------------------------------------
//  Tabela de estações (tempo de compilação)
// ---------------------------------------------------------------------------
//
// Uma entrada hoje. A tabela existe desde já para que acrescentar estação seja
// acrescentar uma linha, e não reestruturar o módulo: STATION_COUNT vem do
// sizeof, então nada mais precisa ser tocado.

struct Station {
  const char *name; // rótulo curto, ASCII, para a tela quando não houver logo
  const char *url;  // http:// apenas — ver a nota 1 no cabeçalho
};

constexpr Station STATIONS[] = {
    {"DIARIO FM", "http://51-222-26-208.webnow.com.br/diario.mp3"},
};

constexpr int STATION_COUNT = (int)(sizeof(STATIONS) / sizeof(STATIONS[0]));

// ---------------------------------------------------------------------------
//  Estado da conexão
// ---------------------------------------------------------------------------
//
// O pré-buffer conta como CONECTANDO de propósito: para quem está na frente da
// TV, "NO AR" tem que significar "já está saindo som", não "o socket abriu".

enum class Conn : uint8_t {
  Idle,       // módulo parado
  Connecting, // resolvendo/conectando/lendo cabeçalho/enchendo o pré-buffer
  OnAir,      // áudio saindo
  NoSignal,   // caiu ou recusou; esperando o backoff
  NoMemory    // PSRAM ou decodificador indisponível — não adianta tentar de novo
};

// Rótulos ASCII sem acento (AGENTS.md 2.7): as fontes bitmap só têm 0x20..0x7E.
inline const char *connLabel(Conn c) {
  return c == Conn::OnAir        ? "NO AR"
         : c == Conn::Connecting ? "CONECTANDO"
         : c == Conn::NoSignal   ? "SEM SINAL"
         : c == Conn::NoMemory   ? "SEM MEMORIA"
                                 : "PARADO";
}

// ---------------------------------------------------------------------------
//  Dimensionamento (ver o relatório de orçamento de memória no README do módulo)
// ---------------------------------------------------------------------------

// 96 kbps = 12.000 bytes/s. 64 KiB = 5,46 s de áudio.
//
// Por que 64 KiB e não mais nem menos: a rede Wi-Fi entrega em rajada e o
// aparelho ainda perde alguns milissegundos por varredura de canal e por
// retransmissão TCP. Meio segundo de anel não sobrevive a isso; 5,5 s
// sobrevivem com folga a uma pausa de rede completa. Mais do que isso só
// aumentaria a latência entre trocar de estação e ouvir som — e a PSRAM,
// apesar de sobrar, é lenta, então não há motivo para exagerar.
constexpr size_t RING_BYTES = 65536;

// Espelha o mp3In[4096] do src/main.cpp: mesma janela de entrada do libhelix.
constexpr size_t MP3_IN_BYTES = 4096;

// Espelha o mp3FramePcm[2304] do src/main.cpp: um frame MPEG1 estéreo (1152x2).
constexpr size_t MP3_PCM_SHORTS = 2304;

// Tudo numa alocação só: um caminho de falha em vez de três.
constexpr size_t PSRAM_BYTES = RING_BYTES + MP3_IN_BYTES + MP3_PCM_SHORTS * 2;

// Só declara OnAir depois de 2 s de áudio no anel. Evita o pior sintoma de
// rádio na internet: começar a tocar e engasgar três segundos depois.
constexpr size_t PREBUFFER_BYTES = 24576;

// Saída fixa do aparelho: o I2S1 é aberto a 22050 Hz estéreo pelo
// initExternalAudio() do src/main.cpp. O resampler linear abaixo leva qualquer
// taxa de origem (44100 na prática, mas lida do primeiro frame) para cá.
constexpr int OUT_RATE = 22050;

// ---------------------------------------------------------------------------
//  Backoff
// ---------------------------------------------------------------------------

constexpr uint32_t BACKOFF_MIN_MS = 1000;
constexpr uint32_t BACKOFF_MAX_MS = 30000;

// 1 s, 2, 4, 8, 16, depois 30 s para sempre. Determinístico de propósito: dá
// para testar (sim/probes/radio_stream.cpp) e, com uma estação só, não há
// rebanho de clientes para dispersar com jitter.
//
// C++11: corpo de constexpr é um único return, sem laço e sem local.
constexpr uint32_t backoffDelayMs(uint32_t attempt) {
  return attempt >= 5 ? BACKOFF_MAX_MS : (BACKOFF_MIN_MS << attempt);
}

// ---------------------------------------------------------------------------
//  Parse da URL
// ---------------------------------------------------------------------------

struct Url {
  char host[80];
  char path[144];
  uint16_t port;
};

// Aceita "http://host[:porta][/caminho]". Recusa https:// de propósito: este
// módulo não abre TLS (ver nota 1). Caminho vazio vira "/".
inline bool parseUrl(const char *url, Url &out) {
  out.host[0] = 0;
  out.path[0] = 0;
  out.port = 80;
  if (!url)
    return false;
  const char *p = url;
  if (strncmp(p, "http://", 7) == 0)
    p += 7;
  else if (strncmp(p, "https://", 8) == 0)
    return false; // sem TLS aqui
  else if (strstr(url, "://"))
    return false; // esquema desconhecido
  // Authority vai até '/', '?' ou fim.
  size_t h = 0;
  for (; *p && *p != '/' && *p != '?'; ++p) {
    if (*p == ':') {
      ++p;
      uint32_t v = 0;
      for (; *p >= '0' && *p <= '9'; ++p)
        v = v * 10 + (uint32_t)(*p - '0');
      if (!v || v > 65535)
        return false;
      out.port = (uint16_t)v;
      break;
    }
    if (h + 1 < sizeof(out.host))
      out.host[h++] = *p;
  }
  out.host[h] = 0;
  if (!h)
    return false;
  // Resto é o caminho (com query, se houver).
  while (*p && *p != '/' && *p != '?')
    ++p;
  size_t j = 0;
  if (*p != '/' && j + 1 < sizeof(out.path))
    out.path[j++] = '/';
  for (; *p && j + 1 < sizeof(out.path); ++p)
    out.path[j++] = *p;
  out.path[j] = 0;
  if (!j) {
    out.path[0] = '/';
    out.path[1] = 0;
  }
  return true;
}

// ---------------------------------------------------------------------------
//  Parse do cabeçalho de resposta
// ---------------------------------------------------------------------------

struct IcyInfo {
  int32_t status;  // código HTTP da linha de status; 0 se não lido
  int32_t metaint; // -1 quando o servidor não manda icy-metaint
  char name[40];   // icy-name  (ex.: "DIARIO FM")
  char desc[80];   // icy-description, quando houver
  bool audio;      // Content-Type é audio/mpeg (ou mp3)
};

inline void icyInfoInit(IcyInfo &i) {
  i.status = 0;
  i.metaint = -1;
  i.name[0] = 0;
  i.desc[0] = 0;
  i.audio = false;
}

namespace detail {

inline bool iEqPrefix(const char *s, const char *prefix) {
  for (size_t i = 0; prefix[i]; ++i) {
    const char a = s[i] >= 'A' && s[i] <= 'Z' ? (char)(s[i] + 32) : s[i];
    const char b = prefix[i] >= 'A' && prefix[i] <= 'Z' ? (char)(prefix[i] + 32) : prefix[i];
    if (!s[i] || a != b)
      return false;
  }
  return true;
}

inline const char *skipSpace(const char *s) {
  while (*s == ' ' || *s == '\t')
    ++s;
  return s;
}

// Copia já normalizado para ASCII: icy-name e icy-description vêm de fora e o
// AGENTS.md 2.7 exige normalizar tudo que venha de fora antes de desenhar.
inline void copyAscii(char *dst, size_t cap, const char *src) {
  ascii::normalize(dst, cap, src);
}

} // namespace detail

// Interpreta UMA linha de cabeçalho já sem o CRLF. Chamada pelo HeaderParser,
// exposta porque é o que o probe exercita diretamente.
inline void parseHeaderLine(const char *line, IcyInfo &out) {
  if (!line || !*line)
    return;
  if (detail::iEqPrefix(line, "HTTP/") || detail::iEqPrefix(line, "ICY ")) {
    // "HTTP/1.1 200 OK" ou "ICY 200 OK" (Shoutcast antigo).
    const char *p = line;
    while (*p && *p != ' ')
      ++p;
    p = detail::skipSpace(p);
    int32_t v = 0;
    while (*p >= '0' && *p <= '9')
      v = v * 10 + (*p++ - '0');
    out.status = v;
    return;
  }
  const char *colon = strchr(line, ':');
  if (!colon)
    return;
  const char *value = detail::skipSpace(colon + 1);
  if (detail::iEqPrefix(line, "icy-metaint:")) {
    int32_t v = 0;
    for (const char *p = value; *p >= '0' && *p <= '9'; ++p)
      v = v * 10 + (*p - '0');
    // metaint zero ou absurdo é tratado como "sem metadado": melhor perder o
    // título do que picotar o fluxo MP3 num intervalo errado.
    out.metaint = (v > 0 && v <= 1024 * 1024) ? v : -1;
  } else if (detail::iEqPrefix(line, "icy-name:")) {
    detail::copyAscii(out.name, sizeof(out.name), value);
  } else if (detail::iEqPrefix(line, "icy-description:")) {
    detail::copyAscii(out.desc, sizeof(out.desc), value);
  } else if (detail::iEqPrefix(line, "content-type:")) {
    out.audio = detail::iEqPrefix(value, "audio/mpeg") || detail::iEqPrefix(value, "audio/mp3") ||
                detail::iEqPrefix(value, "application/octet-stream");
  }
}

// Consome bytes do socket até o fim do cabeçalho (CRLF CRLF ou LF LF).
//
// Trabalha byte a byte porque o cabeçalho e o áudio chegam no MESMO read(): a
// função precisa devolver exatamente quantos bytes eram do cabeçalho para que
// o primeiro byte de áudio não seja perdido nem contado errado no metaint.
class HeaderParser {
public:
  void reset() {
    len_ = 0;
    done_ = false;
    ok_ = false;
    icyInfoInit(info_);
  }

  // Devolve quantos bytes de `data` pertenciam ao cabeçalho. Se done(), o resto
  // (n - consumidos) já é o corpo e deve ir para o de-interleaver.
  size_t feed(const uint8_t *data, size_t n) {
    size_t i = 0;
    for (; i < n && !done_; ++i) {
      const char c = (char)data[i];
      if (c == '\r')
        continue; // CR é ignorado: trata CRLF e LF do mesmo jeito
      if (c != '\n') {
        if (len_ + 1 < sizeof(line_))
          line_[len_++] = c;
        continue;
      }
      line_[len_] = 0;
      if (!len_) {
        // Linha vazia: fim do cabeçalho.
        done_ = true;
        ok_ = (info_.status == 200) && info_.audio;
        ++i;
        break;
      }
      parseHeaderLine(line_, info_);
      len_ = 0;
    }
    return i;
  }

  bool done() const { return done_; }
  bool ok() const { return ok_; }
  const IcyInfo &info() const { return info_; }

private:
  // 200 bytes: cabe icy-url, icy-genre e afins. Uma linha mais longa é truncada
  // — o que se perde é o fim do valor, nunca o sincronismo do fluxo, porque o
  // corte é só na cópia e o byte continua sendo consumido.
  char line_[200];
  uint16_t len_ = 0;
  bool done_ = false;
  bool ok_ = false;
  IcyInfo info_;
};

// ---------------------------------------------------------------------------
//  StreamTitle
// ---------------------------------------------------------------------------

// Extrai o valor de StreamTitle='...'; de um bloco de metadados ICY.
//
// A regra do Icecast/Shoutcast é que o valor termina na sequência "';" — aspa
// simples SEGUIDA de ponto-e-vírgula. Uma aspa solta no meio é conteúdo, e é
// comum ("ROCK N' ROLL"). Cortar na primeira aspa truncaria esses títulos.
// Servidores que omitem o ';' final também existem: aí vale até o fim.
//
// Escreve em `dst` já normalizado para ASCII. Devolve true se achou o campo.
inline bool parseStreamTitle(const char *meta, char *dst, size_t cap) {
  if (!dst || !cap)
    return false;
  dst[0] = 0;
  if (!meta)
    return false;
  const char *k = strstr(meta, "StreamTitle='");
  if (!k)
    return false;
  const char *v = k + 13; // strlen("StreamTitle='")
  const char *e = strstr(v, "';");
  size_t n;
  if (e) {
    n = (size_t)(e - v);
  } else {
    // Sem o ';' final o valor vale ate o fim -- mas a aspa que FECHA o campo
    // ainda esta la e nao faz parte do titulo. Sem esta poda, um servidor que
    // manda StreamTitle='FULANO' sem ponto e virgula deixava a aspa sobrando
    // na tela. So a ultima e podada: uma aspa no meio e conteudo ("ROCK N'
    // ROLL") e continua intacta.
    n = strlen(v);
    if (n && v[n - 1] == '\'')
      --n;
  }
  // Buffer temporário pequeno e limitado: o título vive num campo de tela, não
  // faz sentido guardar mais do que cabe nele.
  char raw[160];
  if (n >= sizeof(raw))
    n = sizeof(raw) - 1;
  memcpy(raw, v, n);
  raw[n] = 0;
  ascii::normalize(dst, cap, raw);
  return true;
}

// ---------------------------------------------------------------------------
//  De-interleave do icy-metaint
// ---------------------------------------------------------------------------

// O servidor intercala, a cada `metaint` bytes de áudio, um bloco de metadado:
// um byte de comprimento (em unidades de 16 bytes) seguido de len*16 bytes de
// texto. Comprimento zero — o caso mais comum — significa "nada mudou".
//
// Esta máquina de estados devolve SÓ os bytes de áudio, contíguos, e não se
// importa com onde a leitura do socket cortou: um bloco de metadado partido
// entre duas leituras é retomado no ponto exato.
//
// `out` PODE ser o mesmo ponteiro que `in`: os bytes de áudio só andam para a
// esquerda, então a cópia in-place é segura e é o que a tarefa de rede usa para
// gastar 256 bytes de pilha em vez de 512.
class MetaSplitter {
public:
  void begin(int32_t metaint) {
    metaint_ = metaint;
    audioLeft_ = metaint > 0 ? (uint32_t)metaint : 0;
    stage_ = metaint > 0 ? Stage::Audio : Stage::Passthrough;
    metaLeft_ = 0;
    bufLen_ = 0;
    blocks_ = 0;
    titleNew_ = false;
    title_[0] = 0;
  }

  size_t split(const uint8_t *in, size_t n, uint8_t *out) {
    size_t w = 0, i = 0;
    if (stage_ == Stage::Passthrough) {
      if (out != in)
        memmove(out, in, n);
      return n;
    }
    while (i < n) {
      if (stage_ == Stage::Audio) {
        size_t k = n - i;
        if (k > audioLeft_)
          k = audioLeft_;
        if (k) {
          memmove(out + w, in + i, k);
          w += k;
          i += k;
          audioLeft_ -= (uint32_t)k;
        }
        if (!audioLeft_)
          stage_ = Stage::Length;
        continue;
      }
      if (stage_ == Stage::Length) {
        metaLeft_ = (uint32_t)in[i++] * 16u;
        bufLen_ = 0;
        ++blocks_;
        if (!metaLeft_)
          rearm();
        else
          stage_ = Stage::Meta;
        continue;
      }
      // Stage::Meta
      size_t k = n - i;
      if (k > metaLeft_)
        k = metaLeft_;
      for (size_t j = 0; j < k; ++j) {
        // Guarda só o começo do bloco. Um bloco ICY pode ter até 4080 bytes,
        // mas o StreamTitle vem primeiro e um título maior que isto não cabe na
        // tela de qualquer jeito. O RESTO CONTINUA SENDO CONSUMIDO — é isso que
        // mantém o de-interleave correto sem gastar 4 KB de SRAM.
        if (bufLen_ + 1 < sizeof(buf_))
          buf_[bufLen_++] = (char)in[i + j];
      }
      i += k;
      metaLeft_ -= (uint32_t)k;
      if (!metaLeft_) {
        buf_[bufLen_] = 0;
        char t[128];
        if (parseStreamTitle(buf_, t, sizeof(t))) {
          if (strcmp(t, title_) != 0) {
            memcpy(title_, t, sizeof(t));
            titleNew_ = true;
          }
        }
        rearm();
      }
    }
    return w;
  }

  const char *title() const { return title_; }
  bool takeTitleFlag() {
    const bool v = titleNew_;
    titleNew_ = false;
    return v;
  }
  uint32_t blocks() const { return blocks_; }
  int32_t metaint() const { return metaint_; }

private:
  enum class Stage : uint8_t { Passthrough, Audio, Length, Meta };

  void rearm() {
    audioLeft_ = (uint32_t)metaint_;
    stage_ = Stage::Audio;
  }

  int32_t metaint_ = -1;
  uint32_t audioLeft_ = 0;
  uint32_t metaLeft_ = 0;
  uint32_t blocks_ = 0;
  Stage stage_ = Stage::Passthrough;
  bool titleNew_ = false;
  uint16_t bufLen_ = 0;
  char buf_[224];  // janela de captura do bloco de metadado
  char title_[128]; // já normalizado para ASCII
};

// ---------------------------------------------------------------------------
//  Anel produtor-consumidor (um escritor, um leitor)
// ---------------------------------------------------------------------------
//
// A tarefa de rede (core 0) escreve; a tarefa de áudio (core 0) lê. Um índice
// por lado, cada um escrito por um único dono: não precisa de mutex, e não usar
// mutex aqui é de propósito — o áudio não pode nunca ficar esperando a rede.
class Ring {
public:
  void attach(uint8_t *mem, size_t size) {
    buf_ = mem;
    size_ = size;
    clear();
  }
  void clear() {
    head_.store(0, std::memory_order_relaxed);
    tail_.store(0, std::memory_order_relaxed);
  }
  size_t capacity() const { return size_; }
  size_t used() const {
    const size_t h = head_.load(std::memory_order_acquire);
    const size_t t = tail_.load(std::memory_order_acquire);
    return h >= t ? h - t : size_ - (t - h);
  }
  // Uma posição fica sempre vaga para distinguir cheio de vazio.
  size_t space() const { return size_ ? size_ - used() - 1 : 0; }

  size_t write(const uint8_t *src, size_t n) {
    if (!buf_)
      return 0;
    size_t free = space();
    if (n > free)
      n = free;
    size_t h = head_.load(std::memory_order_relaxed);
    const size_t first = (h + n <= size_) ? n : size_ - h;
    memcpy(buf_ + h, src, first);
    if (n > first)
      memcpy(buf_, src + first, n - first);
    h += n;
    if (h >= size_)
      h -= size_;
    head_.store(h, std::memory_order_release);
    return n;
  }

  size_t read(uint8_t *dst, size_t n) {
    if (!buf_)
      return 0;
    size_t avail = used();
    if (n > avail)
      n = avail;
    size_t t = tail_.load(std::memory_order_relaxed);
    const size_t first = (t + n <= size_) ? n : size_ - t;
    memcpy(dst, buf_ + t, first);
    if (n > first)
      memcpy(dst + first, buf_, n - first);
    t += n;
    if (t >= size_)
      t -= size_;
    tail_.store(t, std::memory_order_release);
    return n;
  }

private:
  uint8_t *buf_ = nullptr;
  size_t size_ = 0;
  std::atomic<size_t> head_;
  std::atomic<size_t> tail_;
};

} // namespace radio

// ===========================================================================
//  Daqui para baixo: só no aparelho (Arduino + WiFi + FreeRTOS + libhelix).
//  O simulador e os probes compilam apenas a lógica pura acima.
// ===========================================================================
#if defined(ARDUINO)

#include <Arduino.h>
#include <WiFi.h> // WiFiClient — NUNCA WiFiClientSecure aqui (ver nota 1)
#include <esp_heap_caps.h>

#include "libhelix-mp3/mp3dec.h"

namespace radio {

// Timeouts. Todos comparados com timeReached() (aritmética sem sinal): o
// millis() é de 32 bits e dá a volta em ~49 dias (AGENTS.md 3.3).
constexpr uint32_t CONNECT_TIMEOUT_MS = 8000;
constexpr uint32_t HEADER_TIMEOUT_MS = 6000;
constexpr uint32_t STALL_TIMEOUT_MS = 6000; // socket aberto mas mudo: derruba

class Stream {
public:
  // -- ciclo de vida (chamado pelo loop(), core 1) --------------------------

  // Aloca a PSRAM, inicializa o libhelix e cria a tarefa de rede no core 0.
  // Não bloqueia esperando a conexão: a tela mostra CONECTANDO enquanto isso.
  bool begin(int stationIndex) {
    if (running_)
      return true;
    if (stationIndex < 0 || stationIndex >= STATION_COUNT)
      return false;
    station_ = stationIndex;
    if (!parseUrl(STATIONS[station_].url, url_)) {
      conn_.store((uint8_t)Conn::NoMemory);
      return false;
    }
    // Um bloco só de PSRAM para os três buffers: um caminho de falha.
    psram_ = (uint8_t *)heap_caps_malloc(PSRAM_BYTES, MALLOC_CAP_SPIRAM);
    if (!psram_) {
      conn_.store((uint8_t)Conn::NoMemory);
      Serial.println("[M5RETRO] Radio: sem PSRAM para o buffer");
      return false;
    }
    ring_.attach(psram_, RING_BYTES);
    mp3In_ = psram_ + RING_BYTES;
    mp3Pcm_ = (int16_t *)(psram_ + RING_BYTES + MP3_IN_BYTES);
    // O allocador do libhelix (AllocatorExt) já tenta ps_malloc primeiro, então
    // os ~25 KB de estado do decodificador também caem na PSRAM.
    dec_ = MP3InitDecoder();
    if (!dec_) {
      heap_caps_free(psram_);
      psram_ = nullptr;
      conn_.store((uint8_t)Conn::NoMemory);
      Serial.println("[M5RETRO] Radio: sem memoria para o decodificador MP3");
      return false;
    }
    resetDecoder();
    ascii::normalize(name_, sizeof(name_), STATIONS[station_].name);
    title_[0] = 0;
    conn_.store((uint8_t)Conn::Connecting);
    stop_.store(false);
    netAlive_.store(true);
    running_ = true;
    if (xTaskCreatePinnedToCore(netTaskEntry, "RADIO_ICY", 8192, this, 0, nullptr, 1) != pdPASS) {
      netAlive_.store(false);
      running_ = false;
      MP3FreeDecoder(dec_);
      dec_ = nullptr;
      heap_caps_free(psram_);
      psram_ = nullptr;
      conn_.store((uint8_t)Conn::NoMemory);
      return false;
    }
    return true;
  }

  // Encerra tudo. O chamador DEVE ter parado de chamar readPcm() antes (ou
  // seja: desligar o modo rádio do audioTask primeiro).
  //
  // Todas as esperas aqui têm prazo. A armadilha 3 do AGENTS.md foi exatamente
  // uma espera sem prazo que congelou o aparelho para sempre.
  void end() {
    if (!running_)
      return;
    stop_.store(true);
    const uint32_t limit = millis() + 3000;
    while (netAlive_.load() && !timeReached(millis(), limit))
      delay(10);
    const uint32_t limit2 = millis() + 500;
    while (pcmBusy_.load() && !timeReached(millis(), limit2))
      delay(5);
    running_ = false;
    if (dec_) {
      MP3FreeDecoder(dec_);
      dec_ = nullptr;
    }
    if (psram_) {
      heap_caps_free(psram_);
      psram_ = nullptr;
    }
    mp3In_ = nullptr;
    mp3Pcm_ = nullptr;
    ring_.attach(nullptr, 0);
    conn_.store((uint8_t)Conn::Idle);
  }

  // -- consumo de áudio (chamado pela tarefa RCA_PCM, core 0) ---------------

  // MESMO contrato do mp3ReadPcm() do src/main.cpp: escreve `samples` shorts
  // (pares L/R) a 22050 Hz e devolve quantos escreveu.
  //
  // Diferença de propósito em relação ao arquivo: quando o anel seca, isto
  // devolve SILÊNCIO em vez de 0. Devolver 0 faria o audioTask marcar
  // playbackFinished e derrubar a reprodução — e num rádio um engasgo de rede
  // não é fim de faixa.
  size_t readPcm(int16_t *dst, size_t samples) {
    pcmBusy_.store(true);
    size_t produced = 0;
    if (running_ && dec_ && conn_.load() == (uint8_t)Conn::OnAir)
      produced = decodeInto(dst, samples);
    // Zera a cauda so por higiene (o chamador nao vai escrever ali), e devolve
    // o que REALMENTE foi produzido -- nao `samples`.
    //
    // Devolver o total pedido fazia o chamador pacear um bloco inteiro mesmo
    // quando o decodificador tinha entregue um pedaco, e a diferenca ia para o
    // I2S como SILENCIO no meio do som. Era isso que se ouvia como arrasto. Um
    // retorno honesto deixa o chamador escrever menos e esperar menos, e faz o
    // ramo `if (!amostras)` dele finalmente existir.
    for (size_t i = produced; i < samples; ++i)
      dst[i] = 0;
    pcmBusy_.store(false);
    return produced;
  }

  // -- leitura de estado (qualquer core) ------------------------------------

  Conn state() const { return (Conn)conn_.load(); }
  const char *stateLabel() const { return connLabel(state()); }
  bool active() const { return running_; }
  int station() const { return station_; }

  // Nome da estação: o icy-name quando o servidor mandou, senão o da tabela.
  const char *stationName() const { return icyName_[0] ? icyName_ : name_; }

  // Título corrente (StreamTitle), já ASCII. String vazia quando não há.
  // Copiado sob uma trava curta porque a tarefa de rede pode reescrevê-lo.
  void copyTitle(char *dst, size_t cap) const {
    if (!dst || !cap)
      return;
    titleLock_.store(true);
    strncpy(dst, title_, cap - 1);
    dst[cap - 1] = 0;
    titleLock_.store(false);
  }

  uint32_t reconnects() const { return reconnects_.load(); }
  uint32_t bytesReceived() const { return bytesIn_.load(); }
  // 0..100: quanto do anel está cheio. Útil no `diag status`.
  uint8_t bufferPercent() const {
    return ring_.capacity() ? (uint8_t)((uint64_t)ring_.used() * 100 / ring_.capacity()) : 0;
  }
  // Taxa real lida do primeiro frame decodificado (não presumida).
  int sourceRate() const { return mp3Rate_; }
  int sourceChannels() const { return mp3Chans_; }
  int bitrateKbps() const { return bitrate_; }

private:
  // -- decodificação (espelha mp3ReadPcm do src/main.cpp) -------------------

  void resetDecoder() {
    mp3InLen_ = 0;
    mp3PcmCount_ = 0;
    phaseQ16_ = 0;
    mp3Rate_ = 44100;
    mp3Chans_ = 2;
  }

  size_t decodeInto(int16_t *dst, size_t samples) {
    size_t produced = 0;
    while (produced + 1 < samples) {
      if (mp3PcmCount_ == 0) {
        if (mp3InLen_ < 2048) {
          const size_t got = ring_.read(mp3In_ + mp3InLen_, MP3_IN_BYTES - mp3InLen_);
          if (!got && mp3InLen_ == 0)
            break; // anel seco: o chamador completa com silêncio
          mp3InLen_ += got;
        }
        unsigned char *in = mp3In_;
        int bytesLeft = (int)mp3InLen_;
        // MP3Decode devolve código de erro; a CONTAGEM de amostras vem de
        // MP3GetLastFrameInfo().outputSamps — igual ao src/main.cpp.
        const int err = MP3Decode(dec_, &in, &bytesLeft, mp3Pcm_, 0);
        const size_t consumed = (size_t)(in - mp3In_);
        if (consumed > 0) {
          memmove(mp3In_, in, mp3InLen_ - consumed);
          mp3InLen_ -= consumed;
        }
        if (err == 0) {
          MP3FrameInfo info;
          MP3GetLastFrameInfo(dec_, &info);
          mp3PcmCount_ = (size_t)info.outputSamps;
          phaseQ16_ = 0;
          mp3Rate_ = info.samprate; // taxa REAL do fluxo, não presumida
          mp3Chans_ = info.nChans;
          // O passo so muda quando o cabecalho do frame muda, entao sai do laco
          // interno -- era uma divisao dupla POR AMOSTRA. 44100 -> 22050 da 2.0.
          stepQ16_ = (uint32_t)(((uint64_t)mp3Rate_ << 16) / (uint32_t)OUT_RATE);
          if (info.bitrate > 0)
            bitrate_ = info.bitrate / 1000;
        } else if (consumed == 0) {
          // Não avançou: frame corrompido ou desincronizado. Descarta 1 byte e
          // tenta ressincronizar, sem laço apertado.
          if (mp3InLen_ > 0) {
            memmove(mp3In_, mp3In_ + 1, mp3InLen_ - 1);
            --mp3InLen_;
          } else {
            break;
          }
          continue;
        } else {
          continue; // consumiu mas falhou: tenta o próximo frame
        }
      }
      const int ch = mp3Chans_;
      const int framesIn = (int)(mp3PcmCount_ / ch);
      const int idx = (int)(phaseQ16_ >> 16);
      if (idx >= framesIn - 1) {
        mp3PcmCount_ = 0;
        continue;
      }
      const int32_t frac = (int32_t)(phaseQ16_ & 0xFFFFu);
      const int16_t *p0 = mp3Pcm_ + (size_t)idx * ch;
      const int16_t *p1 = p0 + ch;
      // MONO: mistura os dois canais ANTES de interpolar e escreve o mesmo
      // valor nas duas saidas -- uma interpolacao em vez de duas. Misturar
      // antes ou depois da o mesmo resultado, porque a interpolacao e linear.
      int32_t a, b;
      if (ch == 1) {
        a = p0[0];
        b = p1[0];
      } else {
        a = ((int32_t)p0[0] + (int32_t)p0[1]) / 2;
        b = ((int32_t)p1[0] + (int32_t)p1[1]) / 2;
      }
      // / 65536 e nao >> 16: (b - a) tem sinal, e o deslocamento arredondaria
      // para -infinito injetando DC (armadilha 12 do AGENTS.md).
      const int16_t v = (int16_t)(a + (b - a) * frac / 65536);
      dst[produced++] = v;
      dst[produced++] = v;
      phaseQ16_ += stepQ16_;
    }
    return produced;
  }

  // -- tarefa de rede (core 0) ----------------------------------------------

  static void netTaskEntry(void *self) {
    static_cast<Stream *>(self)->netTask();
  }

  void netTask() {
    uint32_t attempt = 0;
    while (!stop_.load()) {
      if (sessionOnce())
        attempt = 0; // sessão útil: o próximo tropeço recomeça do 1 s
      if (stop_.load())
        break;
      conn_.store((uint8_t)Conn::NoSignal);
      // Backoff: dorme em fatias para continuar respondendo ao end().
      const uint32_t wait = backoffDelayMs(attempt);
      const uint32_t until = millis() + wait;
      Serial.printf("[M5RETRO] Radio: reconecta em %lums\n", (unsigned long)wait);
      while (!stop_.load() && !timeReached(millis(), until))
        vTaskDelay(pdMS_TO_TICKS(100));
      if (attempt < 5)
        ++attempt;
      reconnects_.fetch_add(1);
    }
    netAlive_.store(false);
    vTaskDelete(nullptr);
  }

  // Uma sessão HTTP completa. Devolve true se chegou a entrar no ar.
  //
  // Locais: só `buf[256]`. A pilha desta tarefa é de 8 KB e duas tarefas já a
  // estouraram neste repositório (AGENTS.md 2.3) — nada de array grande aqui.
  bool sessionOnce() {
    conn_.store((uint8_t)Conn::Connecting);
    ring_.clear();
    resetDecoder();
    header_.reset();
    WiFiClient client;
    client.setTimeout(5); // segundos: o WiFiClient do arduino-esp32 usa segundos
    if (!client.connect(url_.host, url_.port, CONNECT_TIMEOUT_MS)) {
      Serial.printf("[M5RETRO] Radio: falha ao conectar em %s:%u\n", url_.host, (unsigned)url_.port);
      return false;
    }
    // Icy-MetaData: 1 pede o título da música. Connection: close porque não há
    // reuso a fazer — este socket fica aberto até cair.
    client.printf("GET %s HTTP/1.1\r\n"
                  "Host: %s\r\n"
                  "User-Agent: M5RetroTV/1.0\r\n"
                  "Icy-MetaData: 1\r\n"
                  "Connection: close\r\n"
                  "\r\n",
                  url_.path, url_.host);

    uint8_t buf[256]; // único local grande da tarefa
    // --- cabeçalho ---
    const uint32_t hdrDeadline = millis() + HEADER_TIMEOUT_MS;
    bool started = false;
    while (!stop_.load() && !header_.done()) {
      if (timeReached(millis(), hdrDeadline)) {
        client.stop();
        return false;
      }
      const int n = client.read(buf, sizeof(buf));
      if (n <= 0) {
        if (!client.connected() && !client.available()) {
          client.stop();
          return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
      }
      const size_t used = header_.feed(buf, (size_t)n);
      if (header_.done()) {
        if (!header_.ok()) {
          Serial.printf("[M5RETRO] Radio: resposta invalida (status %ld)\n",
                        (long)header_.info().status);
          client.stop();
          return false;
        }
        const IcyInfo &icy = header_.info();
        if (icy.name[0])
          strncpy(icyName_, icy.name, sizeof(icyName_) - 1);
        splitter_.begin(icy.metaint);
        Serial.printf("[M5RETRO] Radio: %s metaint:%ld\n", stationName(), (long)icy.metaint);
        started = true;
        // A sobra do MESMO read já é corpo: passa pelo de-interleave in-place.
        if ((size_t)n > used) {
          const size_t got = splitter_.split(buf + used, (size_t)n - used, buf);
          ring_.write(buf, got);
          bytesIn_.fetch_add((uint32_t)got);
        }
      }
    }
    if (!started || stop_.load()) {
      client.stop();
      return false;
    }

    // --- corpo ---
    uint32_t lastData = millis();
    bool onAir = false;
    while (!stop_.load()) {
      if (!client.connected() && !client.available())
        break;
      if (timeReached(millis(), lastData + STALL_TIMEOUT_MS)) {
        Serial.println("[M5RETRO] Radio: fluxo mudo, derrubando");
        break;
      }
      // Anel quase cheio: o decodificador está atrasado. Espera em vez de
      // descartar, porque descartar byte de MP3 é estalo garantido.
      if (ring_.space() < sizeof(buf)) {
        vTaskDelay(pdMS_TO_TICKS(20));
        continue;
      }
      const int n = client.read(buf, sizeof(buf));
      if (n <= 0) {
        vTaskDelay(pdMS_TO_TICKS(5));
        continue;
      }
      lastData = millis();
      // in-place: os bytes de áudio só andam para a esquerda.
      const size_t got = splitter_.split(buf, (size_t)n, buf);
      if (got)
        ring_.write(buf, got);
      bytesIn_.fetch_add((uint32_t)got);
      if (splitter_.takeTitleFlag())
        publishTitle(splitter_.title());
      if (!onAir && ring_.used() >= PREBUFFER_BYTES) {
        onAir = true;
        conn_.store((uint8_t)Conn::OnAir);
        Serial.println("[M5RETRO] Radio: no ar");
      }
    }
    client.stop();
    return onAir;
  }

  void publishTitle(const char *t) {
    // Trava de ocupado curtíssima: uma cópia de <=128 bytes. Não é um mutex de
    // verdade e não precisa ser — a alternativa (leitor ver meia string) só
    // afetaria um quadro de texto.
    while (titleLock_.load())
      vTaskDelay(1);
    strncpy(title_, t, sizeof(title_) - 1);
    title_[sizeof(title_) - 1] = 0;
    Serial.printf("[M5RETRO] Radio: %s\n", title_);
  }

  // -- estado ---------------------------------------------------------------

  Url url_{};
  int station_ = 0;
  bool running_ = false;

  uint8_t *psram_ = nullptr;
  Ring ring_;
  unsigned char *mp3In_ = nullptr;
  int16_t *mp3Pcm_ = nullptr;

  HMP3Decoder dec_ = nullptr;
  size_t mp3InLen_ = 0, mp3PcmCount_ = 0;
  int mp3Rate_ = 44100, mp3Chans_ = 2, bitrate_ = 0;
  // Fase e passo do reamostrador em Q16. Eram `double`: o ESP32 nao tem FPU de
  // precisao dupla, e o laco fazia uma DIVISAO dupla por amostra de saida
  // (22.050 por segundo), emulada em software. Era isso que nao cabia nos
  // 11,6 ms de cada volta do audioTask.
  uint32_t phaseQ16_ = 0;
  uint32_t stepQ16_ = 1u << 16;

  HeaderParser header_;
  MetaSplitter splitter_;

  char name_[40] = {0};    // da tabela
  char icyName_[40] = {0}; // do servidor
  char title_[128] = {0};

  std::atomic<uint8_t> conn_{(uint8_t)Conn::Idle};
  std::atomic<bool> stop_{false};
  std::atomic<bool> netAlive_{false};
  std::atomic<bool> pcmBusy_{false};
  mutable std::atomic<bool> titleLock_{false};
  std::atomic<uint32_t> reconnects_{0};
  std::atomic<uint32_t> bytesIn_{0};
};

} // namespace radio

#endif // ARDUINO
