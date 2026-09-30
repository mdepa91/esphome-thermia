#pragma once

// Software I2C *slave* speaking the Danfoss/Thermia "EXT" protocol.
//
// The heat pump is the I2C master (bit-banged, 7-bit slave address 0x2E). Every ~110 ms it:
//   1. writes a ping byte (0xFE, or 0xFD once after boot)            -> we ACK
//   2. reads one byte                                                 -> we answer with either
//        0xFF        "nothing to ask"
//        0x00-0x7F   "send me this register"
//        0x80-0xFF   "write value to register (reg & 0x7F)" - followed by lo, hi (see queue_write())
//   3. if a register was requested, writes [reg, lo, hi] to us        -> we ACK and store it
//
// Protocol reverse engineered by rainisto (https://github.com/rainisto/arduino_i2c_orja,
// https://omakotikotitalomme.blogspot.com/2015/03/danfoss-lampopumpun-salaisuudet.html). Used as a protocol
// reference only - this is an independent implementation, no code was copied.
//
// The ESP8266 has no hardware I2C slave, so a GPIO interrupt on SDA falling (START condition) enters
// on_sda_fall(), which then polls both lines and handles the whole transaction until STOP. Polling with
// the cycle counter instead of one interrupt per edge keeps us independent from WiFi interrupt jitter.
//
// The ESP32 runs the same engine (see thermia_hw.h for the per-platform register access). Its hardware I2C
// slave is deliberately not used: the answer to the pump's read depends on the ping that came just before it,
// the peripheral would have to have it in its TX FIFO already, and sniff/selftest need raw pin access anyway.
// On a dual-core ESP32 the busy-polling costs nothing that matters: ESPHome runs its loop (and therefore
// installs the GPIO ISR) on core 1, while WiFi/lwIP live on core 0.

#include <stdint.h>
#include "thermia_hw.h"

#ifdef THERMIA_HOST_TEST
#define IRAM_ATTR
#else
#include "esphome/core/hal.h"  // IRAM_ATTR
#endif

namespace esphome {
namespace thermia {

class ThermiaSlave {
 public:
  static constexpr uint8_t SLAVE_ADDR = 0x2E;
  static constexpr uint8_t CMD_PING_BOOT = 0xFD;
  static constexpr uint8_t CMD_PING = 0xFE;
  static constexpr uint8_t RESP_IDLE = 0xFF;
  static constexpr uint8_t MAX_REG = 0x7F;
  static constexpr uint8_t DEFAULT_REQUEST_TRIES = 50;  // how many pings we ask for a register before giving up on it
  static constexpr uint8_t WRITE_MAX_TRIES = 3;  // how many read-responses we offer a pending write before giving up
  static constexpr uint8_t FAIL_STREAK_LIMIT = 50;  // consecutive failed transactions before the ISR pauses itself

  struct FrameLog {
    char kind;  // 'W' pump wrote to us, 'R' pump read from us, 'X' other address, 'E' error
    uint8_t len;
    uint8_t data[4];
  };

  // One recorded bus change: time since the previous change (in CPU ticks) and the levels after it.
  struct TraceEv {
    uint32_t dt_ticks;
    uint8_t sda;
    uint8_t scl;
  };
  static constexpr uint8_t TRACE_MAX = 128;

  // Result of probe_sda(): what the SDA line does when we pull it down ourselves.
  struct ProbeResult {
    bool idle_high;      // level before we touched the line
    bool low_reads_low;  // while driven low, it reads low (false: hard-tied high / short)
    bool released_high;  // it came back high after release (false: something holds it low)
    uint32_t rise_ns;    // worst time until it read high again (capped at 2 ms)
  };

  // ---- setup (main context, after the pins are set up, before the interrupt is attached) ----

  void begin(uint8_t sda_pin, uint8_t scl_pin) {
    sda_ = hw::pin_mask(sda_pin);
    scl_ = hw::pin_mask(scl_pin);
    edge_timeout_ = 1500UL * hw::TICKS_PER_US;
    // Worst case legitimate transaction (address + MAX_READ_BYTES, each 9 bits) is ~81 bits; even at a bus twice
    // as slow as the ~20 kHz we measured that is ~8 ms. This is the hard ceiling on how long a single call can
    // busy-loop, so keep it close to that rather than generously large - every microsecond here is CPU time taken
    // away from everything else on this single-core chip (ESP8266, ESP32-C3/C6/S2), WiFi included (see thermia_bus_gate.h).
    max_transaction_ = 8000UL * hw::TICKS_PER_US;
    hw::prepare_sda(sda_pin, sda_);
  }

  void add_poll(uint8_t reg) {
    if (reg > MAX_REG || poll_n_ >= sizeof(poll_)) return;
    for (uint8_t i = 0; i < poll_n_; i++)
      if (poll_[i] == reg) return;
    poll_[poll_n_++] = reg;
  }

