// Teclado e gamepad pelas portas USB host do Fruit Jam. Contrato e as
// decisões de PIO/núcleo/clock em fj/UsbHost.h; decodificação dos relatórios
// HID em fj/UsbHidMap.h (funções puras, testadas em tests/test_core.cpp).
//
// NAO TESTADO NO APARELHO -- nada neste arquivo rodou num Fruit Jam de
// verdade. Ver o comentário de clock em UsbHost.h para o maior risco.
#include "fj/UsbHost.h"

#include "fj/Board.h"
#include "fj/Platform.h"

#include <Arduino.h>
#include <cstdarg>
#include <cstdio>

#include "hardware/clocks.h"

// A ordem importa: pio_usb.h antes de Adafruit_TinyUSB.h só por clareza (os
// dois já se protegem com `#pragma once`); o que importa de verdade é que
// USE_TINYUSB esteja definido no platformio.ini ANTES de qualquer um dos
// dois, porque é isso que tusb_config_rp2040.h usa para decidir "dispositivo
// nativo + host por pio-usb" em vez de "host nativo" (ver o comentário grande
// em UsbHost.h sobre por que a stack tem de ser "Adafruit TinyUSB", não
// "...Host (native)").
#include "pio_usb.h"

#include "Adafruit_TinyUSB.h"

