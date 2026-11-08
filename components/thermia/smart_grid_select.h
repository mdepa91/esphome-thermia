#pragma once

// "Smart grid mode" select: drives the pump's SG1 (307/308) and SG2 (309/310) inputs through PhotoMOS relays on
// ESP GPIOs. It does not touch the EXT bus. The pump gives no feedback over EXT, so the state shown is what we set.
// At boot every output is off (open contact = normal operation); the mode is not restored after a restart.
// The option list is built in __init__.py from the configured pins: mode_[i] is the mode of option i.

#include <vector>

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/components/select/select.h"

#include "smart_grid_modes.h"

namespace esphome {
namespace thermia {

class SmartGridSelect : public select::Select, public Component {
 public:
  void set_sg1_pin(GPIOPin *pin) { this->pins_[0] = pin; }
  void set_sg1_10k_pin(GPIOPin *pin) { this->pins_[1] = pin; }
  void set_sg2_pin(GPIOPin *pin) { this->pins_[2] = pin; }
  void add_mode(uint8_t mode) { this->modes_.push_back(static_cast<SmartGridMode>(mode)); }

  void setup() override {
    for (GPIOPin *pin : this->pins_) {
      if (pin == nullptr)
        continue;
      pin->setup();
      pin->digital_write(false);
    }
    this->publish_state(static_cast<size_t>(0));  // option 0 is always "Normal"
  }

  void dump_config() override {
    static const char *const TAG = "thermia.smart_grid";
    ESP_LOGCONFIG(TAG, "Smart grid:");
    LOG_PIN("  SG1 (307/308 short): ", this->pins_[0]);
    LOG_PIN("  SG1 10k (307/308 via 10 kOhm): ", this->pins_[1]);
    LOG_PIN("  SG2 (309/310 short): ", this->pins_[2]);
  }

  // Drive the outputs before Wi-Fi and the rest: a floating or wrong output must not linger after boot.
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

 protected:
  void control(size_t index) override {
    if (index >= this->modes_.size())
      return;
    uint8_t want = smart_grid_outputs(this->modes_[index]);
    // Close new contacts first, then open the old ones. Reduction <-> EVU then passes through "10k parallel with
    // a short" (= short), never through "open" (= normal), so the pump does not see a brief normal mode.
    for (int pass = 0; pass < 2; pass++) {
      for (int i = 0; i < 3; i++) {
        if (this->pins_[i] == nullptr)
          continue;
        bool on = want & (1 << i);
        if (on == (pass == 0))
          this->pins_[i]->digital_write(on);
      }
    }
    this->publish_state(index);
  }

  GPIOPin *pins_[3]{nullptr, nullptr, nullptr};  // bit order: SG_OUT_SG1, SG_OUT_SG1_10K, SG_OUT_SG2
  std::vector<SmartGridMode> modes_;
};

}  // namespace thermia
}  // namespace esphome
