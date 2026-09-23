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
// WEB: comando vindo do controle remoto pelo navegador. Entra pela mesma fila
// dos botoes de proposito -- assim nao existe um segundo caminho de navegacao
// capaz de divergir do fisico.
enum class InputSource : uint8_t { LCD_BUTTON, BOTTOM_TOUCH, WEB };

// Merges Core2 BtnA/BtnB/BtnC and the LCD control bar into one action queue.
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

private:
  static constexpr uint16_t LONG_MS = 700;
  static constexpr uint16_t HOME_MS = 1500;
  static constexpr uint16_t REPEAT_DELAY_MS = 400;
  static constexpr uint16_t REPEAT_MS = 120;
  static constexpr uint16_t HIGHLIGHT_MS = 150;
  NavAction _queued = NavAction::NONE;
  uint32_t _downAt = 0, _nextRepeatAt = 0, _highlightUntil = 0;
  int8_t _heldZone = -1, _highlightedButton = -1;
  bool _down = false, _repeatEnabled = false, _longSent = false, _homeSent = false, _sideLongSent = false,
       _repeatSent = false;
  void queue(NavAction action, int8_t button = -1);
};