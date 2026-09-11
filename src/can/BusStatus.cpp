#include "can/BusStatus.h"

BusStatus busStatus(bool controllerRunning, uint32_t framesInWindow,
                    uint32_t lostInWindow) {
  if (!controllerRunning) {
    return BusStatus::Starting;
  }
  if (framesInWindow == 0 && lostInWindow == 0) {
    return BusStatus::Silent;
  }
  if (lostInWindow > 0) {
    return BusStatus::Lossy;
  }
  return BusStatus::Healthy;
}
