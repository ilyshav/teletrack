#include "config/internal/ApManager.h"

#include <WiFi.h>
#include <stdio.h>

#include "core/Log.h"

bool ApManager::begin(const char* ssid, uint8_t channel, uint8_t maxClients) {
  snprintf(ssid_, sizeof(ssid_), "%s", ssid != nullptr ? ssid : "teletrack");

  WiFi.mode(WIFI_AP);

  const IPAddress ip(192, 168, 4, 1);
  const IPAddress gateway(192, 168, 4, 1);
  const IPAddress subnet(255, 255, 255, 0);
  if (!WiFi.softAPConfig(ip, gateway, subnet)) {
    Log::error("ap", "softAPConfig failed");
    up_ = false;
    return false;
  }

  // A null password is what makes this an open network.
  if (!WiFi.softAP(ssid_, nullptr, channel, /*ssid_hidden=*/0, maxClients)) {
    Log::error("ap", "softAP failed");
    up_ = false;
    return false;
  }

  // Built by hand rather than via IPAddress::toString(), which returns an
  // Arduino String.
  const IPAddress actual = WiFi.softAPIP();
  snprintf(ip_, sizeof(ip_), "%u.%u.%u.%u", actual[0], actual[1], actual[2], actual[3]);

  up_ = true;
  Log::info("ap", "up ssid=%s ch%u open", ssid_, (unsigned)channel);
  Log::info("ap", "ip %s", ip_);
  return true;
}

void ApManager::tick(uint32_t nowMs) {
  if (!up_) {
    return;
  }
  if (nowMs - lastPollMs_ < kPollIntervalMs) {
    return;
  }
  lastPollMs_ = nowMs;

  const uint8_t current = static_cast<uint8_t>(WiFi.softAPgetStationNum());
  if (current != clients_) {
    Log::info("ap", "clients %u -> %u", (unsigned)clients_, (unsigned)current);
    clients_ = current;
  }
}

bool ApManager::up() const { return up_; }
uint8_t ApManager::clients() const { return clients_; }
const char* ApManager::ssid() const { return ssid_; }
const char* ApManager::ip() const { return ip_; }
