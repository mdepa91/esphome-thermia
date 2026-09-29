#pragma once

// Thin hardware layer for the bit-banged I2C slave. Everything here must be safe to call from an ISR.
// The host test (test/) provides its own thermia_sim_hw.h with the same interface.
//
// Interface every backend provides (namespace esphome::thermia::hw):
//   TICKS_PER_US                 cycle counter ticks per microsecond (constexpr)
//   pin_mask(pin)                bit of `pin` in the value returned by read_bus()
//   read_bus()                   level of all supported pins, sampled in one register read
//   sda_low(mask) / sda_release  drive SDA low / let the pull-up take it high (open drain)
//   ticks()                      free-running CPU cycle counter
//   clear_irq(mask)              drop pending GPIO interrupts caused by our own bus handling
//   irq_lock() / irq_unlock()    mask interrupts on the calling core
//   prepare_sda(pin, mask)       one-time setup of SDA for open-drain driving (after the pin itself is set up)
//
// Both lines must live in the first bank of 32 GPIOs (validated in __init__.py), so a single register read
// always gives SDA and SCL sampled at the same instant.

#include <stdint.h>

#ifdef THERMIA_HOST_TEST

#include "thermia_sim_hw.h"

#elif defined(USE_ESP8266)

#include <Arduino.h>

namespace esphome {
namespace thermia {
namespace hw {

constexpr uint32_t TICKS_PER_US = F_CPU / 1000000UL;

static inline __attribute__((always_inline)) uint32_t pin_mask(uint8_t pin) { return 1UL << pin; }

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

static inline void prepare_sda(uint8_t /*pin*/, uint32_t mask) {
  GPEC = mask;  // input
  GPOC = mask;  // output latch low, so enabling the output pulls the line down
}

}  // namespace hw
}  // namespace thermia
}  // namespace esphome

#elif defined(USE_ESP32)

// Works for every ESP32 variant (Xtensa: ESP32/S2/S3, RISC-V: C3/C6/H2/...) and both frameworks (esp-idf and
// Arduino, which sits on top of esp-idf). Only direct register access in the hot path: the esp-idf gpio_* driver
// functions do argument checking and are not guaranteed to be in IRAM.
//
// Unlike the ESP8266, the pad has a real open-drain driver: SDA is configured once as input + open-drain output,
// then "low" / "release" are just writes to the output latch (W1TC / W1TS), one APB store each.

#include <sdkconfig.h>
#include <driver/gpio.h>
#include <esp_cpu.h>
#include <freertos/FreeRTOS.h>
#include <soc/gpio_reg.h>
#include <soc/soc.h>

namespace esphome {
namespace thermia {
namespace hw {

// ESPHome sets this from `esp32: cpu_frequency:`; it does not enable dynamic frequency scaling, so the CPU runs
// at this speed the whole time.
constexpr uint32_t TICKS_PER_US = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;

static inline __attribute__((always_inline)) uint32_t pin_mask(uint8_t pin) { return 1UL << pin; }

// GPIO0-31 in one read (GPIO32+ would be a second register, which is why __init__.py rejects them).
static inline __attribute__((always_inline)) uint32_t read_bus() { return REG_READ(GPIO_IN_REG); }

static inline __attribute__((always_inline)) void sda_low(uint32_t mask) { REG_WRITE(GPIO_OUT_W1TC_REG, mask); }
static inline __attribute__((always_inline)) void sda_release(uint32_t mask) { REG_WRITE(GPIO_OUT_W1TS_REG, mask); }

static inline __attribute__((always_inline)) uint32_t ticks() { return (uint32_t) esp_cpu_get_cycle_count(); }

static inline __attribute__((always_inline)) void clear_irq(uint32_t mask) {
  REG_WRITE(GPIO_STATUS_W1TC_REG, mask);
}

// Only masks the calling core. That is enough: probe_sda() is the only user and runs before the ISR is attached.
static inline uint32_t irq_lock() { return (uint32_t) portSET_INTERRUPT_MASK_FROM_ISR(); }
static inline void irq_unlock(uint32_t state) { portCLEAR_INTERRUPT_MASK_FROM_ISR((UBaseType_t) state); }

static inline void prepare_sda(uint8_t pin, uint32_t mask) {
  REG_WRITE(GPIO_OUT_W1TS_REG, mask);  // latch high = released, before the driver is switched on
  gpio_set_direction((gpio_num_t) pin, GPIO_MODE_INPUT_OUTPUT_OD);
  gpio_set_pull_mode((gpio_num_t) pin, GPIO_PULLUP_ONLY);
}

}  // namespace hw
}  // namespace thermia
}  // namespace esphome

#else
#error "The thermia component supports ESP8266 and ESP32 only (it emulates an I2C slave in software)."
#endif
