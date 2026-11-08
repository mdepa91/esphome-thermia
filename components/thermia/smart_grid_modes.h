#pragma once

// Smart grid / EVU inputs of the pump (Thermia Legend installation guide, "Inteligentna sieć/EVU"):
//   SG1 = terminals 307/308, SG2 = terminals 309/310. Contacts are driven by PhotoMOS (solid-state) relays:
//   - SG1      shorts 307/308                 (EVU block)
//   - SG1 10k  puts 10 kOhm across 307/308    (reduction)
//   - SG2      shorts 309/310                 (comfort, or boost together with SG1)
// Plain C++ without ESPHome headers, so the mapping can be tested on the host (test/test_smart_grid.cpp).

#include <cstdint>

namespace esphome {
namespace thermia {

enum class SmartGridMode : uint8_t { NORMAL = 0, REDUCTION, EVU_BLOCK, COMFORT, BOOST, COUNT };

// Output bits.
static constexpr uint8_t SG_OUT_SG1 = 0x01;
static constexpr uint8_t SG_OUT_SG1_10K = 0x02;
static constexpr uint8_t SG_OUT_SG2 = 0x04;

// Which outputs are closed in a given mode.
//   SG1 open,  SG2 open   -> normal
//   SG1 10k,   SG2 open   -> reduction
//   SG1 short, SG2 open   -> EVU block (compressor and aux heater off)
//   SG1 open,  SG2 short  -> comfort
//   SG1 short, SG2 short  -> boost
inline uint8_t smart_grid_outputs(SmartGridMode mode) {
  switch (mode) {
    case SmartGridMode::REDUCTION:
      return SG_OUT_SG1_10K;
    case SmartGridMode::EVU_BLOCK:
      return SG_OUT_SG1;
    case SmartGridMode::COMFORT:
      return SG_OUT_SG2;
    case SmartGridMode::BOOST:
      return SG_OUT_SG1 | SG_OUT_SG2;
    default:
      return 0;
  }
}

}  // namespace thermia
}  // namespace esphome
