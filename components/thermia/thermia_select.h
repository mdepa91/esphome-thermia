#pragma once

// A writable pump register with a small, named set of valid values (e.g. main_mode), exposed as an ESPHome
// select::Select. The option at index N is written as the raw value N - callers must list options in that order.
// Same caveat as ThermiaNumber: control() only queues the write, the display is corrected by the next real reading.

#include "esphome/components/select/select.h"

namespace esphome {
namespace thermia {

class ThermiaComponent;

class ThermiaSelect : public select::Select {
 public:
  void set_parent(ThermiaComponent *parent) { this->parent_ = parent; }
  void set_register(uint8_t reg) { this->reg_ = reg; }

 protected:
  void control(size_t index) override;

  ThermiaComponent *parent_{nullptr};
  uint8_t reg_{0};
};

}  // namespace thermia
}  // namespace esphome
