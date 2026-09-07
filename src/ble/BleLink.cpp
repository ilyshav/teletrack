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

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server, ble_gap_conn_desc* desc) override {
    g_connected = true;
    // Ask for the shortest interval the client will accept: throughput is
    // packets-per-interval, so the interval is the dominant term.
    server->updateConnParams(desc->conn_handle, 6, 12, 0, 200);
    Log::info("ble", "connected");
  }

  void onDisconnect(NimBLEServer* server) override {
    g_connected = false;
    g_mtu = 23;
    Log::info("ble", "disconnected, advertising again");
    NimBLEDevice::startAdvertising();
  }

  void onMTUChange(uint16_t mtu, ble_gap_conn_desc* desc) override {
    g_mtu = mtu;
    Log::info("ble", "mtu %u", (unsigned)mtu);
  }
};

ServerCallbacks g_callbacks;

}  // namespace

bool BleLink::begin(const char* deviceName, TelemetryRing& ring) {
  ring_ = &ring;

  NimBLEDevice::init(deviceName);
  NimBLEDevice::setMTU(517);
  // 2M PHY doubles the symbol rate; without it the budget does not close.
  // NimBLE-Arduino 1.4.3 has no NimBLEDevice::setDefaultPhy wrapper (that
  // came later, in the 2.x line) so this calls the underlying NimBLE host
  // function it would otherwise wrap.
  ble_gap_set_prefered_default_le_phy(BLE_GAP_LE_PHY_2M_MASK, BLE_GAP_LE_PHY_2M_MASK);

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
