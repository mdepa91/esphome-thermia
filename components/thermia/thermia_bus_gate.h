#pragma once

// Decides when it is safe to start/stop touching the pump's wires (attach/detach the GPIO interrupt that drives
// the bit-banged I2C slave). Pure state machine, no hardware or ESPHome dependency, so it is unit-testable on the
// host.
//
// Why this exists: the very first version of this component attached the interrupt unconditionally in setup(),
// which on ESP8266 runs long before WiFi has actually associated (setup() only starts the connection attempt,
// it does not wait for it). With the pump hammering the bus at ~20 kHz right from boot, the ISR and the resulting
// debug logging competed with WiFi's connection handshake for CPU time - on a weak link (this installation saw
// about -79 dB) that was enough to keep the device from ever joining the network. The device that went dark after
// an OTA update most likely never got any further than this.
//
// The fix: never attach the interrupt until the network has been up for a few seconds, and detach it immediately
// if the network drops, so reconnecting is never competing with pump traffic either.

#include <stdint.h>

namespace esphome {
namespace thermia {

class BusGate {
 public:
  explicit BusGate(uint32_t grace_ms) : grace_ms_(grace_ms) {}

  // Call once per loop() with the current network state and millis(). Returns whether the bus should be attached
  // right now; the caller attaches/detaches on each change of this value.
  bool update(bool network_connected, uint32_t now_ms) {
    if (!network_connected) {
      this->connected_since_ms_ = 0;
      this->attached_ = false;
      return false;
    }
    if (this->connected_since_ms_ == 0)
      this->connected_since_ms_ = now_ms ? now_ms : 1;
    // Unsigned subtraction wraps correctly across the millis() rollover (~49 days), no special-casing needed.
    if (now_ms - this->connected_since_ms_ >= this->grace_ms_)
      this->attached_ = true;
    return this->attached_;
  }

  bool attached() const { return this->attached_; }

 private:
  uint32_t grace_ms_;
  uint32_t connected_since_ms_{0};
  bool attached_{false};
};

}  // namespace thermia
}  // namespace esphome
