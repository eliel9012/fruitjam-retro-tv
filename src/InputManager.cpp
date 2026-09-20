#include <M5Unified.h>
#include "InputManager.h"
#include "UiLogic.h"

void InputManager::begin() {
  _queued = NavAction::NONE;
  _heldZone = _highlightedButton = -1;
  _down = _longSent = _homeSent = _sideLongSent = _repeatSent = false;
}
void InputManager::queue(NavAction action, int8_t button) {
  if (_queued == NavAction::NONE)
    _queued = action;
  if (button >= 0) {
    _highlightedButton = button;
    _highlightUntil = millis() + HIGHLIGHT_MS;
  }
}
void InputManager::highlightButton(int8_t button) {
  if (button >= 0 && button < 3) {
    _highlightedButton = button;
    _highlightUntil = millis() + HIGHLIGHT_MS;
  }
}
void InputManager::inject(NavAction action, InputSource source) {
  (void)source;
  int8_t button =
      (action == NavAction::LEFT || action == NavAction::PREVIOUS || action == NavAction::SEEK_BACKWARD) ? 0
      : (action == NavAction::RIGHT || action == NavAction::NEXT || action == NavAction::SEEK_FORWARD)   ? 2
                                                                                                         : 1;
  queue(action, button);
}
NavAction InputManager::getAction() {
  NavAction action = _queued;
  _queued = NavAction::NONE;
  return action;
}

void InputManager::update() {
  const uint32_t now = millis();
  // BtnA/B/C are M5Unified's Core2 lower capacitive controls; LCD touch is handled separately.
  const bool pressed[3] = {M5.BtnA.isPressed(), M5.BtnB.isPressed(), M5.BtnC.isPressed()};
  int8_t zone = pressed[0] ? 0 : pressed[1] ? 1 : pressed[2] ? 2 : -1;
  if (zone < 0) {
    if (_down) {
      uint32_t elapsed = now - _downAt;
      if (_heldZone == 1)
        queue((_homeSent || elapsed >= HOME_MS)   ? NavAction::HOME
              : (_longSent || elapsed >= LONG_MS) ? NavAction::BACK
                                                  : NavAction::SELECT,
              1);
      else if (!_sideLongSent && !_repeatSent)
        queue(_heldZone == 0 ? NavAction::LEFT : NavAction::RIGHT, _heldZone);
    }
    _down = _longSent = _homeSent = _sideLongSent = _repeatSent = false;
    _heldZone = -1;
    return;
  }
  if (!_down) {
    _down = true;
    _heldZone = zone;
    _downAt = now;
    _nextRepeatAt = now + REPEAT_DELAY_MS;
    _longSent = _homeSent = _sideLongSent = _repeatSent = false;
    highlightButton(zone);
    return;
  }
  if (zone != _heldZone)
    return;
  uint32_t elapsed = now - _downAt;
  if (_heldZone == 1) {
    if (!_homeSent && elapsed >= HOME_MS)
      _homeSent = true;
    else if (!_longSent && elapsed >= LONG_MS)
      _longSent = true;
    return;
  }
  if (!_repeatEnabled && !_sideLongSent && elapsed >= LONG_MS) {
    _sideLongSent = true;
    queue(_heldZone == 0 ? NavAction::PREVIOUS : NavAction::NEXT, _heldZone);
  } else if (_repeatEnabled && !_sideLongSent && timeReached(now, _nextRepeatAt)) {
    _repeatSent = true;
    queue(_heldZone == 0 ? NavAction::LEFT : NavAction::RIGHT, _heldZone);
    _nextRepeatAt = now + REPEAT_MS;
  }
}