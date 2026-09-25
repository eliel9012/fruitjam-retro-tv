#pragma once
// Stub do M5Unified do upstream (Core2). O port não usa mais o M5Unified; o
// stub fica enquanto houver código de teste que o inclua. Os botões leem o
// mesmo array do stub de fj/Board.h, então os dois caminhos concordam.
#include "fj/Board.h"

struct FakeButton {
  uint8_t index;
  bool isPressed() const { return board::fakeButtons[index]; }
};
struct FakeM5 {
  FakeButton BtnA{0}, BtnB{1}, BtnC{2};
};
inline FakeM5 M5;
