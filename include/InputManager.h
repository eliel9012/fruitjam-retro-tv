#pragma once
#include <Arduino.h>

enum class NavAction : uint8_t {
  NONE,
  LEFT,
  RIGHT,
  UP,
  DOWN,
  SELECT,
  BACK,
  HOME,
  PREVIOUS,
  NEXT,
  PLAY_PAUSE,
  SEEK_BACKWARD,
  SEEK_FORWARD
};
// WEB: comando vindo do controle remoto pelo navegador. USB: teclado ou
// gamepad nas portas USB host (fj/UsbHost.h), decodificado para NavAction por
// fj/UsbHidMap.h. Os dois entram pela mesma fila dos botoes de proposito --
// assim nao existe um segundo caminho de navegacao capaz de divergir do
// fisico. (LCD_BUTTON e BOTTOM_TOUCH eram do touch do Core2; o Fruit Jam nao
// tem tela local.)
enum class InputSource : uint8_t { WEB, USB };

// Junta os tres botoes fisicos do Fruit Jam (board::buttonDown) e os comandos
// injetados numa fila de uma acao so.
//
//   A (esquerda)   toque = LEFT; segurar = PREVIOUS, ou auto-repeat de LEFT
//   B (centro)     toque = SELECT; segurar 0,7 s = BACK; segurar 1,5 s = HOME
//   C (direita)    espelho do A: RIGHT / NEXT
//   A + C juntos   segurar = HOME. Faz o papel do botao PWR do Core2 como
//                  atalho para o inicio (PORTING.md 3.3).
class InputManager {
public:
  void begin();
  void update();
  NavAction getAction();
  void inject(NavAction action, InputSource source);
  void highlightButton(int8_t button);
  void setAutoRepeat(bool enabled) { _repeatEnabled = enabled; }
  int8_t highlightedButton() const { return _highlightedButton; }
  bool highlightActive() const {
    return _highlightedButton >= 0 && (int32_t)(millis() - _highlightUntil) < 0;
  }
  // true enquanto algum botao estiver apertado. O desligamento suave usa isto
  // para so dormir depois que o dedo sair do botao que pediu o DESLIGAR.
  static bool anyDown();

private:
  static constexpr uint16_t LONG_MS = 700;
  static constexpr uint16_t HOME_MS = 1500;
  static constexpr uint16_t REPEAT_DELAY_MS = 400;
  static constexpr uint16_t REPEAT_MS = 120;
  static constexpr uint16_t HIGHLIGHT_MS = 150;
  // A+C precisa ficar apertado este tempo. Curto o bastante para parecer um
  // atalho, longo o bastante para dois toques quase simultaneos em A e C (dois
  // passos rapidos na lista) nao virarem "voltar ao inicio" por acidente.
  static constexpr uint16_t CHORD_MS = 400;
  NavAction _queued = NavAction::NONE;
  uint32_t _downAt = 0, _nextRepeatAt = 0, _highlightUntil = 0, _chordAt = 0;
  int8_t _heldZone = -1, _highlightedButton = -1;
  bool _down = false, _repeatEnabled = false, _longSent = false, _homeSent = false, _sideLongSent = false,
       _repeatSent = false, _chord = false, _chordSent = false;
  void queue(NavAction action, int8_t button = -1);
};
