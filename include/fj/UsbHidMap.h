#pragma once
// Mapeamento HID -> NavAction, em funcoes puras (sem TinyUSB, sem Arduino além
// do que InputManager.h já puxa) para poderem ser testadas no host
// (tests/test_core.cpp). A parte que fala com o TinyUSB de verdade fica em
// fj/UsbHost.h/.cpp; este header só decide "o que os bytes do relatório HID
// significam em termos de navegação".
//
// Um relatório HID pode ligar várias direções e botões ao mesmo tempo (d-pad
// diagonal, ou o dedo ainda no botão anterior). Como o InputManager só aceita
// uma NavAction por vez -- a mesma regra dos três botões físicos --,
// `primaryAction()` escolhe UMA, por prioridade fixa: navegação primeiro
// (LEFT/RIGHT/UP/DOWN), depois os atalhos. Isso evita o mesmo relatório virar
// dois eventos (ex.: HOME e SELECT juntos porque os dois bits vieram no mesmo
// pacote).
#include "InputManager.h" // NavAction
#include <stdint.h>

namespace usbhidmap {

// Estado logico "achatado": o que o dispositivo esta pedindo agora, decodificado
// do relatorio bruto. Um so filho por leitura vira NavAction (primaryAction).
struct LogicalButtons {
  bool up = false, down = false, left = false, right = false;
  bool select = false;    // A / Cruz / Enter
  bool back = false;      // B / Circulo / Esc ou Backspace
  bool home = false;      // Select+Start, PS/Guide, tecla Home
  bool previous = false;  // L1/L, PgUp
  bool next = false;      // R1/R, PgDn
  bool playPause = false; // Start, Espaco, Play/Pause de midia
};

inline bool anyPressed(const LogicalButtons &b) {
  return b.up || b.down || b.left || b.right || b.select || b.back || b.home || b.previous ||
         b.next || b.playPause;
}

// Prioridade fixa, para nunca gerar duas acoes do mesmo relatorio. Navegacao
// primeiro: em um d-pad ruim (ou teclado com tecla presa) mais de um bit pode
// vir junto, e a intencao mais provavel de quem esta navegando e a direcao.
inline NavAction primaryAction(const LogicalButtons &b) {
  if (b.left)
    return NavAction::LEFT;
  if (b.right)
    return NavAction::RIGHT;
  if (b.up)
    return NavAction::UP;
  if (b.down)
    return NavAction::DOWN;
  if (b.home)
    return NavAction::HOME;
  if (b.back)
    return NavAction::BACK;
  if (b.select)
    return NavAction::SELECT;
  if (b.previous)
    return NavAction::PREVIOUS;
  if (b.next)
    return NavAction::NEXT;
  if (b.playPause)
    return NavAction::PLAY_PAUSE;
  return NavAction::NONE;
}

// ---------------------------------------------------------------------------
// Teclado -- protocolo boot HID (fixo, 8 bytes: modifier, reservado, 6 keycodes)
// ---------------------------------------------------------------------------
// Usa so os keycodes padrao da tabela HID "Keyboard/Keypad Page" (0x07); nao
// depende de layout de teclado (a tecla fisica de seta e sempre 0x4F..0x52,
// independente de QWERTY/ABNT2/etc).
namespace hidkey {
constexpr uint8_t ENTER = 0x28;
constexpr uint8_t ESC = 0x29;
constexpr uint8_t BACKSPACE = 0x2A;
constexpr uint8_t SPACE = 0x2C;
constexpr uint8_t PAGE_UP = 0x4B;
constexpr uint8_t PAGE_DOWN = 0x4E;
constexpr uint8_t RIGHT = 0x4F;
constexpr uint8_t LEFT = 0x50;
constexpr uint8_t DOWN = 0x51;
constexpr uint8_t UP = 0x52;
constexpr uint8_t HOME_KEY = 0x4A;
// Teclas de midia do "Consumer Page" (0x0C) chegam num relatorio A PARTE, nao
// no boot de teclado; PLAY_PAUSE por essa via fica pendente (ver AGENTS.md).
} // namespace hidkey

inline LogicalButtons fromKeyboardBootReport(const uint8_t report[8]) {
  LogicalButtons b;
  for (int i = 2; i < 8; ++i) {
    switch (report[i]) {
    case hidkey::LEFT: b.left = true; break;
    case hidkey::RIGHT: b.right = true; break;
    case hidkey::UP: b.up = true; break;
    case hidkey::DOWN: b.down = true; break;
    case hidkey::ENTER: b.select = true; break;
    case hidkey::ESC:
    case hidkey::BACKSPACE: b.back = true; break;
    case hidkey::SPACE: b.playPause = true; break;
    case hidkey::HOME_KEY: b.home = true; break;
    case hidkey::PAGE_UP: b.previous = true; break;
    case hidkey::PAGE_DOWN: b.next = true; break;
    default: break;
    }
  }
  return b;
}

// ---------------------------------------------------------------------------
// Gamepad HID generico -- layout descoberto do report descriptor
// ---------------------------------------------------------------------------
// Cobre o caso comum (um so Report ID, ou nenhum; botoes como Usage Minimum/
// Maximum na Button Page; X/Y e opcionalmente um Hat Switch na Generic Desktop
// Page). Um descritor com mais de um Report ID relevante, ou botoes fora da
// Button Page, sai do escopo -- fica so sem d-pad/analogico (o dispositivo nao
// e reconhecido como gamepad e e ignorado por quem chama).
struct GamepadFieldLayout {
  bool valid = false;
  uint8_t reportId = 0; // 0 = sem byte de Report ID no relatorio
  uint32_t buttonBitOffset = 0;
  uint32_t buttonCount = 0;
  bool hasHat = false;
  uint32_t hatBitOffset = 0, hatBitSize = 0;
  int32_t hatLogicalMin = 0, hatLogicalMax = 0;
  bool hasAxisX = false, hasAxisY = false;
  uint32_t axisXBitOffset = 0, axisXBitSize = 0;
  uint32_t axisYBitOffset = 0, axisYBitSize = 0;
  int32_t axisLogicalMin = 0, axisLogicalMax = 0;
};

namespace detail {
// Um passo do parser HID short-item (Device Class Definition for HID 1.11,
// 6.2.2). So itens curtos (1-4 bytes); descritores com itens longos (raros,
// so em relatorios de fabricante) terminam o parse ali -- o layout fica
// parcial, e o chamador confere `valid`.
struct ItemReader {
  const uint8_t *data;
  uint16_t len;
  uint16_t pos = 0;
  bool next(uint8_t &tag, uint8_t &type, uint32_t &value) {
    if (pos >= len)
      return false;
    const uint8_t prefix = data[pos++];
    if (prefix == 0xFE) // long item: 0xFE, size, tag, dados -- pulamos
      return false;
    const uint8_t sizeCode = prefix & 0x3;
    const uint8_t size = sizeCode == 3 ? 4 : sizeCode;
    tag = (prefix >> 4) & 0xF;
    type = (prefix >> 2) & 0x3;
    value = 0;
    for (uint8_t i = 0; i < size; ++i) {
      if (pos >= len)
        return false;
      value |= (uint32_t)data[pos++] << (8 * i);
    }
    // Campos de 1 ou 2 bytes sao assinados nos itens Global/Local que usam
    // faixa (Logical Minimum etc.); quem le decide se estende o sinal.
    return true;
  }
};
inline int32_t signExtend(uint32_t value, uint8_t byteCount) {
  if (byteCount == 1)
    return (int8_t)value;
  if (byteCount == 2)
    return (int16_t)value;
  return (int32_t)value;
}
} // namespace detail

inline GamepadFieldLayout parseGamepadFieldLayout(const uint8_t *desc, uint16_t len) {
  using namespace detail;
  GamepadFieldLayout out;
  ItemReader r{desc, len};
  uint16_t usagePage = 0;
  int32_t logicalMin = 0, logicalMax = 0;
  uint32_t reportSize = 0, reportCount = 0;
  uint8_t reportId = 0;
  uint32_t bitPos = 0;
  // Usages locais acumulados desde o ultimo item Main (limpos depois dele).
  uint16_t usageList[16];
  uint8_t usageCount = 0;
  bool haveUsageRange = false;
  uint16_t usageRangeMin = 0, usageRangeMax = 0;
  uint8_t tag, type;
  uint32_t raw;
  // O prefixo de 1 byte de cada item so cabe valor de 0/1/2/4 bytes: para
  // Logical Minimum/Maximum, que podem ser negativos, o tamanho do item conta.
  uint8_t lastItemSize = 0;
  while (true) {
    const uint16_t before = r.pos;
    if (!r.next(tag, type, raw))
      break;
    lastItemSize = (uint8_t)(r.pos - before - 1);
    if (type == 1) { // Global
      switch (tag) {
      case 0x0: usagePage = (uint16_t)raw; break;
      case 0x1: logicalMin = signExtend(raw, lastItemSize); break;
      case 0x2: logicalMax = signExtend(raw, lastItemSize); break;
      case 0x7: reportSize = raw; break;
      case 0x8:
        reportId = (uint8_t)raw;
        // Relatorios com ID reservam o primeiro byte para ele; o cursor de
        // bits de QUALQUER relatorio com ID comeca depois desse byte. Como
        // este parser so segue um layout (o primeiro Report ID que aparece),
        // isto e exato para descritores de um relatorio so.
        bitPos = raw ? 8 : 0;
        break;
      case 0x9: reportCount = raw; break;
      default: break;
      }
    } else if (type == 2) { // Local
      switch (tag) {
      case 0x0:
        if (usageCount < sizeof(usageList) / sizeof(usageList[0]))
          usageList[usageCount++] = (uint16_t)raw;
        break;
      case 0x1: usageRangeMin = (uint16_t)raw; haveUsageRange = true; break;
      case 0x2: usageRangeMax = (uint16_t)raw; haveUsageRange = true; break;
      default: break;
      }
    } else if (type == 0) { // Main
      if (tag == 0x8) {     // Input
        const uint32_t fieldBits = reportSize * reportCount;
        const bool isButtonPage = usagePage == 0x09;
        const bool isDesktopPage = usagePage == 0x01;
        if (isButtonPage && haveUsageRange && reportSize == 1) {
          out.buttonBitOffset = bitPos;
          out.buttonCount = (usageRangeMax >= usageRangeMin) ? (usageRangeMax - usageRangeMin + 1) : reportCount;
          out.valid = true;
        } else if (isDesktopPage) {
          for (uint8_t i = 0; i < usageCount; ++i) {
            const uint32_t off = bitPos + (uint32_t)i * reportSize;
            if (usageList[i] == 0x30 && reportCount > i) { // X
              out.hasAxisX = true;
              out.axisXBitOffset = off;
              out.axisXBitSize = reportSize;
              out.axisLogicalMin = logicalMin;
              out.axisLogicalMax = logicalMax;
            } else if (usageList[i] == 0x31 && reportCount > i) { // Y
              out.hasAxisY = true;
              out.axisYBitOffset = off;
              out.axisYBitSize = reportSize;
            } else if (usageList[i] == 0x39) { // Hat Switch
              out.hasHat = true;
              out.hatBitOffset = off;
              out.hatBitSize = reportSize;
              out.hatLogicalMin = logicalMin;
              out.hatLogicalMax = logicalMax;
            }
          }
        }
        bitPos += fieldBits;
      } else if (tag == 0xA || tag == 0xC) {
        // Collection / End Collection: nao rastreamos profundidade -- o
        // layout de bits e linear no relatorio, independente de aninhamento.
      }
      // Todo item Main limpa os locals (secao 6.2.2.8 do HID 1.11).
      usageCount = 0;
      haveUsageRange = false;
    }
  }
  out.reportId = reportId;
  return out;
}

// Le um campo LSB-first, bit a bit, de um relatorio HID (padrao da spec: o
// primeiro campo ocupa os bits menos significativos do primeiro byte). Fora
// dos limites do buffer devolve 0 em vez de ler lixo.
inline uint32_t readBits(const uint8_t *buf, uint32_t len, uint32_t bitOffset, uint32_t bitCount) {
  if (bitCount == 0 || bitCount > 32)
    return 0;
  uint32_t value = 0;
  for (uint32_t i = 0; i < bitCount; ++i) {
    const uint32_t bit = bitOffset + i;
    const uint32_t byteIdx = bit / 8;
    if (byteIdx >= len)
      break;
    const uint8_t bitVal = (buf[byteIdx] >> (bit % 8)) & 1;
    value |= (uint32_t)bitVal << i;
  }
  return value;
}

// Direcao decodificada de um Hat Switch: os 8 valores (min..min+7) sao N, NE,
// E, SE, S, SW, W, NW em sequencia horaria (HID 1.11, exemplo do Hat Switch).
// Qualquer valor fora dessa faixa (o "nono" valor comum de descritores com
// nibble, min=0 max=7 e null=8) e centro.
inline void hatToDirection(int32_t value, int32_t logicalMin, int32_t logicalMax, bool &up, bool &down,
                           bool &left, bool &right) {
  up = down = left = right = false;
  const int32_t count = logicalMax - logicalMin + 1;
  if (count < 8)
    return; // descritor fora do padrao; sem direcao em vez de adivinhar
  const int32_t dir = value - logicalMin;
  if (dir < 0 || dir >= count)
    return; // valor "null" (centro)
  up = dir == 0 || dir == 1 || dir == 7;
  right = dir == 1 || dir == 2 || dir == 3;
  down = dir == 3 || dir == 4 || dir == 5;
  left = dir == 5 || dir == 6 || dir == 7;
}

// Decodifica um relatorio generico usando o layout achado por
// parseGamepadFieldLayout. `stickDeadzonePct` e a fracao (0..100) da meia-faixa
// do eixo que conta como "centro" -- necessario porque um analogico nunca fica
// exatamente no meio.
//
// Mapeamento dos botoes: NAO ha como saber, so pelo descritor, qual bit e "A" e
// qual e "Start" num pad generico -- cada fabricante numera do seu jeito. Isto
// usa a convencao mais comum entre os gamepads USB baratos tipo SNES (checada
// contra o layout de alguns modelos comuns, NAO o dispositivo real): bit 0 =
// confirmar, bit 1 = voltar, bits 4/5 = ombro esquerdo/direito
// (anterior/proximo), bit 6 = Select/Back (inicio), bit 7 = Start
// (play/pause). Um pad com outra numeracao ainda navega pelo d-pad/analogico;
// so os atalhos de botao saem trocados. NAO TESTADO com um gamepad real.
inline LogicalButtons fromGeneric(const GamepadFieldLayout &layout, const uint8_t *report, uint32_t len,
                                  int stickDeadzonePct = 35) {
  LogicalButtons b;
  if (!layout.valid)
    return b;
  const uint32_t buttons = readBits(report, len, layout.buttonBitOffset, layout.buttonCount);
  b.select = buttons & (1u << 0);
  b.back = buttons & (1u << 1);
  if (layout.buttonCount > 4) {
    b.previous = buttons & (1u << 4);
    b.next = buttons & (1u << 5);
  }
  if (layout.buttonCount > 6)
    b.home = buttons & (1u << 6);
  if (layout.buttonCount > 7)
    b.playPause = buttons & (1u << 7);
  bool hatUp = false, hatDown = false, hatLeft = false, hatRight = false;
  bool hatCentered = true;
  if (layout.hasHat) {
    const uint32_t raw = readBits(report, len, layout.hatBitOffset, layout.hatBitSize);
    hatToDirection((int32_t)raw, layout.hatLogicalMin, layout.hatLogicalMax, hatUp, hatDown, hatLeft, hatRight);
    hatCentered = !(hatUp || hatDown || hatLeft || hatRight);
  }
  if (!hatCentered) {
    b.up = hatUp;
    b.down = hatDown;
    b.left = hatLeft;
    b.right = hatRight;
  } else if (layout.hasAxisX || layout.hasAxisY) {
    const int32_t mid = (layout.axisLogicalMin + layout.axisLogicalMax) / 2;
    const int32_t half = (layout.axisLogicalMax - layout.axisLogicalMin) / 2;
    const int32_t deadzone = half * stickDeadzonePct / 100;
    if (layout.hasAxisX) {
      const int32_t x = (int32_t)readBits(report, len, layout.axisXBitOffset, layout.axisXBitSize) - mid;
      b.left = x < -deadzone;
      b.right = x > deadzone;
    }
    if (layout.hasAxisY) {
      const int32_t y = (int32_t)readBits(report, len, layout.axisYBitOffset, layout.axisYBitSize) - mid;
      b.up = y < -deadzone;
      b.down = y > deadzone;
    }
  }
  return b;
}

// ---------------------------------------------------------------------------
// DualShock 4 e DualSense -- layout fixo (documentado publicamente pela
// comunidade -- hid-sony do kernel Linux, SDL_GameControllerDB), por VID/PID.
// Nao usa o report descriptor: os dois controles reportam corretamente por ele
// (sao HID "de verdade"), mas o layout fixo e mais simples e não depende do
// parser generico acima aceitar descritores maiores que os de um gamepad
// generico (os dois têm giroscópio, acelerômetro, touchpad e luz no mesmo
// relatorio).
// ---------------------------------------------------------------------------
constexpr uint16_t VID_SONY = 0x054C;
constexpr uint16_t PID_DUALSHOCK4_V1 = 0x05C4;
constexpr uint16_t PID_DUALSHOCK4_V2 = 0x09CC;
constexpr uint16_t PID_DUALSENSE = 0x0CE6;

inline bool isDualShock4(uint16_t vid, uint16_t pid) {
  return vid == VID_SONY && (pid == PID_DUALSHOCK4_V1 || pid == PID_DUALSHOCK4_V2);
}
inline bool isDualSense(uint16_t vid, uint16_t pid) { return vid == VID_SONY && pid == PID_DUALSENSE; }

// Layout comum aos dois: LX,LY,RX,RY nos primeiros 4 bytes, hat (d-pad) no
// nibble baixo do byte 4 (0..7 sentido horario a partir do N, 8 = centro),
// Quadrado/Cruz/Circulo/Triangulo no nibble alto, depois L1/R1/L2/R2/
// Share/Options/L3/R3 e por fim PS/touchpad. NAO TESTADO com o controle real:
// se os botoes de face saírem trocados, o dpad honra o layout mesmo assim (o
// nibble baixo do byte 4 e o unico bit que os dois formatos concordam 100%).
inline LogicalButtons fromDualShock4(const uint8_t *report, uint32_t len) {
  LogicalButtons b;
  if (len < 6)
    return b;
  const uint8_t hat = report[4] & 0x0F;
  bool up, down, left, right;
  hatToDirection(hat, 0, 7, up, down, left, right);
  b.up = up;
  b.down = down;
  b.left = left;
  b.right = right;
  b.select = report[4] & 0x20;      // Cruz
  b.back = report[4] & 0x40;        // Circulo
  if (len > 5) {
    b.previous = report[5] & 0x01;  // L1
    b.next = report[5] & 0x02;      // R1
    b.home = report[5] & 0x10;      // Share
    b.playPause = report[5] & 0x20; // Options
  }
  if (len > 6)
    b.home = b.home || (report[6] & 0x01); // botao PS
  return b;
}

// DualSense manda o mesmo layout de eixos/d-pad/botoes do DS4 nos mesmos
// bytes (a Sony manteve compatibilidade de relatorio); só o resto do pacote
// (giroscópio, bateria, toque) muda de lugar. NAO TESTADO.
inline LogicalButtons fromDualSense(const uint8_t *report, uint32_t len) { return fromDualShock4(report, len); }

} // namespace usbhidmap
