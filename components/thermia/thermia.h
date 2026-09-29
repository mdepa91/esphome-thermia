#pragma once

#include <vector>

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/number/number.h"
#include "esphome/components/select/select.h"
#include "esphome/components/sensor/sensor.h"

#include "thermia_bus_gate.h"
#include "thermia_slave.h"

namespace esphome {
namespace thermia {

class ThermiaComponent : public PollingComponent {
 public:
  void set_sda_pin(InternalGPIOPin *pin) { this->sda_pin_ = pin; }
  void set_scl_pin(InternalGPIOPin *pin) { this->scl_pin_ = pin; }
  void set_stale_timeout(uint32_t ms) { this->stale_timeout_ms_ = ms; }
  void set_debug_frames(bool enabled) { this->debug_frames_ = enabled; }
  void set_sniff(bool enabled) { this->sniff_ = enabled; }
  void set_selftest(bool enabled) { this->selftest_ = enabled; }
  void set_link_sensor(binary_sensor::BinarySensor *sensor) { this->link_ = sensor; }

  void add_sensor(uint8_t reg, sensor::Sensor *sensor, float scale, bool is_signed, float min, float max) {
    this->sensors_.push_back({reg, sensor, scale, is_signed, min, max});
    this->note_register_(reg);
  }
  void add_binary_sensor(uint8_t reg, uint16_t mask, binary_sensor::BinarySensor *sensor) {
    this->binary_sensors_.push_back({reg, mask, sensor});
    this->note_register_(reg);
  }
  // Also polls the register for reading, so the entity's displayed state is corrected by what the pump actually
  // reports, not just what we last asked it to write - see thermia_number.h / thermia_select.h.
  void add_number(uint8_t reg, number::Number *num, float scale, bool is_signed, float min, float max) {
    this->numbers_.push_back({reg, num, scale, is_signed, min, max});
    this->note_register_(reg);
  }
  void add_select(uint8_t reg, select::Select *sel) {
    this->selects_.push_back({reg, sel});
    this->note_register_(reg);
  }
  // Forwarded from ThermiaNumber::control() / ThermiaSelect::control() - see thermia_slave.h queue_write() for
  // what "delivered" does and does not guarantee.
  bool queue_write(uint8_t reg, uint16_t raw_value) { return this->slave_.queue_write(reg, raw_value); }

  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  // Wait this long after the network comes up before touching the pump's wires (and go quiet again the moment it
  // drops) - see thermia_bus_gate.h for why.
  static constexpr uint32_t BUS_GATE_GRACE_MS = 5000;

 protected:
  struct SensorBinding {
    uint8_t reg;
    sensor::Sensor *sensor;
    float scale;
    bool is_signed;
    float min;
    float max;
  };
  struct BinaryBinding {
    uint8_t reg;
    uint16_t mask;
    binary_sensor::BinarySensor *sensor;
  };
  struct NumberBinding {
    uint8_t reg;
    number::Number *number;
    float scale;
    bool is_signed;
    float min;
    float max;
  };
  struct SelectBinding {
    uint8_t reg;
    select::Select *select;
  };

  static void isr_(ThermiaComponent *self);

  void note_register_(uint8_t reg) {
    for (uint8_t r : this->registers_)
      if (r == reg)
        return;
    this->registers_.push_back(reg);
  }

  // Latest value of a register, if it was received recently enough.
  bool fresh_value_(uint8_t reg, uint16_t &raw) const;
  void refresh_timestamps_(uint32_t now);
  void drain_frame_log_();
  void dump_trace_();
  void watch_pins_(uint32_t now);
  void update_link_(uint32_t now);
  void update_bus_gate_(uint32_t now);

  InternalGPIOPin *sda_pin_{nullptr};
  InternalGPIOPin *scl_pin_{nullptr};
  uint32_t stale_timeout_ms_{120000};
  bool debug_frames_{false};
  bool sniff_{false};
  bool selftest_{false};
  ThermiaSlave::ProbeResult probe_{};
  int watch_last_sda_{-1};
  uint32_t watch_changes_{0};
  uint32_t watch_last_log_ms_{0};
  uint32_t watch_status_ms_{0};
  uint32_t last_trace_ms_{0};
  uint32_t trace_count_{0};
  uint32_t last_log_drain_ms_{0};

  BusGate bus_gate_{BUS_GATE_GRACE_MS};
  bool bus_attached_{false};

  ThermiaSlave slave_;
  std::vector<SensorBinding> sensors_;
  std::vector<BinaryBinding> binary_sensors_;
  std::vector<NumberBinding> numbers_;
  std::vector<SelectBinding> selects_;
  std::vector<uint8_t> registers_;
  binary_sensor::BinarySensor *link_{nullptr};

  uint16_t seen_seq_[ThermiaSlave::MAX_REG + 1]{};
  uint32_t rx_ms_[ThermiaSlave::MAX_REG + 1]{};
  uint32_t last_scan_ms_{0};
  uint32_t last_pings_{0};
  uint32_t last_ping_ms_{0};
  bool link_up_{false};
  bool link_published_{false};
  uint32_t guard_since_ms_{0};
};

}  // namespace thermia
}  // namespace esphome
