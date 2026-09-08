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
// The live connection, so end() can drop the peer politely instead of letting
// it time out. Only one is ever allowed (CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1).
uint16_t g_connHandle = 0;
bool g_hasConn = false;

// Set by the callbacks, drained by tick(). The callbacks run on the NimBLE
// host task, and logging there puts a USB CDC write -- which can drop or
// stall -- in the middle of connection handling. Flag it and let loop() do
// the talking.
// What RaceChrono last wrote to the CAN filter characteristic, drained by
// tick(). Recording it answers a question we could not otherwise ask: whether
// the app talks to that characteristic at all.
uint8_t g_filterCmd = 0;
bool g_filterWritten = false;

bool g_logConnected = false;
bool g_logDisconnected = false;
uint16_t g_logMtu = 0;

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server, ble_gap_conn_desc* desc) override {
    g_connected = true;
    g_connHandle = desc->conn_handle;
    g_hasConn = true;
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
    g_hasConn = false;
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

// Accepts and ignores the filter commands: deny-all (0), allow-all (1) and
// allow-one-PID (2) all mean the same thing to a device with no CAN bus. What
// matters is that the write succeeds, so the app can finish configuring.
class FilterCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic) override {
    const std::string value = characteristic->getValue();
    if (!value.empty()) {
      g_filterCmd = static_cast<uint8_t>(value[0]);
      g_filterWritten = true;
    }
  }
};

FilterCallbacks g_filterCallbacks;

}  // namespace

bool BleLink::begin(const char* deviceName, TelemetryRing& ring) {
  ring_ = &ring;

  if (NimBLEDevice::getInitialized()) {
    // end() leaves the stack running on purpose, so the service and its
    // characteristics are still there from the first begin(). Only the name
    // and the advertising need to come back.
    NimBLEDevice::setDeviceName(deviceName);
    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    advertising->setName(deviceName);
    if (!advertising->start()) {
      Log::error("ble", "advertising failed to restart");
      return false;
    }
    Log::info("ble", "advertising as %s", deviceName);
    return true;
  }

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

  // The CAN characteristics come first, in the order the reference declares
  // them. Nothing is ever notified on 0x0001 -- there is no CAN bus here --
  // but both must exist for RaceChrono to finish setting the device up.
  service->createCharacteristic(NimBLEUUID(kCanMainUuid16),
                                NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  NimBLECharacteristic* filter = service->createCharacteristic(
      NimBLEUUID(kCanFilterUuid16), NIMBLE_PROPERTY::WRITE);
  // Not owned: NimBLECharacteristic's destructor does not delete its
  // callbacks, unlike NimBLEServer's, so a static object is safe here.
  filter->setCallbacks(&g_filterCallbacks);

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
  // Deliberately NOT NimBLEDevice::deinit(). Tearing the stack down while its
  // own host task is running it crashes with PC=0 inside nimble_port_run()
  // (NimBLEDevice.cpp:837) -- observed on a mode switch, and again on sleep.
  // The reference implementation never deinitialises either; that was our
  // invention and it has had two ways to bite.
  //
  // Going quiet is all a mode switch or a sleep actually needs: advertising
  // stops and the peer is dropped, so the radio is silent and the front end is
  // free for WiFi. The stack stays up, and begin() knows how to find it.
  NimBLEDevice::stopAdvertising();
  if (g_hasConn && g_server != nullptr) {
    // Tell the peer rather than leaving it to notice a supervision timeout.
    g_server->disconnect(g_connHandle);
    g_hasConn = false;
  }
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
  if (g_filterWritten) {
    g_filterWritten = false;
    Log::info("ble", "racechrono wrote filter command %u",
              static_cast<unsigned>(g_filterCmd));
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
