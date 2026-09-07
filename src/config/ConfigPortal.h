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
  static constexpr uint8_t kChannel = 1;
  static constexpr uint8_t kMaxClients = 4;
  static constexpr uint16_t kHttpPort = 80;

  explicit ConfigPortal(Settings& settings);

  // Reads saved settings from NVS. Called from setup() before any radio
  // starts: the device boots into BLE mode, where begin() never runs, so
  // loading inside begin() meant a BLE-mode boot silently used defaults.
  void loadSettings();

  // Starts the AP (SSID = settings.deviceName), DNS and HTTP server. Returns
  // false only when the AP itself could not start — a storage failure is
  // survivable and is reported in the log.
  bool begin();

  // Call from loop().
  void tick(uint32_t nowMs);

  // Tears down HTTP, DNS and the access point. Safe to call when never begun,
  // and safe to call twice.
  void end();

  DeviceStatus status() const;

  // True once a save has actually changed settings.deviceName. main.cpp polls
  // this from loop(), restarts the active radio with the new name, and calls
  // clearRenamePending() — never acts inside the request handler itself.
  bool renamePending() const { return renamePending_; }
  void clearRenamePending() { renamePending_ = false; }

  // millis() timestamp of the moment renamePending() became true. main.cpp
  // waits a short flush delay past this before tearing anything down, so the
  // async web server has had a chance to actually put the response on the
  // wire (returning from the handler only means the response was queued).
  uint32_t renameFlaggedAtMs() const { return renameFlaggedAtMs_; }

  // Called by WebUi once a save changes settings.deviceName. Not for main.cpp.
  void flagRenamePending(uint32_t nowMs) {
    renamePending_ = true;
    renameFlaggedAtMs_ = nowMs;
  }

 private:
  Settings& settings_;
  NvsStore store_;
  ApManager ap_;
  CaptivePortal portal_;
  AsyncWebServer server_;
  WebUi web_;
  bool renamePending_ = false;
  uint32_t renameFlaggedAtMs_ = 0;
};
