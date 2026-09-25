#!/usr/bin/env python3
"""Converte fotos para o formato que o slideshow do Fruit Jam Retro TV consegue exibir.

O formato e o mesmo do upstream (m5-retro-tv, Core2): o JPEGDEC e o MAX_JPEG
sao os mesmos, entao a mesma pasta /M5RETRO/fotos serve nos dois aparelhos.

Uso:
    python3 tools/prepare_photos.py ORIGEM [ORIGEM...] DESTINO

ORIGEM pode ser arquivo ou pasta (varrida recursivamente). DESTINO recebe os
.jpg prontos para copiar em /M5RETRO/fotos no cartao.

Por que este script existe, em vez de "e so copiar o JPEG":

    1. 4:2:0 obrigatorio. O JPEGDEC do firmware devolve JPEG_DECODE_ERROR em
       chroma 4:4:4. Medido: um q2 em yuvj444p renderiza 0x0; um q5 renderiza
       so uma faixa de 320x80 antes de falhar.

    2. Baseline obrigatorio. JPEG progressivo NAO da erro: decode() devolve
       sucesso e desenha 40x30 pixels, porque a biblioteca so le o primeiro
       scan (os coeficientes DC). Ver jpeg.inl:4961. E o padrao do "save for
       web" de varias ferramentas, entao a foto chega e parece corrompida sem
       nenhum erro para capturar.

    3. Teto de 128 KB. O buffer de decodificacao e ps_malloc(MAX_JPEG) com
       MAX_JPEG = 128*1024 (src/main.cpp). Foto de celular tem 0,5 a 5 MB
       e simplesmente nao cabe.

    4. Escala. O JPEGDEC so reduz em 1/2, 1/4 e 1/8. Uma foto de 4000 px em
       1/8 ainda da 500 px, maior que os 320 do quadro. Redimensionar aqui
       evita depender disso.

O alvo e 320x240 baseline yuvj420p em -q:v 5, que mediu ~8 KB por foto e 1267
cores depois do RGB565 (a qualidade quase nao muda a contagem de cores: q2 deu
1249 e q10 deu 1302 -- o que limita nao e o JPEG).
"""

import argparse
import json
import os
import subprocess
import sys

# Precisa bater com MAX_JPEG em src/main.cpp.
MAX_JPEG = 128 * 1024

# Quadro logico da TV: 320x240, dobrado para 640x480 no DVI. Ver include/SafeArea.h.
FRAME_W, FRAME_H = 320, 240

EXTENSOES = {".jpg", ".jpeg", ".png", ".heic", ".heif", ".webp", ".tif", ".tiff", ".bmp", ".gif"}


def ffmpeg_existe():
    try:
        subprocess.run(["ffmpeg", "-version"], capture_output=True, check=True)
        return True
    except (OSError, subprocess.CalledProcessError):
        return False


def coletar(origens):
    """Devolve os arquivos de imagem das origens, em ordem estavel."""
    achados = []
    for origem in origens:
        if os.path.isfile(origem):
            achados.append(origem)
        elif os.path.isdir(origem):
            for raiz, _, arquivos in os.walk(origem):
                for nome in sorted(arquivos):
                    if os.path.splitext(nome)[1].lower() in EXTENSOES:
                        achados.append(os.path.join(raiz, nome))
        else:
            print("ignorado (nao existe): %s" % origem, file=sys.stderr)
    return achados


def nome_saida(caminho, usados):
    """Nome ASCII, maiusculo, curto e unico -- a fonte do firmware e ASCII."""
    base = os.path.splitext(os.path.basename(caminho))[0]
    limpo = "".join(c if c.isalnum() else "_" for c in base).strip("_").upper()
    limpo = limpo[:24] or "FOTO"
    candidato = limpo
    n = 2
    while candidato in usados:
        candidato = "%s_%d" % (limpo[:21], n)
        n += 1
    usados.add(candidato)
    return candidato + ".jpg"


def converter(entrada, saida, qualidade, preencher):
    """Uma foto. Devolve o tamanho final em bytes, ou None se falhou."""
    if preencher:
        # Cobre o quadro inteiro e corta o excesso: sem barras pretas, mas
        # perde as bordas da foto.
        vf = ("scale=%d:%d:force_original_aspect_ratio=increase,"
              "crop=%d:%d" % (FRAME_W, FRAME_H, FRAME_W, FRAME_H))
    else:
        # Cabe inteira e completa com preto. Preserva o enquadramento.
        vf = ("scale=%d:%d:force_original_aspect_ratio=decrease,"
              "pad=%d:%d:(ow-iw)/2:(oh-ih)/2:black" % (FRAME_W, FRAME_H, FRAME_W, FRAME_H))

    cmd = [
        "ffmpeg", "-nostdin", "-v", "error", "-y",
        "-i", entrada,
        "-vf", vf,
        "-pix_fmt", "yuvj420p",   # 4:2:0: 4:4:4 nao decodifica no dispositivo
        "-q:v", str(qualidade),
        "-frames:v", "1",
        saida,
    ]
    resultado = subprocess.run(cmd, capture_output=True)
    if resultado.returncode != 0:
        erro = resultado.stderr.decode("utf-8", "replace").strip().splitlines()
        print("  FALHOU: %s" % (erro[-1] if erro else "ffmpeg %d" % resultado.returncode),
              file=sys.stderr)
        return None
    return os.path.getsize(saida)


