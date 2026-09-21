# Música de teste para o player MP3/WAV

Cole o conteúdo desta pasta no cartão SD, em:

```
/M5RETRO/music/
  Artista Teste/
    Album/
      cover.jpg            (ou folder.jpg) — capa do álbum (JPEG)
      01-faixa-musica.mp3  (128 kbps, ID3: "Música de Teste" / "Artista Cômico")
      02-nao-me-deixes.mp3 (192 kbps, ID3: "Não Me Deixes" / "Banda Açúcar")
      03-faixa-wav.wav     (WAV PCM 16-bit 22050 Hz estéreo)
      04-com-capa-embutida.mp3  (160 kbps, APIC embutido: "Banda Capa Embutida")
```

Os arquivos foram gerados com `ffmpeg`/`lame` e exercitam:

- **Acentos** nas tags ID3 ("Música", "Cômico", "Álbum", "Não", "Açúcar") — o
  módulo `include/Ascii.h` normaliza para ASCII na tela.
- **Capa do álbum** via `cover.jpg`/`folder.jpg` (decodificada e exibida na RCA).
- **Capa embutida (APIC)** no `04-com-capa-embutida.mp3` — a imagem `cover.jpg`
  é anexada como *attached picture* (frame `APIC`, stream de vídeo `mjpeg`,
  `attached_pic=1`), exercitando o caminho de capa-apic do player.
- **MP3** (128/192/160 kbps, 44100 Hz, estéreo) → decodificado por libhelix e
  reamostrado para 22050 Hz; e **WAV** nativo.
- **Skip do cabeçalho ID3** — o firmware pula a tag ID3v2 inteira antes de
  decodificar. O offset de início do áudio é `10 + tamanho_syncsafe` (bytes
  6–9 do header `ID3`, tamanho lido como *syncsafe*), mais `10` se houver
  footer (ID3v2.4 com bit 0x10 nas flags).

Offsets medidos (`audioStart` = primeiro byte do primeiro frame MPEG, `0xFFEx`):

| Arquivo                     | ID3     | tamanho | audioStart |
| --------------------------- | ------- | ------- | ---------- |
| `01-faixa-musica.mp3`       | v2.3    | 229     | 239 (0xef) |
| `02-nao-me-deixes.mp3`      | v2.3    | 221     | 231 (0xe7) |
| `04-com-capa-embutida.mp3`  | v2.3    | 3004    | 3014 (0xbc6) |

Regenerar com:

```sh
lame --tt "Título" --ta "Artista" --tl "Álbum" --ty 2026 -b 128 in.wav out.mp3
ffmpeg -f lavfi -i "sine=frequency=440:duration=8" -ar 22050 -ac 2 -c:a pcm_s16le faixa.wav
ffmpeg -f lavfi -i "gradients=size=300x300:c0=0x2233aa:c1=0xaa2266" -frames:v 1 cover.jpg
# APIC (capa embutida):
ffmpeg -f lavfi -i "sine=frequency=523:duration=8" -ar 44100 -ac 2 \
  -c:a libmp3lame -b:a 160k -id3v2_version 3 \
  -metadata title="Faixa Com Capa" -metadata artist="Banda Capa Embutida" \
  -metadata album="Álbum de Exemplo" -metadata date=2026 /tmp/tmp.mp3
ffmpeg -i /tmp/tmp.mp3 -i cover.jpg -map 0:a -map 1:v \
  -c:a copy -c:v mjpeg -id3v2_version 3 \
  -metadata:s:v title="Album cover" -metadata:s:v comment="Cover (front)" \
  04-com-capa-embutida.mp3
```
