#pragma once
// Teclado e gamepad pelas portas USB host do Fruit Jam.
//
//   D+  GPIO 1   D-  GPIO 2 (D+ +1, fixo pelo PIO-USB)   5V_EN  GPIO 11
//
// Pilha: Adafruit_TinyUSB (device nativo, rhport 0 -- e o Serial/CDC da
// gravacao) + Pico-PIO-USB (host bit-banged num PIO inteiro, rhport 1). Por
// isso o platformio.ini define USE_TINYUSB nos build_flags (o equivalente,
// para quem so conhece a IDE Arduino, do menu Tools -> USB Stack -> "Adafruit
// TinyUSB"): a variante "...Host (native)" viraria o controlador NATIVO em
// host e mataria o Serial da gravacao -- inaceitavel (AGENTS.md pede o
// diagnostico serial funcionando sempre). O PlatformIO nao le o menu do
// boards.txt da IDE; quem realmente decide isto e o define, conferido em
// framework-arduinopico/tools/platformio-build.py (configure_usb_flags).
//
// Decisoes registradas aqui, com o porque (todas em PORTING.md tambem):
//
// - PIO: forcado para o PIO2 (pio_tx_num=pio_rx_num=2). O RP2350 tem 3 PIOs; o
//   SDIO do cartao e o I2S do audio usam o alocador dinamico do arduino-pico
//   (PIOProgram), que tenta o PIO0 primeiro. O Pico-PIO-USB NAO usa esse
//   alocador: carrega o programa TX sempre no offset 0 do PIO escolhido, sem
//   checar se ja tem algo la (pio_usb.c, pio_usb_bus_init). Se caisse no mesmo
//   PIO que o SDIO/I2S, um dos dois teria a memoria de instrucoes corrompida
//   em silencio. Reservar o PIO2 so para o USB evita a colisao enquanto
//   SDIO+I2S continuarem cabendo em PIO0/PIO1 -- o que e o caso hoje (cada um
//   usa 1-2 state machines de 4). NAO TESTADO: se um dia SDIO ou I2S
//   precisarem de um terceiro PIO, isto tem de ser revisto.
//
// - Nucleo: a tarefa do host (USB_HOST) fica fixa no nucleo 1. A pilha
//   Pico-PIO-USB instala um alarme de hardware de 1 ms (pio_usb_host.c,
//   start_timer/sof_timer) que gera o SOF e roda a maquina de estado da
//   transferencia -- isto e HARDWARE (IRQ de alarme), independente de
//   qualquer tarefa do FreeRTOS, e fica atrelado ao nucleo que chamou
//   USBHost.begin(). O nucleo 0 tem a interrupcao de linha do DVI, que roda a
//   ~31,5 kHz (640x480@60) e nao pode perder prazo; colocar mais um IRQ
//   periodico ali é o unico jeito de arriscar o video. O nucleo 1 ja tem
//   RCA_PCM (audio, prioridade 4) e VIDEO_DEC (decode, prioridade 2); um IRQ
//   de alguns microssegundos a cada 1 ms e o preco aceito, NAO TESTADO.
//
// - A tarefa USB_HOST em si so bombeia `tuh_task()`: com CFG_TUSB_OS=
//   OPT_OS_PICO (o padrao do Adafruit_TinyUSB pro RP2040/2350), a fila interna
//   do TinyUSB (osal_pico.h, osal_queue_receive) NUNCA bloqueia -- trata
//   qualquer timeout como zero. Ou seja tuh_task() so drena o que ja chegou e
//   volta; quem decide a cadencia de poll e o vTaskDelay() da nossa propria
//   tarefa. Prioridade 2 (a mesma do VIDEO_DEC), sempre abaixo do RCA_PCM
//   (4): entrada nunca pode competir com audio.
//
// - Clock: o `board_build.f_cpu` do platformio.ini fica em 150 MHz (exigencia
//   de build do DVHSTX), mas em tempo de execucao o clk_sys passa por DOIS
//   ajustes: um preinit generico do DVHSTX sobe para 240 MHz antes do
//   setup() (dvhstx.cpp, display_setup_clock_preinit -- PLL USB 480 MHz / 2);
//   display::begin() troca de novo, para o valor EXATO que o modo de video
//   pede (DVHSTX::display_setup_clock, check_sys_clock_khz). Para
//   640x480@60 esse valor e 126 MHz (bit_clk_khz=252000 -> /2), NAO 240 nem
//   264 como a documentacao antiga deste port dizia -- conferido lendo
//   dvhstx.cpp e dvi.cpp, nao suposto. 126 MHz NAO e multiplo de 12 MHz (a
//   regra que os proprios exemplos da Adafruit para PIO-USB tratam como
//   obrigatoria e travam o boot se nao bater). Este codigo NAO trava o boot
//   por isso -- pararia o firmware inteiro por causa de um requisito de uma
//   biblioteca opcional, o que AGENTS.md nao aceita (retentativa/travamento
//   sem saida). Em vez disso `logClockFit()` mede e imprime o fato: com
//   clk_sys=126 MHz os divisores fracionarios que o Pico-PIO-USB calcula para
//   full/low speed (clk_sys/48e6, /96e6, /6e6, /12e6) caem em 2,625 / 1,3125 /
//   21 / 10,5 -- todos exatos em oitavos, dentro da resolucao de 1/256 do
//   divisor de clock do PIO (sem arredondamento). Isso sugere que funciona,
//   mas e teoria de datasheet: o PRIMEIRO suspeito, se o teclado/gamepad
//   piscar, travar ou perder relatorios no aparelho, e este clock.
#include "fj/UsbHidMap.h"
#include <stdint.h>

namespace usbhost {

// Uma acao de navegacao decodificada de um teclado/gamepad. Quem chama
// usbhost::begin() fornece esta funcao; UsbHost.cpp nao inclui InputManager.h
// nem sabe da fila do loop() -- so decodifica bytes de HID e entrega a acao,
// do mesmo jeito que FileTransfer entrega o comando web por um callback
// (src/main.cpp, webCommand/setCommandHandler) em vez de chamar input.inject()
// direto. Chamado da tarefa USB_HOST (nucleo 1): so enfileira, nunca desenha.
using ActionHandler = void (*)(NavAction action);

// Sobe o PIO-USB e a tarefa dedicada. Chamado uma vez em appSetup(), depois de
// display::begin() (o DVI ja fixou o clk_sys final) e storage::begin(), e
// antes de audioout::begin() e das tarefas de rede -- a ordem entre USB e
// audio/rede nao importa uma pela outra (pinos diferentes), mas display::
// begin() TEM de vir antes: e ele quem decide o clk_sys que logClockFit() mede.
void begin(ActionHandler onAction);

// Estado para "diag usb". Nunca bloqueia: le so o que a tarefa do host ja
// publicou.
uint8_t deviceCount();
struct DeviceLine {
  char text[80];
};
DeviceLine describe(uint8_t idx);
const char *lastEvent();

} // namespace usbhost