namespace usbhost {
namespace {

// Só um host: PIO2, forçado (ver UsbHost.h). SDIO e I2S usam o alocador
// dinâmico do arduino-pico e tentam PIO0 primeiro; reservar o PIO2 inteiro
// para o USB evita que o Pico-PIO-USB (que carrega seu programa sempre no
// offset 0, sem checar o que já está lá) corrompa a memória de instruções de
// outro periférico.
constexpr uint8_t USB_PIO_INDEX = 2;

// Pilha da tarefa dedicada. TinyUSB e o parser de HID só usam buffers de
// dezenas de bytes (CFG_TUH_HID_EPIN_BUFSIZE=64); 4096 é folga generosa, do
// mesmo tamanho da RCA_PCM (fj/Platform.h conta em bytes, não palavras).
constexpr uint32_t TASK_STACK_BYTES = 4096;
constexpr UBaseType_t TASK_PRIORITY = 2; // igual à VIDEO_DEC; sempre < RCA_PCM (4)
constexpr BaseType_t TASK_CORE = 1;      // nunca 0: lá mora a interrupção de linha do DVI

Adafruit_USBH_Host USBHost;
ActionHandler g_onAction = nullptr;
char g_lastEvent[80] = "nenhum dispositivo ainda";

void setLastEvent(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vsnprintf(g_lastEvent, sizeof(g_lastEvent), fmt, args);
  va_end(args);
}

// Um dispositivo de entrada (teclado, gamepad genérico ou DualShock4/
// DualSense) conectado agora. CFG_TUH_HID do tusb_config_rp2040.h permite até
// 3*(3*CFG_TUH_HUB+1) interfaces HID simultâneas; guardamos menos (8 chega
// muito além do que alguém pluga num hub de TV retrô) e ignoramos o resto com
// aviso em vez de estourar.
constexpr uint8_t MAX_DEVICES = 8;

enum class Kind : uint8_t { NONE, KEYBOARD, GENERIC_GAMEPAD, DUALSHOCK4, DUALSENSE };

struct Device {
  bool used = false;
  uint8_t devAddr = 0, instance = 0;
  uint16_t vid = 0, pid = 0;
  Kind kind = Kind::NONE;
  usbhidmap::GamepadFieldLayout layout; // só para GENERIC_GAMEPAD
  usbhidmap::LogicalButtons state;
  // Repique: só para as quatro direções, do mesmo jeito que o InputManager
  // repete LEFT/RIGHT segurado. Os atalhos (SELECT/BACK/HOME/...) disparam
  // uma vez por borda -- repetir BACK sozinho ficaria voltando sem parar.
  NavAction firedAction = NavAction::NONE;
  uint32_t downAt = 0, nextRepeatAt = 0;
};
Device g_devices[MAX_DEVICES];

// Os mesmos números do InputManager (include/InputManager.h), copiados aqui
// porque são privados da classe: a intenção é o repique parecer igual,
// físico ou USB, não reusar a implementação (a fonte do estado é diferente:
// lá é board::buttonDown(), aqui é o último relatório HID decodificado).
constexpr uint16_t REPEAT_DELAY_MS = 400;
constexpr uint16_t REPEAT_MS = 120;

bool isDirectional(NavAction a) {
  return a == NavAction::LEFT || a == NavAction::RIGHT || a == NavAction::UP || a == NavAction::DOWN;
}

Device *findDevice(uint8_t devAddr, uint8_t instance) {
  for (auto &d : g_devices)
    if (d.used && d.devAddr == devAddr && d.instance == instance)
      return &d;
  return nullptr;
}

Device *allocDevice(uint8_t devAddr, uint8_t instance) {
  for (auto &d : g_devices)
    if (!d.used) {
      d = Device{};
      d.used = true;
      d.devAddr = devAddr;
      d.instance = instance;
      return &d;
    }
  return nullptr;
}

// Chamado a cada volta da tarefa (não só quando chega relatório novo): um
// dispositivo que trava sem soltar o botão -- ou que só reenvia o relatório
// quando MUDA, não a cada intervalo de polling -- ainda tem de gerar repique
// pelo relógio da nossa própria tarefa, não pelo dele.
void serviceRepeat(Device &d, uint32_t now) {
  if (!g_onAction)
    return;
  const NavAction action = usbhidmap::primaryAction(d.state);
  if (action != d.firedAction) {
    d.firedAction = action;
    if (action != NavAction::NONE) {
      g_onAction(action);
      d.downAt = now;
      d.nextRepeatAt = now + REPEAT_DELAY_MS;
    }
    return;
  }
  if (action == NavAction::NONE || !isDirectional(action))
    return;
  if ((int32_t)(now - d.nextRepeatAt) >= 0) {
    g_onAction(action);
    d.nextRepeatAt = now + REPEAT_MS;
  }
}

// Clock que o Pico-PIO-USB realmente usa (clk_sys, fixado em 240 MHz pelo
// preinit do DVHSTX -- display::begin() reprograma o pll_sys/clk_hstx do
// vídeo, não o clk_sys; ver o comentário grande em UsbHost.h). Só imprime o
// diagnóstico -- nunca trava o boot: a pilha PIO-USB não tem essa exigência
// como um assert, é uma prática recomendada dos exemplos da própria Adafruit
// para full/low speed ficarem dentro da tolerância do padrão USB.
void logClockFit() {
  const uint32_t hz = clock_get_hz(clk_sys);
  const bool multipleOf12MHz = (hz % 12000000UL) == 0;
  Serial.printf("[USB] clk_sys=%lu Hz (%s multiplo de 12 MHz -- ver fj/UsbHost.h)\n", (unsigned long)hz,
                multipleOf12MHz ? "e" : "NAO e");
}

void rp2040ConfigurePioUsb() {
  pinMode(board::USB_HOST_5V_EN, OUTPUT);
  digitalWrite(board::USB_HOST_5V_EN, board::USB_HOST_5V_EN_STATE);

  pio_usb_configuration_t cfg = PIO_USB_DEFAULT_CONFIG;
  cfg.pin_dp = board::USB_HOST_DP;
  cfg.pio_tx_num = USB_PIO_INDEX;
  cfg.pio_rx_num = USB_PIO_INDEX;
  USBHost.configure_pio_usb(1, &cfg);
}

void usbHostTask(void *) {
  // Roda no núcleo 1 (xTaskCreatePinnedToCore abaixo): é aqui, e não em
  // appSetup(), que o alarme de 1 ms do PIO-USB fica de fato instalado --
  // pio_usb_host_init roda dentro de USBHost.begin(), chamado por ESTA
  // tarefa, então a interrupção do alarme herda o núcleo dela (ver
  // UsbHost.h).
  rp2040ConfigurePioUsb();
  logClockFit();
  USBHost.begin(1);
  Serial.println("[USB] host USB no ar (PIO2, nucleo 1)");
  for (;;) {
    USBHost.task(); // nao bloqueia de verdade (osal_pico ignora o timeout)
    const uint32_t now = millis();
    for (auto &d : g_devices)
      if (d.used && d.kind != Kind::NONE)
        serviceRepeat(d, now);
    vTaskDelay(pdMS_TO_TICKS(4));
  }
}

} // namespace

void begin(ActionHandler onAction) {
  g_onAction = onAction;
  xTaskCreatePinnedToCore(usbHostTask, "USB_HOST", TASK_STACK_BYTES, nullptr, TASK_PRIORITY, nullptr, TASK_CORE);
}

uint8_t deviceCount() {
  uint8_t n = 0;
  for (auto &d : g_devices)
    if (d.used)
      ++n;
  return n;
}

DeviceLine describe(uint8_t idx) {
  DeviceLine line{};
  uint8_t seen = 0;
  for (auto &d : g_devices) {
    if (!d.used)
      continue;
    if (seen++ != idx)
      continue;
    const char *tipo = d.kind == Kind::KEYBOARD          ? "teclado"
                       : d.kind == Kind::GENERIC_GAMEPAD  ? "gamepad generico"
                       : d.kind == Kind::DUALSHOCK4       ? "DualShock4"
                       : d.kind == Kind::DUALSENSE        ? "DualSense"
                                                           : "ignorado (nao reconhecido)";
    snprintf(line.text, sizeof(line.text), "%04X:%04X %s", d.vid, d.pid, tipo);
    return line;
  }
  snprintf(line.text, sizeof(line.text), "-");
  return line;
}

const char *lastEvent() { return g_lastEvent; }

} // namespace usbhost