def verificar_baseline(caminho):
    """Confere no arquivo gerado que o SOF e 0xFFC0 (baseline), nao 0xFFC2.

    O ffmpeg nao emite progressivo por padrao, mas isto e barato e o modo
    progressivo falha em silencio no dispositivo -- vale conferir em vez de
    confiar.
    """
    with open(caminho, "rb") as f:
        dados = f.read(64 * 1024)
    i = 2  # pula o SOI
    while i + 3 < len(dados):
        if dados[i] != 0xFF:
            i += 1
            continue
        marcador = dados[i + 1]
        if marcador == 0xC0:
            return True, "baseline"
        if marcador == 0xC2:
            return False, "progressivo"
        if marcador in (0xC1, 0xC3):
            return False, "modo 0x%02X nao suportado" % marcador
        if marcador in (0xD8, 0x01) or 0xD0 <= marcador <= 0xD7:
            i += 2
            continue
        comprimento = (dados[i + 2] << 8) | dados[i + 3]
        if comprimento < 2:
            return False, "cabecalho invalido"
        i += 2 + comprimento
    return False, "sem SOF"


def main():
    p = argparse.ArgumentParser(
        description="Prepara fotos para /M5RETRO/fotos no Fruit Jam Retro TV.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Exemplo:\n"
               "  python3 tools/prepare_photos.py ~/Pictures/viagem ~/Downloads/M5RETRO-fotos\n")
    p.add_argument("caminhos", nargs="+", help="uma ou mais origens, e por ultimo o destino")
    p.add_argument("--qualidade", type=int, default=5, metavar="N",
                   help="qualidade do ffmpeg, 2 (melhor) a 31 (pior). Padrao 5, "
                        "que mediu ~8 KB e 1267 cores. Acima de 5 o ganho de "
                        "cor e nulo e o arquivo so cresce.")
    p.add_argument("--preencher", action="store_true",
                   help="cortar para preencher o quadro inteiro em vez de "
                        "completar com preto")
    args = p.parse_args()

    if len(args.caminhos) < 2:
        p.error("informe pelo menos uma origem e um destino")
    if not 2 <= args.qualidade <= 31:
        p.error("--qualidade fora da faixa 2..31")
    if not ffmpeg_existe():
        print("ffmpeg nao encontrado no PATH.", file=sys.stderr)
        return 1

    *origens, destino = args.caminhos
    arquivos = coletar(origens)
    if not arquivos:
        print("nenhuma imagem encontrada.", file=sys.stderr)
        return 1

    os.makedirs(destino, exist_ok=True)
    usados = set()
    indice = []
    total = grandes = falhas = 0

    print("%d imagem(ns) -> %s" % (len(arquivos), destino))
    for caminho in arquivos:
        nome = nome_saida(caminho, usados)
        alvo = os.path.join(destino, nome)
        print("  %s -> %s" % (os.path.basename(caminho), nome))
        tamanho = converter(caminho, alvo, args.qualidade, args.preencher)
        if tamanho is None:
            falhas += 1
            if os.path.exists(alvo):
                os.remove(alvo)
            continue
        ok, modo = verificar_baseline(alvo)
        if not ok:
            print("  REJEITADO (%s): o dispositivo nao exibe este arquivo" % modo,
                  file=sys.stderr)
            os.remove(alvo)
            falhas += 1
            continue
        if tamanho > MAX_JPEG:
            # Nao deveria acontecer em 320x240, mas o limite e do firmware e
            # uma foto aceita aqui e rejeitada la seria pior.
            print("  REJEITADO: %d B passa do teto de %d B do firmware"
                  % (tamanho, MAX_JPEG), file=sys.stderr)
            os.remove(alvo)
            grandes += 1
            continue
        total += tamanho
        indice.append({"arquivo": nome, "origem": os.path.basename(caminho), "bytes": tamanho})

    if indice:
        with open(os.path.join(destino, "fotos.json"), "w") as f:
            json.dump({"formato": "320x240 baseline yuvj420p",
                       "qualidade": args.qualidade,
                       "fotos": indice}, f, indent=2)

    print()
    print("%d foto(s) prontas, %s no total, media de %s por foto"
          % (len(indice), humano(total), humano(total // len(indice) if indice else 0)))
    if falhas:
        print("%d falharam" % falhas)
    if grandes:
        print("%d passaram do teto de 128 KB" % grandes)
    print()
    print("Copie o conteudo de %s para /M5RETRO/fotos no cartao," % destino)
    print("ou envie pelo menu TRANSFERIR ARQUIVOS.")
    return 0 if indice else 1


def humano(n):
    if n < 1024:
        return "%d B" % n
    if n < 1024 * 1024:
        return "%.1f KB" % (n / 1024.0)
    return "%.1f MB" % (n / (1024.0 * 1024.0))


if __name__ == "__main__":
    sys.exit(main())
