#ifndef PIO_UNIT_TESTING

#include <Arduino.h>

#include "config/ConfigPortal.h"
#include "config/Settings.h"
#include "core/Log.h"
#include "ui/Display.h"

namespace {

// The one global. Everything else is reached through it by reference.
struct App {
  Settings settings = Settings::defaults();
  Display display;
  ConfigPortal portal{settings};
};

App app;

}  // namespace

void setup() {
  Log::begin(115200);
  if (!app.display.begin()) {
    Log::error("tft", "display init failed, serial only");
  }

  Log::info("boot", "teletrack");

  if (!app.portal.begin()) {
    Log::error("boot", "config portal failed to start");
  }
}

void loop() {
  const uint32_t now = millis();
  app.portal.tick(now);
  app.display.tick(now, app.portal.status());
  delay(1);  // yield to the WiFi and AsyncTCP tasks
}

#endif  // PIO_UNIT_TESTING
