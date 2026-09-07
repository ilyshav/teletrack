#pragma once

#include <DNSServer.h>
#include <IPAddress.h>

class AsyncWebServer;

// DNS hijack plus the OS connectivity-probe routes that make a phone raise its
// "Sign in to network" sheet.
class CaptivePortal {
 public:
  static constexpr uint16_t kDnsPort = 53;

  bool begin(const IPAddress& ip);

  // Call from loop(). Answers one pending DNS query.
  void tick();

  // Registers the probe routes and the catch-all. Call before server.begin().
  void registerRoutes(AsyncWebServer& server);

  // Stops answering DNS. Safe to call when never begun.
  void end();

 private:
  DNSServer dns_;
  bool up_ = false;
};
