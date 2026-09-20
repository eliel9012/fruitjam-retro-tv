#pragma once
struct FakeButton {
  bool down = false;
  bool isPressed() const { return down; }
};
struct FakeM5 {
  FakeButton BtnA, BtnB, BtnC;
};
extern FakeM5 M5;
