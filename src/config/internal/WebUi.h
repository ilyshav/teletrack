#pragma once

#include <ESPAsyncWebServer.h>
#include <stddef.h>

#include "config/Settings.h"
#include "config/SettingsStore.h"
#include "config/internal/ConfigApi.h"

// Forward-declared, not included: ConfigPortal.h includes this header for its
// WebUi member, so including it back would be circular. WebUi.cpp includes it.
class ConfigPortal;

// HTTP plumbing. Every decision it makes is delegated: ConfigApi for
// serialisation, ConfigService for saves. If logic starts accumulating here,
// it belongs in one of those instead.
class WebUi {
 public:
  // Non-const: a successful rename flags the pending restart on portal, which
  // status() (used read-only elsewhere) does not need to know about.
  WebUi(Settings& settings, SettingsStore& store, ConfigPortal& portal);

  // Call before AsyncWebServer::begin().
  void registerRoutes(AsyncWebServer& server);

 private:
  void collectBody(AsyncWebServerRequest* request, uint8_t* data, size_t len,
                   size_t index, size_t total);
  void handleSave(AsyncWebServerRequest* request);

  Settings& settings_;
  SettingsStore& store_;
  ConfigPortal& portal_;

  // POST body accumulation. One buffer is enough: the AsyncTCP task delivers
  // request bodies one at a time, and owner_ guards against a completion
  // handler reading a body that belonged to a different request.
  AsyncWebServerRequest* owner_ = nullptr;
  char body_[ConfigApi::kMaxBodyBytes + 1] = {};
  size_t bodyLen_ = 0;
  bool bodyOverflow_ = false;
};
