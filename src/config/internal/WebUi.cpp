#include "config/internal/WebUi.h"

#include <string.h>

#include "config/ConfigPortal.h"
#include "config/internal/ConfigService.h"
#include "config/internal/ui_index.h"
#include "core/Log.h"

namespace {

void sendJson(AsyncWebServerRequest* request, uint16_t status, const char* body,
              size_t length) {
  AsyncWebServerResponse* response = request->beginResponse(
      status, "application/json", reinterpret_cast<const uint8_t*>(body), length);
  response->addHeader("Cache-Control", "no-store");
  request->send(response);
}

}  // namespace

WebUi::WebUi(Settings& settings, SettingsStore& store, const ConfigPortal& portal)
    : settings_(settings), store_(store), portal_(portal) {}

void WebUi::registerRoutes(AsyncWebServer& server) {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    AsyncWebServerResponse* response =
        request->beginResponse(200, "text/html", kUiIndexGz, kUiIndexGzLength);
    response->addHeader("Content-Encoding", "gzip");
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
    Log::info("http", "GET /");
  });

  server.on("/api/config", HTTP_GET, [this](AsyncWebServerRequest* request) {
    char body[ConfigApi::kJsonBufferSize];
    const size_t n = ConfigApi::toJson(settings_, body, sizeof(body));
    sendJson(request, 200, body, n);
    Log::info("http", "GET /api/config");
  });

  server.on("/api/status", HTTP_GET, [this](AsyncWebServerRequest* request) {
    const DeviceStatus snapshot = portal_.status();
    char body[ConfigApi::kJsonBufferSize];
    const size_t n = ConfigApi::statusToJson(snapshot, body, sizeof(body));
    sendJson(request, 200, body, n);
  });

  server.on(
      "/api/config", HTTP_POST,
      [this](AsyncWebServerRequest* request) { handleSave(request); },
      nullptr,
      [this](AsyncWebServerRequest* request, uint8_t* data, size_t len,
             size_t index, size_t total) {
        collectBody(request, data, len, index, total);
      });
}

void WebUi::collectBody(AsyncWebServerRequest* request, uint8_t* data, size_t len,
                        size_t index, size_t total) {
  if (index == 0) {
    owner_ = request;
    bodyLen_ = 0;
    bodyOverflow_ = total > ConfigApi::kMaxBodyBytes;
  }
  if (bodyOverflow_) {
    return;  // do not buffer what we are going to reject
  }
  if (bodyLen_ + len > ConfigApi::kMaxBodyBytes) {
    bodyOverflow_ = true;
    bodyLen_ = 0;
    return;
  }
  memcpy(body_ + bodyLen_, data, len);
  bodyLen_ += len;
  body_[bodyLen_] = '\0';
}

void WebUi::handleSave(AsyncWebServerRequest* request) {
  // A POST that never reached collectBody (no body at all) must not be judged
  // on whatever the previous request left behind.
  if (owner_ != request) {
    bodyLen_ = 0;
    bodyOverflow_ = false;
  }
  owner_ = nullptr;

  // An oversized body is signalled to ConfigService by a length past the cap,
  // which is exactly the condition its 413 branch tests for.
  const size_t effectiveLen =
      bodyOverflow_ ? ConfigApi::kMaxBodyBytes + 1 : bodyLen_;

  char response[ConfigApi::kJsonBufferSize];
  const ConfigService::SaveOutcome outcome = ConfigService::save(
      body_, effectiveLen, settings_, store_, response, sizeof(response));

  if (outcome.httpStatus == 200) {
    Log::info("cfg", "saved name=%s hz=%u", settings_.deviceName,
              (unsigned)settings_.sampleHz);
  } else {
    Log::warn("http", "POST /api/config %u", (unsigned)outcome.httpStatus);
  }

  sendJson(request, outcome.httpStatus, response, outcome.bodyLength);

  bodyLen_ = 0;
  bodyOverflow_ = false;
}
