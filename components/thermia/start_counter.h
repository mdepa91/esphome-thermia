#pragma once

// Compressor start counter for "today" and "this month". The pump has no start counter register, so starts are
// counted here from the compressor bit (r10 & 0x0002): an off -> on change between two readings is one start.
// Plain C++ without ESPHome headers, so it can be tested on the host (test/test_start_counter.cpp).
//
// Day/month come from the caller as keys (day = YYYYMMDD, month = YYYYMM, 0 = clock not valid yet). When the key
// changes, that counter starts again from 0. The state is a plain struct so it can be stored in flash as is.

#include <cstdint>

namespace esphome {
namespace thermia {

struct StartCounterState {
  uint32_t day_key{0};
  uint32_t month_key{0};
  uint32_t today{0};
  uint32_t month{0};
};

class StartCounter {
 public:
  StartCounterState &state() { return this->state_; }
  const StartCounterState &state() const { return this->state_; }

  // Feed the current compressor state. The first reading after boot only sets the baseline: we cannot tell whether
  // the compressor started while we were not looking. Returns true when a start was counted.
  bool feed(bool running) {
    const bool counted = this->known_ && !this->running_ && running;
    this->running_ = running;
    this->known_ = true;
    if (counted) {
      this->state_.today++;
      this->state_.month++;
    }
    return counted;
  }

  // Apply the current date. With no date stored yet (first run, or starts counted before the clock was valid)
  // the counts are kept and the date is just taken over. Returns true when the stored state changed.
  bool roll(uint32_t day_key, uint32_t month_key) {
    if (day_key == 0 || month_key == 0)
      return false;  // clock not valid yet
    bool changed = false;
    if (this->state_.day_key != day_key) {
      if (this->state_.day_key != 0)
        this->state_.today = 0;
      this->state_.day_key = day_key;
      changed = true;
    }
    if (this->state_.month_key != month_key) {
      if (this->state_.month_key != 0)
        this->state_.month = 0;
      this->state_.month_key = month_key;
      changed = true;
    }
    return changed;
  }

 protected:
  StartCounterState state_;
  bool known_{false};
  bool running_{false};
};

}  // namespace thermia
}  // namespace esphome
