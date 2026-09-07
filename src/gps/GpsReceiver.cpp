#include "gps/GpsReceiver.h"

#include "core/Log.h"

#if defined(BOARD_TBEAM)

#include <Arduino.h>

#include "board/BoardConfig.h"

namespace {

// The module's current baud is genuinely unknown: LilyGO's board support says
// 9600, u-blox M10 parts default to 38400, and LilyGO ship a recovery sketch
// that sweeps bauds precisely because it varies. We also set 115200 ourselves
// in the RAM layer, which survives a warm reset but not a power cycle. So all
// three are plausible on any given boot.
constexpr uint32_t kBaudCandidates[] = {115200, 38400, 9600};
constexpr uint32_t kTargetBaud = 115200;
constexpr uint32_t kProbeMs = 250;

constexpr uint8_t kClassCfg = 0x06;
constexpr uint8_t kIdValset = 0x8A;

// Config keys, from SparkFun's u-blox_config_keys.h.
constexpr uint32_t kKeyUart1Baud = 0x40520001;    // U4
constexpr uint32_t kKeyNavPvtUart1 = 0x20910007;  // U1
constexpr uint32_t kKeyRateMeas = 0x30210001;     // U2
constexpr uint32_t kKeyRateNav = 0x30210002;      // U2
constexpr uint32_t kKeyNmeaOff[] = {
    0x209100bb,  // GGA
    0x209100ca,  // GLL
    0x209100c0,  // GSA
    0x209100c5,  // GSV
    0x209100ac,  // RMC
    0x209100b1,  // VTG
};

// Wraps a payload in sync bytes, header and Fletcher checksum, and writes it.
void sendUbx(uint8_t cls, uint8_t id, const uint8_t* payload, uint16_t len) {
  const uint8_t header[6] = {0xB5, 0x62, cls, id,
                             static_cast<uint8_t>(len),
                             static_cast<uint8_t>(len >> 8)};
  uint8_t ckA = 0;
  uint8_t ckB = 0;
  for (size_t i = 2; i < 6; ++i) {
    ckA += header[i];
    ckB += ckA;
  }
  for (uint16_t i = 0; i < len; ++i) {
    ckA += payload[i];
    ckB += ckA;
  }
  const uint8_t checksum[2] = {ckA, ckB};

  Serial1.write(header, sizeof(header));
  Serial1.write(payload, len);
  Serial1.write(checksum, sizeof(checksum));
  Serial1.flush();
}

// CFG-VALSET header: version 0, RAM layer only, two reserved bytes. RAM means
// the module reverts to its own defaults on a power cycle, so we reconfigure
// every boot -- no flash wear, and nothing left permanently altered.
size_t beginValset(uint8_t* p) {
  p[0] = 0x00;
  p[1] = 0x01;
  p[2] = 0x00;
  p[3] = 0x00;
  return 4;
}

size_t addKey(uint8_t* p, size_t n, uint32_t key) {
  p[n++] = static_cast<uint8_t>(key);
  p[n++] = static_cast<uint8_t>(key >> 8);
  p[n++] = static_cast<uint8_t>(key >> 16);
  p[n++] = static_cast<uint8_t>(key >> 24);
  return n;
}

size_t addU1(uint8_t* p, size_t n, uint32_t key, uint8_t value) {
  n = addKey(p, n, key);
  p[n++] = value;
  return n;
}

size_t addU2(uint8_t* p, size_t n, uint32_t key, uint16_t value) {
  n = addKey(p, n, key);
  p[n++] = static_cast<uint8_t>(value);
  p[n++] = static_cast<uint8_t>(value >> 8);
  return n;
}

size_t addU4(uint8_t* p, size_t n, uint32_t key, uint32_t value) {
  n = addKey(p, n, key);
  p[n++] = static_cast<uint8_t>(value);
  p[n++] = static_cast<uint8_t>(value >> 8);
  p[n++] = static_cast<uint8_t>(value >> 16);
  p[n++] = static_cast<uint8_t>(value >> 24);
  return n;
}

// Opens Serial1 at baud and looks for evidence of a receiver: a UBX sync
// pair, or the '$' that starts an NMEA sentence. Merely seeing bytes is not
// evidence -- a floating RX pin, or the right pin at the wrong baud, produces
// plenty of those, and accepting them would lock us to a baud that cannot
// carry a single valid frame.
bool probe(uint32_t baud) {
  Serial1.begin(baud, SERIAL_8N1, BoardConfig::kGpsRxPin, BoardConfig::kGpsTxPin);
  const uint32_t deadline = millis() + kProbeMs;
  uint8_t prev = 0;
  while (millis() < deadline) {
    while (Serial1.available() > 0) {
      const uint8_t b = static_cast<uint8_t>(Serial1.read());
      if (b == '$' || (prev == UbxParser::kSync1 && b == UbxParser::kSync2)) {
        return true;
      }
      prev = b;
    }
    delay(5);
  }
  Serial1.end();
  return false;
}

}  // namespace