// ---------------------------------------------------------------------------
// Callbacks do TinyUSB host. Extern "C": e assim que hid_host.h as declara.
// Chamadas de dentro de USBHost.task(), ou seja, na propria tarefa USB_HOST
// (nunca de uma IRQ) -- por isso podem mexer em usbhost::g_devices direto,
// sem seção crítica: quem mais lê essa tabela e a própria tarefa, no mesmo
// loop, um pouco depois.
// ---------------------------------------------------------------------------
extern "C" {

void tuh_mount_cb(uint8_t dev_addr) {
  uint16_t vid = 0, pid = 0;
  tuh_vid_pid_get(dev_addr, &vid, &pid);
  usbhost::setLastEvent("conectado %04X:%04X (endereco %u)", vid, pid, dev_addr);
  Serial.printf("[USB] %s\n", usbhost::lastEvent());
}

void tuh_umount_cb(uint8_t dev_addr) {
  usbhost::setLastEvent("desconectado (endereco %u)", dev_addr);
  Serial.printf("[USB] %s\n", usbhost::lastEvent());
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *desc_report, uint16_t desc_len) {
  using namespace usbhost;
  Device *d = allocDevice(dev_addr, instance);
  if (!d) {
    Serial.println("[USB] ERRO: tabela de dispositivos HID cheia, ignorando");
    return;
  }
  tuh_vid_pid_get(dev_addr, &d->vid, &d->pid);
  const uint8_t protocol = tuh_hid_interface_protocol(dev_addr, instance);

  if (protocol == HID_ITF_PROTOCOL_KEYBOARD) {
    d->kind = Kind::KEYBOARD;
  } else if (usbhidmap::isDualShock4(d->vid, d->pid)) {
    d->kind = Kind::DUALSHOCK4;
  } else if (usbhidmap::isDualSense(d->vid, d->pid)) {
    d->kind = Kind::DUALSENSE;
  } else if (desc_report && desc_len) {
    // Confere se é um gamepad de verdade (Joystick/Gamepad/Multi-axis na
    // Generic Desktop Page) antes de tentar decifrar o layout de bits -- um
    // relatório de mouse ou de um dispositivo de fabricante também "parseia"
    // sem erro, só que os deslocamentos não significam nada.
    tuh_hid_report_info_t reports[4];
    const uint8_t n = tuh_hid_parse_report_descriptor(reports, 4, desc_report, desc_len);
    bool isGamepad = false;
    for (uint8_t i = 0; i < n; ++i)
      if (reports[i].usage_page == 0x01 && (reports[i].usage == 0x04 || reports[i].usage == 0x05 ||
                                            reports[i].usage == 0x08))
        isGamepad = true;
    if (isGamepad) {
      d->layout = usbhidmap::parseGamepadFieldLayout(desc_report, desc_len);
      d->kind = d->layout.valid ? Kind::GENERIC_GAMEPAD : Kind::NONE;
    }
  }

  if (d->kind == Kind::NONE) {
    setLastEvent("%04X:%04X ignorado (nao e teclado nem gamepad reconhecido)", d->vid, d->pid);
    Serial.printf("[USB] %s\n", lastEvent());
    d->used = false; // devolve a vaga: nao vamos pedir relatorio deste
    return;
  }
  setLastEvent("%04X:%04X montado (instancia %u)", d->vid, d->pid, instance);
  Serial.printf("[USB] %s\n", lastEvent());
  if (!tuh_hid_receive_report(dev_addr, instance))
    Serial.println("[USB] ERRO: tuh_hid_receive_report falhou no mount");
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
  using namespace usbhost;
  if (Device *d = findDevice(dev_addr, instance)) {
    setLastEvent("%04X:%04X desmontado", d->vid, d->pid);
    Serial.printf("[USB] %s\n", lastEvent());
    *d = Device{};
  }
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len) {
  using namespace usbhost;
  Device *d = findDevice(dev_addr, instance);
  if (d) {
    switch (d->kind) {
    case Kind::KEYBOARD:
      if (len >= 8)
        d->state = usbhidmap::fromKeyboardBootReport(report);
      break;
    case Kind::GENERIC_GAMEPAD: d->state = usbhidmap::fromGeneric(d->layout, report, len); break;
    case Kind::DUALSHOCK4: d->state = usbhidmap::fromDualShock4(report, len); break;
    case Kind::DUALSENSE: d->state = usbhidmap::fromDualSense(report, len); break;
    case Kind::NONE: break;
    }
  }
  // Sempre repede o pedido, mesmo sem achar o dispositivo (nao deveria
  // acontecer, mas silenciar o endpoint de vez seria pior que um relatorio
  // perdido).
  if (!tuh_hid_receive_report(dev_addr, instance))
    Serial.println("[USB] ERRO: tuh_hid_receive_report falhou apos relatorio");
}

} // extern "C"
