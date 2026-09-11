#include "can/CanBus.h"

#include "core/Log.h"

#if defined(BOARD_TBEAM)

#include <Arduino.h>
#include <driver/twai.h>

#include "board/BoardConfig.h"

bool CanBus::transceiverPresent() {
  pinMode(BoardConfig::kCanRxPin, INPUT_PULLDOWN);
  // The pull-down is weak (tens of kilohms) and the transceiver's output is
  // not; a couple of milliseconds is far longer than either needs to settle.
  delay(2);
  const bool driven = digitalRead(BoardConfig::kCanRxPin) == HIGH;
  pinMode(BoardConfig::kCanRxPin, INPUT);
  return driven;
}

bool CanBus::begin() {
  // Idempotent. A second install fails because the first driver is still
  // there, and reporting that failure would set present_ false while a
  // correctly listening driver kept running underneath -- read() would then
  // return nothing for the rest of the session, silently. GpsReceiver::begin()
  // is already re-called on a sample-rate change, so re-entry here is a matter
  // of time rather than a hypothetical.
  if (present_) {
    return true;
  }

  // LISTEN_ONLY is the whole safety story. In any other mode the controller
  // acknowledges every frame it receives and emits error frames when it
  // disagrees with the bus -- it becomes an active participant on a live
  // powertrain bus. The driver's own header puts it plainly: this mode "will
  // not influence the bus (No transmissions or acknowledgments)".
  twai_general_config_t general = TWAI_GENERAL_CONFIG_DEFAULT(
      static_cast<gpio_num_t>(BoardConfig::kCanTxPin),
      static_cast<gpio_num_t>(BoardConfig::kCanRxPin), TWAI_MODE_LISTEN_ONLY);
  // Sized for a display repaint rather than for loop()'s average pass -- see
  // kRxQueueLen. An overflow is no longer assumed harmless either: RaceChrono
  // subscribes to specific ids, so a dropped frame is as likely to be one that
  // was asked for as one the filter would have discarded.
  general.rx_queue_len = kRxQueueLen;
  general.tx_queue_len = 0;  // nothing is ever transmitted

  const twai_timing_config_t timing = TWAI_TIMING_CONFIG_500KBITS();
  // Everything is accepted in hardware and filtered in software: the hardware
  // filter is a single code and mask, which cannot express the arbitrary set of
  // ids RaceChrono asks for.
  const twai_filter_config_t filter = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  if (twai_driver_install(&general, &timing, &filter) != ESP_OK) {
    Log::error("can", "driver install failed");
    present_ = false;
    return false;
  }
  if (twai_start() != ESP_OK) {
    Log::error("can", "start failed");
    twai_driver_uninstall();
    present_ = false;
    return false;
  }

  present_ = true;
  Log::info("can", "listening at %lu kbps on tx=%u rx=%u",
            static_cast<unsigned long>(kBitrateKbps),
            static_cast<unsigned>(BoardConfig::kCanTxPin),
            static_cast<unsigned>(BoardConfig::kCanRxPin));
  return true;
}

bool CanBus::read(CanFrame& out) {
  if (!present_) {
    return false;
  }
  twai_message_t message;
  // Zero timeout: this is called from loop() and must never block on a bus
  // that has gone quiet.
  if (twai_receive(&message, 0) != ESP_OK) {
    return false;
  }
  if (message.rtr) {
    return false;  // a remote request carries no data to forward
  }

  out.id = message.identifier;
  out.dlc = message.data_length_code > 8 ? 8 : message.data_length_code;
  for (uint8_t i = 0; i < out.dlc; ++i) {
    out.data[i] = message.data[i];
  }

  if (received_++ == 0) {
    Log::info("can", "first frame, id 0x%lX", static_cast<unsigned long>(out.id));
  }
  return true;
}

CanBus::Diagnostics CanBus::diagnostics() const {
  Diagnostics out;
  if (!present_) {
    out.state = "not installed";
    return out;
  }

  twai_status_info_t status;
  if (twai_get_status_info(&status) != ESP_OK) {
    out.state = "unreadable";
    return out;
  }

  switch (status.state) {
    case TWAI_STATE_STOPPED:
      out.state = "STOPPED";
      break;
    case TWAI_STATE_RUNNING:
      out.state = "RUNNING";
      break;
    case TWAI_STATE_BUS_OFF:
      out.state = "BUS_OFF";
      break;
    case TWAI_STATE_RECOVERING:
      out.state = "RECOVERING";
      break;
    default:
      out.state = "?";
      break;
  }
  out.rxErrors = status.rx_error_counter;
  out.busErrors = status.bus_error_count;
  out.missed = status.rx_missed_count + status.rx_overrun_count;
  out.arbLost = status.arb_lost_count;
  return out;
}

uint16_t CanBus::takeLost() {
  if (!present_) {
    return 0;
  }
  twai_status_info_t status;
  if (twai_get_status_info(&status) != ESP_OK) {
    return 0;
  }

  // Both counters are cumulative and both mean the same thing to us: a frame
  // was on the bus and we never saw it.
  const uint32_t total = status.rx_missed_count + status.rx_overrun_count;
  const uint32_t since = total - lostSeen_;
  lostSeen_ = total;
  return since > UINT16_MAX ? UINT16_MAX : static_cast<uint16_t>(since);
}

#else

bool CanBus::transceiverPresent() {
  return false;  // no transceiver on this board, and no pin to test
}

CanBus::Diagnostics CanBus::diagnostics() const {
  Diagnostics out;
  out.state = "no transceiver";
  return out;
}

uint16_t CanBus::takeLost() { return 0; }

bool CanBus::begin() {
  present_ = false;
  return false;  // no transceiver on this board
}

bool CanBus::read(CanFrame& out) {
  (void)out;
  return false;
}

#endif
