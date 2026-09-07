#include "ble/BleLink.h"

#include <NimBLEDevice.h>

#include "core/Log.h"

namespace {

// Upper bound on packets notified per tick() call. Keeps loop() bounded even
// if the ring built up a backlog while disconnected; at the producer's GPS
// rate this comfortably drains within a few ticks.
constexpr size_t kMaxPacketsPerTick = 8;

NimBLEServer* g_server = nullptr;
NimBLECharacteristic* g_gpsMain = nullptr;
NimBLECharacteristic* g_gpsTime = nullptr;
bool g_connected = false;
uint16_t g_mtu = 23;

// Set by the callbacks, drained by tick(). The callbacks run on the NimBLE
// host task, and logging there puts a USB CDC write -- which can drop or
// stall -- in the middle of connection handling. Flag it and let loop() do
// the talking.
bool g_logConnected = false;
bool g_logDisconnected = false;
uint16_t g_logMtu = 0;

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server, ble_gap_conn_desc* desc) override {
    g_connected = true;
    // 12 and 24 are 15 ms and 30 ms. These are the floor Apple's Accessory
    // Design Guidelines allow -- interval min >= 15 ms, and interval max at
    // least 15 ms above it -- and Android rejects out-of-range requests too.
    // The previous 6/12 asked for 7.5 ms and violated both rules, on every
    // connect, which is what a central refuses by dropping the link.
    // 15 ms still carries 66 notifications a second against the 25 we send.
    server->updateConnParams(desc->conn_handle, 12, 24, 0, 400);
    g_logConnected = true;
  }

  void onDisconnect(NimBLEServer* server) override {
    g_connected = false;
    g_mtu = 23;
    g_logDisconnected = true;
    // No startAdvertising() here: NimBLEServer::m_advertiseOnDisconnect
    // defaults to true and the stack restarts it for us the moment this
    // callback returns. Doing it again just fails with EALREADY.
  }

  void onMTUChange(uint16_t mtu, ble_gap_conn_desc* desc) override {
    g_mtu = mtu;
    g_logMtu = mtu;
  }
};

ServerCallbacks g_callbacks;

}  // namespace

bool BleLink::begin(const char* deviceName, TelemetryRing& ring) {
  ring_ = &ring;

  NimBLEDevice::init(deviceName);
  // No setMTU and no 2M PHY. Both were sized for a 30 kB/s target that died
  // when the consumer became RaceChrono, which takes one fix per notify --
  // 500 B/s at 25 Hz, inside the default MTU and the 1M PHY. The reference
  // implementation does neither, and every deviation from it here is
  // something that can go wrong with a central we do not control.

  g_server = NimBLEDevice::createServer();
  if (g_server == nullptr) {
    Log::error("ble", "createServer failed");
    return false;
  }
  // false = do not take ownership. g_callbacks is a static object in .bss, and
  // NimBLEServer's destructor calls `delete` on the callbacks it owns
  // (NimBLEServer.cpp:61), which asserts on a non-heap pointer during
  // NimBLEDevice::deinit() on the first mode switch.
  g_server->setCallbacks(&g_callbacks, false);

  NimBLEService* service = g_server->createService(NimBLEUUID(kServiceUuid16));
  g_gpsMain = service->createCharacteristic(
      NimBLEUUID(kGpsMainUuid16), NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  g_gpsTime = service->createCharacteristic(
      NimBLEUUID(kGpsTimeUuid16), NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  service->start();

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(NimBLEUUID(kServiceUuid16));
  // The name must be in the primary advertisement, not only the scan response:
  // a passive scanner never sends a scan request and so never sees the latter.
  // The 16-bit UUID above leaves room for it.
  advertising->setName(deviceName);
  advertising->setScanResponse(true);
  if (!advertising->start()) {
    // start() fails if the advertisement payload will not fit in 31 bytes,
    // among other things. Logging success unconditionally would report a
    // device that is on the air when it is not.
    Log::error("ble", "advertising failed to start");
    return false;
  }

  Log::info("ble", "advertising as %s", deviceName);
  return true;
}

void BleLink::end() {
  NimBLEDevice::stopAdvertising();
  NimBLEDevice::deinit(true);
  g_server = nullptr;
  g_gpsMain = nullptr;
  g_gpsTime = nullptr;
  g_connected = false;
  g_mtu = 23;
  connected_ = false;
  mtu_ = 23;
  Log::info("ble", "stopped");
}

void BleLink::tick(uint32_t nowMs) {
  (void)nowMs;
  connected_ = g_connected;
  mtu_ = g_mtu;

  // The callbacks only raise flags; the logging happens here, on loop(), so a
  // slow serial host can never delay connection handling.
  if (g_logConnected) {
    g_logConnected = false;
    Log::info("ble", "connected");
  }
  if (g_logDisconnected) {
    g_logDisconnected = false;
    Log::info("ble", "disconnected, advertising again");
  }
  if (g_logMtu != 0) {
    Log::info("ble", "mtu %u", (unsigned)g_logMtu);
    g_logMtu = 0;
  }

  if (!connected_ || ring_ == nullptr || g_gpsMain == nullptr) {
    return;
  }

  uint8_t packet[TelemetrySample::kSize];
  for (size_t i = 0; i < kMaxPacketsPerTick; ++i) {
    const size_t n = ring_->drain(packet, sizeof(packet));
    if (n == 0) {
      break;
    }
    g_gpsMain->setValue(packet, n);
    g_gpsMain->notify();
    sentBytes_ += n;
  }
}

void BleLink::publishTime(const uint8_t bytes[3]) {
  if (g_gpsTime == nullptr) {
    return;
  }
  g_gpsTime->setValue(bytes, 3);
  if (connected_) {
    g_gpsTime->notify();
    sentBytes_ += 3;
  }
}
