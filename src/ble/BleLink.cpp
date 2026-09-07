#include "ble/BleLink.h"

#include <NimBLEDevice.h>

#include "core/Log.h"

namespace {

NimBLEServer* g_server = nullptr;
NimBLECharacteristic* g_live = nullptr;
NimBLECharacteristic* g_status = nullptr;
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

  NimBLEService* service = g_server->createService(kServiceUuid);
  g_live = service->createCharacteristic(kLiveUuid, NIMBLE_PROPERTY::NOTIFY);
  g_status = service->createCharacteristic(
      kStatusUuid, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  service->start();

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(kServiceUuid);
  advertising->setScanResponse(true);
  advertising->start();

  Log::info("ble", "advertising as %s", deviceName);
  return true;
}

void BleLink::end() {
  NimBLEDevice::stopAdvertising();
  NimBLEDevice::deinit(true);
  g_server = nullptr;
  g_live = nullptr;
  g_status = nullptr;
  g_connected = false;
  g_mtu = 23;
  connected_ = false;
  mtu_ = 23;
  Log::info("ble", "stopped");
}

void BleLink::tick(uint32_t nowMs) {
  connected_ = g_connected;
  mtu_ = g_mtu;

  if (!connected_ || ring_ == nullptr || g_live == nullptr) {
    return;
  }

  // Fill one notification to just under the negotiated MTU. Batching is what
  // keeps protocol overhead from eating the budget.
  const size_t budget = (mtu_ > 3) ? static_cast<size_t>(mtu_ - 3) : 20;
  const size_t limit = budget < kMaxNotifyBytes ? budget : kMaxNotifyBytes;

  uint8_t buffer[kMaxNotifyBytes];
  const size_t n = ring_->drain(buffer, limit);
  if (n == 0) {
    return;
  }

  g_live->setValue(buffer, n);
  g_live->notify();
  sentBytes_ += n;
}
