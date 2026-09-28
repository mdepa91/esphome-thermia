#pragma once

// Software I2C *slave* speaking the Danfoss/Thermia "EXT" protocol.
//
// The heat pump is the I2C master (bit-banged, 7-bit slave address 0x2E). Every ~110 ms it:
//   1. writes a ping byte (0xFE, or 0xFD once after boot)            -> we ACK
//   2. reads one byte                                                 -> we answer with either
//        0xFF        "nothing to ask"
//        0x00-0x7F   "send me this register"
//        0x80-0xFF   "write value to register (reg & 0x7F)" - NEVER used here, this module is read-only
//   3. if a register was requested, writes [reg, lo, hi] to us        -> we ACK and store it
//
// Protocol reverse engineered by rainisto (https://github.com/rainisto/arduino_i2c_orja,
// https://omakotikotitalomme.blogspot.com/2015/03/danfoss-lampopumpun-salaisuudet.html).
//
// The ESP8266 has no hardware I2C slave, so a GPIO interrupt on SDA falling (START condition) enters
// on_sda_fall(), which then polls both lines and handles the whole transaction until STOP. Polling with
// the cycle counter instead of one interrupt per edge keeps us independent from WiFi interrupt jitter.

#include <stdint.h>
#include "thermia_hw.h"

