#include "config/internal/CaptivePortal.h"

#include <ESPAsyncWebServer.h>

#include "core/Log.h"

namespace {

constexpr const char* kPortalUrl = "http://192.168.4.1/";

// Each OS probes a different URL to decide whether a network has internet.
// Answering all of them with a redirect — never a 204 — is what classifies the
// network as needing sign-in and pops the portal.
const char* const kProbePaths[] = {
    "/generate_204",             // Android
    "/gen_204",                  // Android
    "/hotspot-detect.html",      // iOS, macOS
    "/library/test/success.html", // iOS, macOS
    "/ncsi.txt",                 // Windows
    "/connecttest.txt",          // Windows
    "/redirect",                 // Windows
};

}  // namespace

bool CaptivePortal::begin(const IPAddress& ip) {
  dns_.setErrorReplyCode(DNSReplyCode::NoError);
  up_ = dns_.start(kDnsPort, "*", ip);
  if (up_) {
    Log::info("dns", "captive portal up on port %u", (unsigned)kDnsPort);
  } else {
    Log::error("dns", "failed to start on port %u", (unsigned)kDnsPort);
  }
  return up_;
}

void CaptivePortal::tick() {
  if (up_) {
    dns_.processNextRequest();
  }
}

void CaptivePortal::registerRoutes(AsyncWebServer& server) {
  for (const char* path : kProbePaths) {
    server.on(path, HTTP_GET, [](AsyncWebServerRequest* request) {
      request->redirect(kPortalUrl);
    });
  }
  server.onNotFound([](AsyncWebServerRequest* request) {
    if (request->method() == HTTP_OPTIONS) {
      request->send(200);
      return;
    }
    request->redirect(kPortalUrl);
  });
}

void CaptivePortal::end() {
  if (up_) {
    dns_.stop();
    up_ = false;
  }
}
