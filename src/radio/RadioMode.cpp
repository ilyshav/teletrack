#include "radio/RadioMode.h"

bool ModeController::handle(ModeEvent event) {
  if (pending_) {
    return false;
  }
  switch (event) {
    case ModeEvent::ButtonHeld:
      mode_ = (mode_ == RadioMode::Ble) ? RadioMode::Wifi : RadioMode::Ble;
      pending_ = true;
      return true;
  }
  return false;
}