#ifdef THERMIA_HOST_TEST
#define IRAM_ATTR
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
  static constexpr uint8_t MAX_TRIES = 3;  // how many pings we ask for a register before giving up on it
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

  // ---- setup (main context, before the interrupt is attached) ----

  void begin(uint8_t sda_pin, uint8_t scl_pin) {
    sda_ = 1UL << sda_pin;
    scl_ = 1UL << scl_pin;
    edge_timeout_ = 1500UL * hw::TICKS_PER_US;
    // Worst case legitimate transaction (address + MAX_READ_BYTES, each 9 bits) is ~81 bits; even at a bus twice
    // as slow as the ~20 kHz we measured that is ~8 ms. This is the hard ceiling on how long a single call can
    // busy-loop, so keep it close to that rather than generously large - every microsecond here is CPU time taken
    // away from everything else on this single-core chip, WiFi included (see thermia_bus_gate.h).
    max_transaction_ = 8000UL * hw::TICKS_PER_US;
    hw::prepare_open_drain(sda_);
  }

  void add_poll(uint8_t reg) {
    if (reg > MAX_REG || poll_n_ >= sizeof(poll_)) return;
    for (uint8_t i = 0; i < poll_n_; i++)
      if (poll_[i] == reg) return;
    poll_[poll_n_++] = reg;
  }

  void set_log_enabled(bool enabled) { log_enabled_ = enabled; }

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
  void set_sniff(bool enabled) { sniff_ = enabled; }
  bool trace_ready() const { return trace_ready_; }
  uint8_t trace_count() const { return trace_n_; }
  const TraceEv &trace_at(uint8_t i) const { return trace_[i]; }
  void trace_rearm() { trace_ready_ = false; }
  uint32_t ticks_per_us() const { return hw::TICKS_PER_US; }

  // ---- ISR entry point: SDA falling edge ----

  void IRAM_ATTR on_sda_fall() {
    if (guard_) {
      hw::clear_irq(sda_);
      return;
    }
    isr_calls_++;
    if (sniff_) {
      sniff();
      return;
    }
    const uint32_t t_begin = hw::ticks();

    uint8_t ev = EV_STOP;
    bool restart = true;
    while (restart) {
      restart = false;
      if (hw::ticks() - t_begin > max_transaction_ || !wait_scl(false)) {
        ev = EV_TIMEOUT;
        break;
      }
      uint8_t addr = 0;
      ev = rx_byte(addr);
      if (ev == EV_START) {  // START directly followed by another START
        restart = true;
        continue;
      }
      if (ev != RX_OK)
        break;

      if ((addr >> 1) != SLAVE_ADDR) {
        mismatch_++;
        log_frame('X', &addr, 1);
        ev = skip_to_end();
        restart = (ev == EV_START);
        continue;
      }

      if (!send_ack()) {
        ev = EV_TIMEOUT;
        break;
      }

      if (addr & 1) {
        // Master reads from us: this is where we tell it what we want. Either "please send me register N"
        // (a single byte, 0x00-0x7F), "please WRITE this value to register N" (three bytes: N|0x80, lo, hi -
        // see queue_write()), or "nothing" (0xFF). Bytes past what we intend are filler (RESP_IDLE) in case the
        // master reads more than expected.
        const bool answering_write = write_pending_;
        uint8_t resp[3];
        uint8_t resp_len;
        if (answering_write) {
          resp[0] = write_reg_ | 0x80;
          resp[1] = write_value_ & 0xFF;
          resp[2] = write_value_ >> 8;
          resp_len = 3;
        } else {
          resp[0] = (req_reg_ <= MAX_REG) ? req_reg_ : RESP_IDLE;
          resp_len = 1;
        }
        uint8_t r = RX_OK;
        uint8_t sent = 0;
        for (uint8_t i = 0; i < MAX_READ_BYTES; i++) {
          r = tx_byte(i < resp_len ? resp[i] : RESP_IDLE);
          if (r == RX_OK) {
            sent = i + 1;
            continue;
          }
          if (r == BIT_HIGH)  // NACK: master confirms it received this byte and wants no more
            sent = i + 1;
          break;  // NACK, timeout, or bus event: either way this transaction's read side is done
        }
        if (r == BIT_HIGH)
          r = skip_to_end();
        reads_++;
        log_frame('R', resp, resp_len);
        if (answering_write)
          on_write_response_done(sent >= resp_len);
        ev = r;
        restart = (ev == EV_START);
      } else {
        // Master writes to us: collect bytes until STOP / repeated START.
        uint8_t buf[MSG_MAX];
        uint8_t n = 0;
        for (;;) {
          uint8_t byte = 0;
          ev = rx_byte(byte);
          if (ev != RX_OK)
            break;
          if (n < MSG_MAX)
            buf[n] = byte;
          if (n < 255)
            n++;
          if (!send_ack()) {
            ev = EV_TIMEOUT;
            break;
          }
          if (hw::ticks() - t_begin > max_transaction_) {
            ev = EV_TIMEOUT;
            break;
          }
        }
        if (ev == EV_START || ev == EV_STOP)
          on_message(buf, n < MSG_MAX ? n : MSG_MAX, n);
        restart = (ev == EV_START);
      }
    }

    hw::sda_release(sda_);
    hw::clear_irq(sda_);  // edges caused by the transaction we just handled must not re-trigger us

    if (ev == EV_STOP) {
      transactions_++;
      fail_streak_ = 0;
    } else {
      errors_++;
      const uint8_t code = ev;
      log_frame('E', &code, 1);
      if (++fail_streak_ >= FAIL_STREAK_LIMIT)
        guard_ = true;
    }
    const uint32_t dt = hw::ticks() - t_begin;
    if (dt > max_isr_ticks_)
      max_isr_ticks_ = dt;
  }

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
  bool IRAM_ATTR wait_scl(bool high) {
    const uint32_t t0 = hw::ticks();
    for (;;) {
      if (((hw::read_bus() & scl_) != 0) == high)
        return true;
      if (hw::ticks() - t0 > edge_timeout_)
        return false;
    }
  }

  // Receive one bit. Precondition: SCL low. Returns BIT_LOW/BIT_HIGH with SCL low again, or EV_*.
  uint8_t IRAM_ATTR rx_bit() {
    uint32_t b;
    uint32_t t0 = hw::ticks();
    while (!((b = hw::read_bus()) & scl_)) {
      if (hw::ticks() - t0 > edge_timeout_)
        return EV_TIMEOUT;
    }
    const bool v = (b & sda_) != 0;  // sampled on SCL rising edge
    t0 = hw::ticks();
    for (;;) {
      b = hw::read_bus();
      if (!(b & scl_))
        return v ? BIT_HIGH : BIT_LOW;
      if (((b & sda_) != 0) != v)  // SDA moved while SCL is high: START or STOP
        return v ? EV_START : EV_STOP;
      if (hw::ticks() - t0 > edge_timeout_)
        return EV_TIMEOUT;
    }
  }

  uint8_t IRAM_ATTR rx_byte(uint8_t &out) {
    uint8_t v = 0;
    for (uint8_t i = 0; i < 8; i++) {
      const uint8_t r = rx_bit();
      if (r > BIT_HIGH)
        return r;
      v = (v << 1) | r;
    }
    out = v;
    return RX_OK;
  }

  // Pull SDA low during the 9th clock. Precondition: SCL low.
  bool IRAM_ATTR send_ack() {
    hw::sda_low(sda_);
    const bool ok = wait_scl(true) && wait_scl(false);
    hw::sda_release(sda_);
    return ok;
  }

  // Send one byte and read the master's ACK. Precondition: SCL low.
  // Returns RX_OK (= ACK, master wants more), BIT_HIGH (NACK) or EV_*.
  uint8_t IRAM_ATTR tx_byte(uint8_t value) {
    for (int8_t i = 7; i >= 0; i--) {
      if (value & (1 << i))
        hw::sda_release(sda_);
      else
        hw::sda_low(sda_);
      if (!wait_scl(true) || !wait_scl(false)) {
        hw::sda_release(sda_);
        return EV_TIMEOUT;
      }
    }
    hw::sda_release(sda_);
    return rx_bit();
  }

  // Ignore everything until STOP / START / timeout. Precondition: SCL low.
  uint8_t IRAM_ATTR skip_to_end() {
    for (uint16_t i = 0; i < 512; i++) {
      const uint8_t r = rx_bit();
      if (r > BIT_HIGH)
        return r;
    }
    return EV_TIMEOUT;
  }

  // Record every change of SDA/SCL after a falling edge on either line (both are wired to this ISR in sniff mode), until the bus is quiet for a while or the buffer is
  // full. Purely passive: used to find out what the pump really does on the wires (clock speed, which line is
  // which, whether a line is stuck).
  void IRAM_ATTR sniff() {
    if (trace_ready_) {  // main loop has not printed the previous capture yet
      hw::clear_irq(sda_ | scl_);
      return;
    }
    const uint32_t mask = sda_ | scl_;
    const uint32_t quiet = 1500UL * hw::TICKS_PER_US;
    const uint32_t window = 6000UL * hw::TICKS_PER_US;
    const uint32_t t0 = hw::ticks();
    uint32_t t_last = t0;
    uint32_t last = hw::read_bus() & mask;
    uint8_t n = 0;
    trace_[n].dt_ticks = 0;
    trace_[n].sda = (last & sda_) != 0;
    trace_[n].scl = (last & scl_) != 0;
    n++;
    while (n < TRACE_MAX) {
      const uint32_t b = hw::read_bus() & mask;
      const uint32_t now = hw::ticks();
      if (b != last) {
        trace_[n].dt_ticks = now - t_last;
        trace_[n].sda = (b & sda_) != 0;
        trace_[n].scl = (b & scl_) != 0;
        n++;
        last = b;
        t_last = now;
      } else if (now - t_last > quiet || now - t0 > window) {
        break;
      }
    }
    trace_n_ = n;
    trace_ready_ = true;
    hw::clear_irq(sda_ | scl_);
  }

  // ---- protocol ----

  void IRAM_ATTR on_message(const uint8_t *buf, uint8_t stored, uint8_t total) {
    if (total == 0)
      return;
    log_frame('W', buf, stored);
    const uint8_t cmd = buf[0];
    if (cmd == CMD_PING || cmd == CMD_PING_BOOT) {
      on_ping();
    } else if (cmd <= MAX_REG && total == 3) {
      on_data(cmd, (uint16_t) buf[1] | ((uint16_t) buf[2] << 8));
    } else {
      unknown_++;
    }
  }

  void IRAM_ATTR on_ping() {
    pings_++;
    if (poll_n_ == 0)
      return;
    if (req_reg_ != RESP_IDLE) {  // previous request was not answered
      if (++req_tries_ >= MAX_TRIES) {
        unanswered_++;
        advance();
      }
    }
    if (req_reg_ == RESP_IDLE) {
      req_reg_ = poll_[poll_idx_];
      req_tries_ = 0;
    }
  }

  void IRAM_ATTR on_data(uint8_t reg, uint16_t value) {
    data_frames_++;
    uint16_t seq = (slot_[reg] >> 16) + 1;
    if (seq == 0)
      seq = 1;
    slot_[reg] = ((uint32_t) seq << 16) | value;
    if (reg == req_reg_)
      advance();
  }

  void IRAM_ATTR advance() {
    req_reg_ = RESP_IDLE;
    req_tries_ = 0;
    if (++poll_idx_ >= poll_n_)
      poll_idx_ = 0;
  }

  // Called right after we offered a pending write as the response to a master read. `delivered` means the master
  // read all 3 bytes of our response (reg|0x80, lo, hi) without a bus error - not that the pump necessarily acted
  // on it, just that it heard us. Give up after WRITE_MAX_TRIES offers with no clean delivery.
  void IRAM_ATTR on_write_response_done(bool delivered) {
    if (delivered) {
      write_pending_ = false;
      writes_delivered_++;
    } else if (++write_tries_ >= WRITE_MAX_TRIES) {
      write_pending_ = false;
      writes_failed_++;
    }
  }

  void IRAM_ATTR log_frame(char kind, const uint8_t *d, uint8_t len) {
    if (!log_enabled_)
      return;
    const uint8_t next = (log_head_ + 1) % LOG_SIZE;
    if (next == log_tail_)
      return;  // full, drop
    FrameLog &f = log_[log_head_];
    f.kind = kind;
    f.len = len > 4 ? 4 : len;
    for (uint8_t i = 0; i < f.len; i++)
      f.data[i] = d[i];
    log_head_ = next;
  }

  uint32_t sda_{0}, scl_{0};
  uint32_t edge_timeout_{0}, max_transaction_{0};

  uint8_t poll_[128];
  uint8_t poll_n_{0};
  volatile uint8_t poll_idx_{0};
  volatile uint8_t req_reg_{RESP_IDLE};
  volatile uint8_t req_tries_{0};

  volatile uint32_t slot_[MAX_REG + 1]{};

  volatile bool write_pending_{false};
  volatile uint8_t write_reg_{0};
  volatile uint16_t write_value_{0};
  volatile uint8_t write_tries_{0};
  volatile uint32_t writes_delivered_{0}, writes_failed_{0};

  volatile uint32_t pings_{0}, data_frames_{0}, reads_{0}, transactions_{0}, errors_{0};
  volatile uint32_t mismatch_{0}, unknown_{0}, unanswered_{0}, isr_calls_{0}, max_isr_ticks_{0};
  volatile uint8_t fail_streak_{0};
  volatile bool guard_{false};

  bool sniff_{false};
  TraceEv trace_[TRACE_MAX];
  volatile uint8_t trace_n_{0};
  volatile bool trace_ready_{false};

  bool log_enabled_{false};
  FrameLog log_[LOG_SIZE];
  volatile uint8_t log_head_{0}, log_tail_{0};
};

}  // namespace thermia
}  // namespace esphome
