# The Weather Channel — Local Forecast (clone anos 80)

Firmware para **M5Stack Core2** + módulo **RCA (M125)**: saída de vídeo composto
(NTSC) para TV e áudio pelo RCA, recriando a estética do *Local Forecast* dos
anos 80. Os dados vêm da [Open-Meteo](https://open-meteo.com) — API pública,
sem cadastro nem chave — lidos pela rede Wi-Fi e exibidos com uma fonte bitmap
estilo VCR/teletexto sobre fundo azul-escuro, com um ticker de previsão rolando
no rodapé. Todo o conteúdo é desenhado dentro da área segura do tubo (margem de
24 px na horizontal e 18 px na vertical), porque a TV corta cerca de 7% de cada
borda por overscan.

## Arquitetura FreeRTOS

O trabalho está dividido entre os dois núcleos do ESP32. No **core 0** roda uma
única tarefa de áudio (`RCA_MUSIC`, prioridade 4), que lê o arquivo
`/M5RETRO/weather_music.wav` do cartão SD em blocos de 1024 bytes e alimenta o
I2S1 do módulo RCA (BCK=19, DATA=2, LRCK=0, sem MCK). O ritmo é ditado pelo
próprio relógio de amostras PCM (22050 Hz estéreo 16-bit): a tarefa calcula
quantos microssegundos cada bloco equivale e dorme esse intervalo, evitando
encher a fila DMA; ao chegar ao fim dos dados ela faz `seek()` de volta ao
início, mantendo a música em loop contínuo.

No **core 1** ficam o `loop()` do Arduino e a tarefa HTTP periódica
(`WEATHER_HTTP`, prioridade 0). O loop cuida da renderização — desenha a tela
estática apenas quando chegam dados novos e desliza o ticker de forma
**não-bloqueante** usando `millis()` (2 px a cada ~33 ms), sem `delay()` longo e
sem travar o áudio. A tarefa HTTP roda em prioridade baixa (abaixo do loop e do
áudio), a cada ~10 minutos, faz o `GET` na Open-Meteo, lê a resposta inteira
(~800 bytes) para um buffer de 4 KiB, parseia com o ArduinoJson 7 e publica o
resultado num buffer duplo protegido por atômicos — o loop apenas consome, sem
tearing. A resposta é rejeitada quando chega truncada, em vez de publicar dados
pela metade.

## Gravação

A partir da raiz do repositório (onde está o `.venv`):

```sh
.venv/bin/pio run -d weather --target upload
.venv/bin/pio run -d weather --target monitor   # opcional: log serial
```

Antes de compilar, edite `weather/src/main.cpp` e preencha as credenciais:

```cpp
#define WIFI_SSID "..."
#define WIFI_PASS "..."
```

## Cartão SD

Coloque a música de fundo em **`/M5RETRO/weather_music.wav`** no cartão microSD
do Core2. O arquivo deve ser **PCM 16-bit estéreo 22050 Hz** (o firmware valida
esse formato no cabeçalho WAV). Sem esse arquivo (ou com outro formato), o
firmware para na tela de erro `WAV INVALIDO`.

> O Core2 monta o TF no barramento VSPI (SCK=18, MISO=38, MOSI=23, CS=4).
> O GPIO 19 **não** pode ser usado no SPI porque pertence ao BCK do módulo RCA.