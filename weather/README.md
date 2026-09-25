# The Weather Channel — Local Forecast (clone anos 80)

Firmware autônomo para o **Adafruit Fruit Jam** (RP2350B): imagem pela saída
**DVI** (640×480 a 60 Hz, com o quadro de 320×240 dobrado nos dois eixos) e
música de fundo pelo DAC **TLV320DAC3100** da placa, recriando a estética do
*Local Forecast* dos anos 80. Os dados vêm da [Open-Meteo](https://open-meteo.com)
— API pública, sem cadastro nem chave — lidos pela rede Wi-Fi e exibidos com
fontes bitmap sobre fundo azul-escuro, com um ticker de previsão rolando no
rodapé.

Este é o fork do projeto que rodava no M5Stack Core2 + módulo RCA. A tela
continua dentro da área segura de um tubo (margem de 24 px na horizontal e
18 px na vertical): um monitor DVI mostra o quadro inteiro, mas TV ligada por
HDMI costuma reaplicar o overscan.

> **Nada deste port foi testado no aparelho.** Compila; o comportamento no
> Fruit Jam ainda precisa ser conferido.

## Arquitetura FreeRTOS

O `setup()` segue a ordem de boot fixa do `PORTING.md` (§2.1): placa, DVI,
cartão, rádio Wi-Fi e só então o DAC — o rádio pulsa o GPIO 22, que também zera
o TLV320.

No **núcleo 0** ficam o `loop()` do Arduino e a interrupção de linha do DVI. O
loop cuida da renderização: desenha a tela estática apenas quando chegam dados
novos e desliza o ticker de forma **não-bloqueante** usando `millis()` (2 px a
cada ~33 ms). O ticker é desenhado num sprite de 320×16 e copiado para o
framebuffer de uma vez, para não piscar. O loop **nunca** chama o WiFiNINA.

No **núcleo 1** rodam:

- a tarefa de áudio (`RCA_MUSIC`, prioridade 4), que lê
  `/M5RETRO/weather_music.wav` do cartão em blocos de 1024 bytes e entrega o
  PCM ao `audioout::write()`, que bloqueia enquanto o buffer do I2S está cheio
  e por isso dita o ritmo. Um teto pelo relógio de amostras (no máximo 100 ms à
  frente do tempo real) cobre o caso de o DAC não bloquear. Ao chegar ao fim dos
  dados ela faz `seek()` de volta ao início: loop contínuo. A saída é o **fone
  P2**, que é a que vai à entrada de áudio da TV — o DVI não leva som.
- a tarefa HTTP (`WEATHER_HTTP`, prioridade 1), criada a cada ~10 minutos (30 s
  depois de uma falha). Ela conecta ao Wi-Fi se preciso — o `WiFi.begin()` do
  NINA bloqueia dezenas de segundos, e por isso não mora no loop —, faz o GET na
  Open-Meteo por `net::httpGet` para um buffer de 4 KiB na PSRAM, parseia com o
  ArduinoJson 7 e publica o resultado num buffer duplo protegido por atômicos.
  A resposta é rejeitada quando chega truncada, em vez de publicar dados pela
  metade. O TLS roda dentro do ESP32-C6.

Se o DAC não iniciar, a previsão segue muda em vez de parar na tela de erro.

## Camada de plataforma

O código da previsão (WAV, parse do tempo, ticker) é duplicado de propósito do
firmware principal (ver `AGENTS.md` §1). A camada do Fruit Jam não é: o
`platformio.ini` daqui aponta `include_dir` para `../include` e compila
`../src/fj/*.cpp` pelo `build_src_filter`. Vídeo (`fj/Display.h`, canvas `tv`),
botões e reset (`fj/Board.h`), cartão (`fj/Storage.h`), áudio (`fj/AudioOut.h`)
e rede (`fj/Net.h`) são os mesmos arquivos do firmware principal.

## Gravação

A partir da raiz do repositório (onde está o `.venv`):

```sh
.venv/bin/pio run -d weather --target upload   # segure BOOT, aperte RESET
.venv/bin/pio run -d weather --target monitor  # opcional: log serial (USB)
```

Sem acesso ao registro do PlatformIO, crie `weather/platformio_local.ini` como
descrito no `PORTING.md` §4 (o ambiente também se chama `fruitjam`).

Antes de compilar, edite `weather/src/main.cpp` e preencha as credenciais:

```cpp
#define WIFI_SSID "..."
#define WIFI_PASS "..."
```

## Cartão SD

Coloque a música de fundo em **`/M5RETRO/weather_music.wav`** no microSD do
Fruit Jam — a mesma pasta do Core2, para que um cartão sirva aos dois. O arquivo
deve ser **PCM 16-bit estéreo 22050 Hz** (o firmware valida esse formato no
cabeçalho WAV). Sem cartão, o firmware para na tela `SEM CARTAO SD`; sem esse
arquivo (ou com outro formato), na tela `WAV INVALIDO`.

O cartão fica num barramento **SDIO** próprio (CLK 34, CMD 35, D0..D3 36..39),
sem dividir nada com o vídeo. A previsão só lê do cartão.
