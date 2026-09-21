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
```

Os arquivos foram gerados com `ffmpeg`/`lame` e exercitam:

- **Acentos** nas tags ID3 ("Música", "Cômico", "Álbum", "Não", "Açúcar") — o
  módulo `include/Ascii.h` normaliza para ASCII na tela.
- **Capa do álbum** via `cover.jpg`/`folder.jpg` (decodificada e exibida na RCA).
- **MP3** (128/192 kbps, 44100 Hz, estéreo) → decodificado por libhelix e
  reamostrado para 22050 Hz; e **WAV** nativo.

Regenerar com:

```sh
lame --tt "Título" --ta "Artista" --tl "Álbum" --ty 2026 -b 128 in.wav out.mp3
ffmpeg -f lavfi -i "sine=frequency=440:duration=8" -ar 22050 -ac 2 -c:a pcm_s16le faixa.wav
ffmpeg -f lavfi -i "gradients=size=300x300:c0=0x2233aa:c1=0xaa2266" -frames:v 1 cover.jpg
```
