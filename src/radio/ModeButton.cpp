#include "radio/ModeButton.h"

#include <Arduino.h>

void ModeButton::begin() { pinMode(kPin, INPUT_PULLUP); }

bool ModeButton::tick(uint32_t nowMs) {
  const bool pressed = digitalRead(kPin) == LOW;  // pull-up: LOW means pressed
  return detector_.update(pressed, nowMs);
}
