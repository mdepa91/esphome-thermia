#pragma once

// Host-side stand-in for thermia_hw.h: a scripted I2C master driving SCL/SDA. Each call to read_bus()
// consumes one poll of the current waveform step (a step lasts `rep` polls), and the slave's SDA drive is
// wired-AND'ed with the master's, like on a real open-drain bus.

#include <stdint.h>
#include <utility>
#include <vector>

namespace esphome {
namespace thermia {
namespace hw {

constexpr uint32_t TICKS_PER_US = 10;

struct Step {
  bool scl;
  bool sda;
  int tag;  // >= 0: record what the master sees on SDA at the end of this step
};

struct Sim {
  std::vector<Step> steps;
  size_t pos{0};
  int rep{1};
  int sub{0};
  uint32_t tick{0};
  bool slave_low{false};
  bool sda_stuck_high{false};  // line hard-tied high (short), cannot be pulled low
  bool sda_stuck_low{false};   // line held low by someone else
  uint32_t sda_mask{1u << 4};
  uint32_t scl_mask{1u << 5};
  std::vector<std::pair<int, int>> sampled;  // (tag, value seen on the bus)
};

inline Sim g_sim;

inline uint32_t pin_mask(uint8_t pin) { return 1u << pin; }

inline uint32_t read_bus() {
  Sim &s = g_sim;
  s.tick++;
  bool scl = true, sda = true;
  int tag = -1;
  if (s.pos < s.steps.size()) {
    scl = s.steps[s.pos].scl;
    sda = s.steps[s.pos].sda;
    tag = s.steps[s.pos].tag;
  }
  bool bus_sda = sda && !s.slave_low;
  if (s.sda_stuck_high)
    bus_sda = true;
  if (s.sda_stuck_low)
    bus_sda = false;
  if (tag >= 0 && s.sub == s.rep - 1)
    s.sampled.emplace_back(tag, bus_sda ? 1 : 0);
  const uint32_t v = (scl ? s.scl_mask : 0) | (bus_sda ? s.sda_mask : 0);
  if (++s.sub >= s.rep) {
    s.sub = 0;
    s.pos++;
  }
  return v;
}

inline void sda_low(uint32_t) { g_sim.slave_low = true; }
inline void sda_release(uint32_t) { g_sim.slave_low = false; }
inline uint32_t ticks() { return g_sim.tick; }
inline void clear_irq(uint32_t) {}
inline void prepare_sda(uint8_t, uint32_t) {}
inline uint32_t irq_lock() { return 0; }
inline void irq_unlock(uint32_t) {}

}  // namespace hw
}  // namespace thermia
}  // namespace esphome
