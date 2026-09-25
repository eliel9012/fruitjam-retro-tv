#pragma once
// Stub de include/fj/Board.h para os testes nativos. O de verdade depende dos
// pinos do variant do arduino-pico (PIN_BUTTON1...), que não existem no host.
//
// Os três botões moram num array só, que o stub do M5Unified também lê: assim
// o mesmo teste serve ao InputManager antigo (M5.BtnA/B/C) e ao do port
// (board::buttonDown), e não precisa mudar quando o InputManager mudar.
#include <stdint.h>

namespace board {

inline bool fakeButtons[3] = {false, false, false};
inline bool fakeCard = true;

inline void begin() {}
inline bool buttonDown(uint8_t index) { return index < 3 && fakeButtons[index]; }
inline bool cardInserted() { return fakeCard; }

} // namespace board
