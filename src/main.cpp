#ifndef PIO_UNIT_TESTING

#include <Arduino.h>

#include "config/ConfigPortal.h"
#include "config/Settings.h"
#include "core/Log.h"

namespace {

// The one global. Everything else is reached through it by reference.
struct App {
  Settings settings = Settings::defaults();
  ConfigPortal portal{settings};
};

App app;

}  // namespace

void setup() {
  Log::begin(115200);
  Log::info("boot", "teletrack phase 1");

  if (!app.portal.begin()) {
    Log::error("boot", "config portal failed to start");
  }
}

void loop() {
  const uint32_t now = millis();
  app.portal.tick(now);
  delay(1);  // yield to the WiFi and AsyncTCP tasks
}

#endif  // PIO_UNIT_TESTING