bool GpsReceiver::begin(uint8_t rateHz) {
  if (rateHz == 0) {
    rateHz = 1;
  }
  if (rateHz > kMaxRateHz) {
    Log::warn("gps", "%u Hz is above the MAX-M10S limit, using %u Hz",
              static_cast<unsigned>(rateHz), static_cast<unsigned>(kMaxRateHz));
    rateHz = kMaxRateHz;
  }

  uint32_t found = 0;
  for (uint32_t baud : kBaudCandidates) {
    if (probe(baud)) {
      found = baud;
      break;
    }
  }
  if (found == 0) {
    Log::error("gps", "no response at any baud");
    present_ = false;
    return false;
  }
  Log::info("gps", "receiver answering at %lu baud", static_cast<unsigned long>(found));

  // Raise the baud first. NAV-PVT is 100 bytes on the wire, so 10 Hz needs
  // 1000 B/s and 9600 baud 8N1 carries only 960 -- it does not fit.
  if (found != kTargetBaud) {
    uint8_t p[16];
    size_t n = beginValset(p);
    n = addU4(p, n, kKeyUart1Baud, kTargetBaud);
    sendUbx(kClassCfg, kIdValset, p, static_cast<uint16_t>(n));
    delay(100);  // let the module finish the reply at the old rate
    Serial1.updateBaudRate(kTargetBaud);
    delay(100);
  }

  // One VALSET per concern, so a key the module rejects cannot take the
  // others down with it.
  {
    uint8_t p[64];
    size_t n = beginValset(p);
    for (uint32_t key : kKeyNmeaOff) {
      n = addU1(p, n, key, 0);
    }
    sendUbx(kClassCfg, kIdValset, p, static_cast<uint16_t>(n));
  }
  {
    uint8_t p[16];
    size_t n = beginValset(p);
    n = addU1(p, n, kKeyNavPvtUart1, 1);
    sendUbx(kClassCfg, kIdValset, p, static_cast<uint16_t>(n));
  }
  {
    uint8_t p[24];
    size_t n = beginValset(p);
    n = addU2(p, n, kKeyRateMeas, static_cast<uint16_t>(1000u / rateHz));
    n = addU2(p, n, kKeyRateNav, 1);
    sendUbx(kClassCfg, kIdValset, p, static_cast<uint16_t>(n));
  }

  rateHz_ = rateHz;
  present_ = true;
  // Deliberately no ACK parsing: NAV-PVT frames actually arriving is stronger
  // evidence than an ACK, and much less code. See the log line in tick().
  Log::info("gps", "configured for UBX-NAV-PVT at %u Hz",
            static_cast<unsigned>(rateHz));
  return true;
}

bool GpsReceiver::tick() {
  if (!present_) {
    return false;
  }
  bool decoded = false;
  // Bounded so one call cannot monopolise loop() if the buffer has backed up.
  for (int i = 0; i < 512 && Serial1.available() > 0; ++i) {
    if (parser_.feed(static_cast<uint8_t>(Serial1.read()))) {
      decoded = true;
      if (frames_++ == 0) {
        Log::info("gps", "first NAV-PVT decoded, %u satellites",
                  static_cast<unsigned>(parser_.fix().satellites));
      }
    }
  }
  return decoded;
}

#else

bool GpsReceiver::begin(uint8_t rateHz) {
  (void)rateHz;
  present_ = false;
  return false;  // no receiver on this board
}

bool GpsReceiver::tick() { return false; }

#endif