  void set_log_enabled(bool enabled) { log_enabled_ = enabled; }
  // How many consecutive pings we keep asking for the same register before moving on to the next one. The pump
  // answers most registers on the first ping, but some (r00/r01, r32/r34 on a Thermia Duo) only after a longer
  // wait: with 3 tries they never arrived at all, with 50 they arrive every cycle and unanswered pings dropped from
  // ~25% to ~0.5%. The original Arduino bridge (rainisto) keeps asking for the same register until it arrives.
  void set_request_tries(uint8_t tries) { request_tries_ = tries ? tries : 1; }

  // Ask the pump to write `value` to `reg` the next time it reads from us (main context only, e.g. from a
  // number::Number's control()). Overwrites any write that is still pending (last one wins) - this is a UI-driven
  // control channel, not a queue. There is no protocol-level confirmation that the pump *applied* the value, only
  // that it *received* our 3-byte response without a bus error (writes_delivered()); the way to be sure it took
  // effect is to also poll the same register for reading (add_poll) and compare.
  bool queue_write(uint8_t reg, uint16_t value) {
    if (reg > MAX_REG)
      return false;
    write_value_ = value;
    write_reg_ = reg;      // data first, flag last - the ISR only trusts reg_/value_ once it sees the flag
    write_tries_ = 0;
    write_pending_ = true;
    return true;
  }
  bool write_pending() const { return write_pending_; }
  // True from the moment a write to `reg` is delivered until the pump next reports that register: the value in
  // slot(reg) predates our write, so it must not be used to "correct" what the UI shows. The pump may answer a
  // given register only every few minutes, and republishing the pre-write value meanwhile looks like a revert.
  bool superseded(uint8_t reg) const { return superseded_[reg & MAX_REG]; }
  uint32_t writes_delivered() const { return writes_delivered_; }
  uint32_t writes_failed() const { return writes_failed_; }

  // Drive SDA low for ~30 us, three times, and measure how it recovers. Main context only, and before the
  // interrupt is attached. Only ever touches SDA (never SCL, which is driven by the pump).
  ProbeResult probe_sda() {
    ProbeResult r{};
    const uint32_t tpu = hw::TICKS_PER_US;
    r.idle_high = (hw::read_bus() & sda_) != 0;
    bool low_ok = true, high_ok = true;
    uint32_t worst = 0;
    for (uint8_t i = 0; i < 3; i++) {
      const uint32_t irq = hw::irq_lock();
      hw::sda_low(sda_);
      uint32_t t0 = hw::ticks();
      while (hw::ticks() - t0 < 30 * tpu)
        hw::read_bus();
      const bool low = (hw::read_bus() & sda_) == 0;
      hw::sda_release(sda_);
      t0 = hw::ticks();
      bool high;
      uint32_t dt;
      do {
        high = (hw::read_bus() & sda_) != 0;
        dt = hw::ticks() - t0;
      } while (!high && dt < 2000 * tpu);
      hw::irq_unlock(irq);
      hw::clear_irq(sda_);  // our own edge must not count as pump traffic
      low_ok = low_ok && low;
      high_ok = high_ok && high;
      if (dt > worst)
        worst = dt;
      t0 = hw::ticks();
      while (hw::ticks() - t0 < 300 * tpu)
        hw::read_bus();
    }
    r.low_reads_low = low_ok;
    r.released_high = high_ok;
    r.rise_ns = worst * 1000UL / tpu;
    return r;
  }

  // Sniff mode: the ISR only records the raw SDA/SCL waveform (never drives the bus), see sniff().
  // The 1 KB capture buffer is only allocated when sniffing is enabled (main context, before the ISR is attached);
  // on the ESP8266 that is a noticeable slice of the heap that normal operation never needs.
  void set_sniff(bool enabled) {
    if (enabled && trace_ == nullptr)
      trace_ = new TraceEv[TRACE_MAX];  // NOLINT(cppcoreguidelines-owning-memory) - lives as long as the component
    sniff_ = enabled && trace_ != nullptr;
  }
  bool trace_ready() const { return trace_ready_; }
  uint8_t trace_count() const { return trace_n_; }
  const TraceEv &trace_at(uint8_t i) const { return trace_[i]; }
  void trace_rearm() { trace_ready_ = false; }
  uint32_t ticks_per_us() const { return hw::TICKS_PER_US; }

  // ---- ISR entry point: SDA falling edge (definitions of all ISR code: thermia_slave.cpp) ----

  void on_sda_fall();

  // ---- accessors for the main loop ----

  // (seq << 16) | value. seq == 0 means the register was never received.
  uint32_t slot(uint8_t reg) const { return slot_[reg & MAX_REG]; }

  uint32_t pings() const { return pings_; }
  uint32_t data_frames() const { return data_frames_; }
  uint32_t reads() const { return reads_; }
  uint32_t transactions() const { return transactions_; }
  uint32_t errors() const { return errors_; }
  uint32_t mismatches() const { return mismatch_; }
  uint32_t unknown_messages() const { return unknown_; }
  uint32_t unanswered() const { return unanswered_; }
  uint32_t isr_calls() const { return isr_calls_; }
  uint32_t max_isr_us() const { return max_isr_ticks_ / hw::TICKS_PER_US; }
  uint8_t poll_count() const { return poll_n_; }
  uint8_t poll_at(uint8_t i) const { return poll_[i]; }

