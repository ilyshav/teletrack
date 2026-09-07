#include "config/ConfigPortal.h"

#include <Arduino.h>
#include <WiFi.h>

#include "core/Log.h"

ConfigPortal::ConfigPortal(Settings& settings)
    : settings_(settings), server_(kHttpPort), web_(settings, store_, *this) {}

void ConfigPortal::loadSettings() {
  if (!store_.begin()) {
    Log::error("nvs", "storage unavailable, changes will not persist");
    return;
  }
  if (store_.load(settings_)) {
    Log::info("cfg", "loaded name=%s hz=%u", settings_.deviceName,
              (unsigned)settings_.sampleHz);
  } else {
    Log::info("cfg", "no stored settings, using defaults");
  }
}

bool ConfigPortal::begin() {
  if (!ap_.begin(settings_.deviceName, kChannel, kMaxClients)) {
    return false;
  }

  portal_.begin(IPAddress(192, 168, 4, 1));
  web_.registerRoutes(server_);
  portal_.registerRoutes(server_);
  server_.begin();
  Log::info("http", "listening on port %u", (unsigned)kHttpPort);
  return true;
}

void ConfigPortal::tick(uint32_t nowMs) {
  portal_.tick();
  ap_.tick(nowMs);
}

DeviceStatus ConfigPortal::status() const {
  DeviceStatus s;
  snprintf(s.ssid, sizeof(s.ssid), "%s", ap_.ssid());
  snprintf(s.ip, sizeof(s.ip), "%s", ap_.ip());
  s.clients = ap_.clients();
  s.uptimeMs = millis();
  s.freeHeap = ESP.getFreeHeap();
  s.apUp = ap_.up();
  return s;
}

void ConfigPortal::end() {
  server_.end();
  portal_.end();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  Log::info("ap", "stopped");
}
