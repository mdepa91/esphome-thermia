#pragma once

// Thin hardware layer for the bit-banged I2C slave. Everything here must be safe to call from an ISR.
// The host test (test/) provides its own thermia_sim_hw.h with the same interface.

#include <stdint.h>

#ifdef THERMIA_HOST_TEST

#include "thermia_sim_hw.h"

#else  // ESP8266 firmware build

#ifndef USE_ESP8266
#error "The thermia component only supports ESP8266 (it needs to emulate an I2C slave in software)."
#endif

#include <Arduino.h>

namespace esphome {
namespace thermia {
namespace hw {

constexpr uint32_t TICKS_PER_US = F_CPU / 1000000UL;

// One register read returns the level of all GPIO0-15 at the same instant.
static inline __attribute__((always_inline)) uint32_t read_bus() { return GPI; }

// Open-drain emulation: the output latch is kept at 0, we only toggle the output enable.
static inline __attribute__((always_inline)) void sda_low(uint32_t mask) { GPES = mask; }
static inline __attribute__((always_inline)) void sda_release(uint32_t mask) { GPEC = mask; }

static inline __attribute__((always_inline)) uint32_t ticks() {
  uint32_t ccount;
  __asm__ __volatile__("rsr %0, ccount" : "=a"(ccount));
  return ccount;
}

static inline __attribute__((always_inline)) void clear_irq(uint32_t mask) {
  GPIO_REG_WRITE(GPIO_STATUS_W1TC_ADDRESS, mask);
}

static inline uint32_t irq_lock() { return xt_rsil(15); }
static inline void irq_unlock(uint32_t state) { xt_wsr_ps(state); }

static inline void prepare_open_drain(uint32_t mask) {
  GPEC = mask;  // input
  GPOC = mask;  // output latch low, so enabling the output pulls the line down
}

}  // namespace hw
}  // namespace thermia
}  // namespace esphome

#endif  // THERMIA_HOST_TEST