  bool guard_tripped() const { return guard_; }
  void reset_guard() {
    fail_streak_ = 0;
    guard_ = false;
  }

  bool pop_log(FrameLog &out) {
    if (log_tail_ == log_head_)
      return false;
    out = log_[log_tail_];
    log_tail_ = (log_tail_ + 1) % LOG_SIZE;
    return true;
  }

 protected:
  static constexpr uint8_t MSG_MAX = 4;
  static constexpr uint8_t MAX_READ_BYTES = 8;
  static constexpr uint8_t LOG_SIZE = 32;

  // rx_bit()/rx_byte()/tx_byte() results. Values 0/1 are data bits (or ACK/NACK).
  static constexpr uint8_t RX_OK = 0;
  static constexpr uint8_t BIT_LOW = 0;
  static constexpr uint8_t BIT_HIGH = 1;
  static constexpr uint8_t EV_START = 2;    // SDA fell while SCL high
  static constexpr uint8_t EV_STOP = 3;     // SDA rose while SCL high
  static constexpr uint8_t EV_TIMEOUT = 4;  // the bus stopped moving

  // ---- bus primitives (all polled) ----

  // Wait for SCL to reach `high`. False on timeout.
  bool wait_scl(bool high);

  // Receive one bit. Precondition: SCL low. Returns BIT_LOW/BIT_HIGH with SCL low again, or EV_*.
  uint8_t rx_bit();
  uint8_t rx_byte(uint8_t &out);

  // Pull SDA low during the 9th clock. Precondition: SCL low.
  bool send_ack();

  // Send one byte and read the master's ACK. Precondition: SCL low.
  // Returns RX_OK (= ACK, master wants more), BIT_HIGH (NACK) or EV_*.
  uint8_t tx_byte(uint8_t value);

  // True once the current ISR call has used up its budget (max_transaction_). Every loop that can follow the bus
  // for more than a few bits checks it, so one call never masks interrupts for much longer than that - the
  // per-edge timeout alone would let a noisy bus with a slow, never-ending clock keep us here for seconds.
  bool expired() const { return hw::ticks() - t_begin_ > max_transaction_; }

  // Ignore everything until STOP / START / timeout. Precondition: SCL low.
  uint8_t skip_to_end();

  // Record every change of SDA/SCL after a falling edge on either line (both are wired to this ISR in sniff mode), until the bus is quiet for a while or the buffer is
  // full. Purely passive: used to find out what the pump really does on the wires (clock speed, which line is
  // which, whether a line is stuck).
  void sniff();

  // ---- protocol ----

  void on_message(const uint8_t *buf, uint8_t stored, uint8_t total);
  void on_ping();
  void on_data(uint8_t reg, uint16_t value);
  void advance();

  // Called right after we offered a pending write as the response to a master read. `delivered` means the master
  // read all 3 bytes of our response (reg|0x80, lo, hi) without a bus error - not that the pump necessarily acted
  // on it, just that it heard us. Give up after WRITE_MAX_TRIES offers with no clean delivery.
  void on_write_response_done(bool delivered);

  void log_frame(char kind, const uint8_t *d, uint8_t len);

  uint32_t sda_{0}, scl_{0};
  uint32_t edge_timeout_{0}, max_transaction_{0};
  uint32_t t_begin_{0};  // hw::ticks() when the current ISR call started

  uint8_t poll_[128];
  uint8_t poll_n_{0};
  volatile uint8_t poll_idx_{0};
  volatile uint8_t req_reg_{RESP_IDLE};
  volatile uint8_t req_tries_{0};
  uint8_t request_tries_{DEFAULT_REQUEST_TRIES};

  volatile uint32_t slot_[MAX_REG + 1]{};

  volatile bool write_pending_{false};
  volatile uint8_t write_reg_{0};
  volatile uint16_t write_value_{0};
  volatile uint8_t write_tries_{0};
  volatile uint32_t writes_delivered_{0}, writes_failed_{0};
  volatile bool superseded_[MAX_REG + 1]{};

  volatile uint32_t pings_{0}, data_frames_{0}, reads_{0}, transactions_{0}, errors_{0};
  volatile uint32_t mismatch_{0}, unknown_{0}, unanswered_{0}, isr_calls_{0}, max_isr_ticks_{0};
  volatile uint8_t fail_streak_{0};
  volatile bool guard_{false};

  bool sniff_{false};
  TraceEv *trace_{nullptr};
  volatile uint8_t trace_n_{0};
  volatile bool trace_ready_{false};

  bool log_enabled_{false};
  FrameLog log_[LOG_SIZE];
  volatile uint8_t log_head_{0}, log_tail_{0};
};

}  // namespace thermia
}  // namespace esphome
