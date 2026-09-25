# pio_fruitjam_launcher_uf2.py - extra_script (post) do env "fruitjam-launcher".
#
# platform-raspberrypi (maxgerhardt) gera o UF2 com
#     picotool uf2 convert -t elf <elf> <uf2>
# sem --family nem --platform (ver builder/main.py, generate_uf2()). Para uma
# imagem que comeca em 0x10000000 (o env "fruitjam" padrao) isso funciona: o
# picotool deduz a familia certa (rp2350-arm-s) e valida os enderecos contra o
# RP2350 sem ajuda. Para esta imagem, que comeca em 0x10C00000 (o topo da
# flash reservado pelo fork fruitjam-retro-tv do pico-bootLoader -- ver
# PORTING.md, secao "Emuladores"), o picotool >= 2.2 nao reconhece o layout e
# faz duas coisas erradas:
#   1. marca o UF2 com a familia 'rp2040' em vez de 'rp2350-arm-s' -- a ROM de
#      BOOTSEL do RP2350 pode ignorar blocos com a familia errada;
#   2. recusa o segmento de RAM (SCRATCH_X/Y em 0x20080000+) com "outside of
#      valid address range for device", porque a deducao automatica de
#      plataforma parte do endereco do primeiro segmento carregado.
#
# O proprio pico-bootLoader teve exatamente este problema (ver o comentario
# sobre "picotool >= 2.2.0" em pico_shared/BootPartition.cmake, repositorio
# do lancador) e a correcao e a mesma: passar --family e --platform
# explicitamente. Aqui isso e feito refazendo a conversao depois que o
# platform-raspberrypi jah rodou a dele (AddPostAction empilha, nao
# substitui -- a nossa roda depois e regrava o .uf2 por cima).
Import("env")

if env["PIOENV"] == "fruitjam-launcher":

    def fix_uf2(target, source, env):
        elf_file = target[0].get_path()
        uf2_file = elf_file.replace(".elf", ".uf2")
        # A ordem importa para o parser do picotool (CLI11): as opcoes tem de
        # vir DEPOIS dos dois posicionais (infile, outfile), senao ele recusa
        # com "unexpected option" -- ja aconteceu aqui.
        cmd = " ".join([
            "picotool", "uf2", "convert", "-t", "elf",
            '"%s"' % elf_file,
            '"%s"' % uf2_file,
            "--family", "rp2350-arm-s",
            "--platform", "rp2350",
        ])
        env.Execute(cmd)

    env.AddPostAction(
        env.subst("$BUILD_DIR/${PROGNAME}.elf"),
        env.VerboseAction(
            fix_uf2,
            "Regenerating UF2 with --family/--platform rp2350 (fruitjam-launcher, "
            "origem de flash 0x10C00000)",
        ),
    )
