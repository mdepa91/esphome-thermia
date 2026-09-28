#pragma once

// A writable pump register, exposed as an ESPHome number::Number. control() only queues the write (see
// ThermiaSlave::queue_write in thermia_slave.h) - it does not confirm the pump applied it. The displayed state is
// corrected by the next real reading of the same register (ThermiaComponent::update()), so if the pump ignores or
// clamps the value, the entity settles back to what the pump actually reports, not what we asked for.

#include "esphome/components/number/number.h"

namespace esphome {
namespace thermia {

class ThermiaComponent;

class ThermiaNumber : public number::Number {
 public:
  void set_parent(ThermiaComponent *parent) { this->parent_ = parent; }
  void set_register(uint8_t reg) { this->reg_ = reg; }
  void set_scale(float scale) { this->scale_ = scale; }
  void set_signed(bool is_signed) { this->signed_ = is_signed; }

 protected:
  void control(float value) override;

  ThermiaComponent *parent_{nullptr};
  uint8_t reg_{0};
  float scale_{1.0f};
  bool signed_{true};
};

}  // namespace thermia
}  // namespace esphome
