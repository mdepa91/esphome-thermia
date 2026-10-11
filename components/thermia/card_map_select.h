#pragma once

// "Card map": a read-only diagnostic select whose options tell thermia-heatpump-card which entity is which, so the
// card can fill itself in after the user picks the ESPHome device. Option 0 is the marker "thermia-card-map:1"
// (always the selected one), every further option is "<config key>=<entity name>", e.g.
// "temp_brine_in=Brine in". The options are built at compile time in __init__.py; a select is used because its
// option list reaches Home Assistant as an attribute, without the 255-character limit of a text sensor's state.

#include <cstddef>

#include "esphome/core/component.h"
#include "esphome/components/select/select.h"

namespace esphome {
namespace thermia {

class CardMapSelect : public select::Select, public Component {
 public:
  void setup() override { this->publish_state(static_cast<size_t>(0)); }
  float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  // Not a setting: whatever is chosen in Home Assistant, go back to the marker.
  void control(size_t index) override { this->publish_state(static_cast<size_t>(0)); }
};

}  // namespace thermia
}  // namespace esphome
