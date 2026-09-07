#pragma once

#include <ESPAsyncWebServer.h>
#include <stdint.h>

#include "config/Settings.h"
#include "config/SettingsStore.h"
#include "config/internal/ApManager.h"
#include "config/internal/CaptivePortal.h"
#include "config/internal/NvsStore.h"
#include "config/internal/WebUi.h"
#include "core/DeviceStatus.h"

// The configuration module's whole public surface, besides Settings.
//
// Owns the store, the AP, the DNS hijack and the HTTP server. main.cpp only
// ever calls begin(), tick() and status().
class ConfigPortal {
 public:
  static constexpr const char* kSsid = "teletrack";
  static constexpr uint8_t kChannel = 1;
  static constexpr uint8_t kMaxClients = 4;
  static constexpr uint16_t kHttpPort = 80;

  explicit ConfigPortal(Settings& settings);

  // Loads persisted settings, starts the AP, DNS and HTTP server. Returns false
  // only when the AP itself could not start — a storage failure is survivable
  // and is reported in the log.
  bool begin();

  // Call from loop().
  void tick(uint32_t nowMs);

  DeviceStatus status() const;

 private:
  Settings& settings_;
  NvsStore store_;
  ApManager ap_;
  CaptivePortal portal_;
  AsyncWebServer server_;
  WebUi web_;
};
