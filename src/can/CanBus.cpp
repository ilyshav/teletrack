#include "can/CanBus.h"

#include "core/Log.h"

#if defined(BOARD_TBEAM)

#include <driver/twai.h>

#include "board/BoardConfig.h"

bool CanBus::begin() {
  // LISTEN_ONLY is the whole safety story. In any other mode the controller
  // acknowledges every frame it receives and emits error frames when it
  // disagrees with the bus -- it becomes an active participant on a live
  // powertrain bus. The driver's own header puts it plainly: this mode "will
  // not influence the bus (No transmissions or acknowledgments)".
  twai_general_config_t general = TWAI_GENERAL_CONFIG_DEFAULT(
      static_cast<gpio_num_t>(BoardConfig::kCanTxPin),
      static_cast<gpio_num_t>(BoardConfig::kCanRxPin), TWAI_MODE_LISTEN_ONLY);
  // A busy 500 kbps bus delivers on the order of 1000-2000 frames a second and
  // loop() turns at roughly 1 kHz, so a pass sees a couple of frames. The queue
  // absorbs the jitter; an overflow costs a frame the filter would probably
  // have dropped anyway.
  general.rx_queue_len = 32;
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

#else

bool CanBus::begin() {
  present_ = false;
  return false;  // no transceiver on this board
}

bool CanBus::read(CanFrame& out) {
  (void)out;
  return false;
}

#endif
