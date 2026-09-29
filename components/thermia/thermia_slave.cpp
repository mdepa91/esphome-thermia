// Bus/protocol code of ThermiaSlave that runs inside the GPIO interrupt. It lives out of line, in its own
// translation unit, on purpose: IRAM_ATTR on functions defined inline in a header ends up in COMDAT sections whose
// literal pools the ESP32 (Xtensa) linker places after the code, failing with "l32r: literal placed after use".

#include "thermia_slave.h"

namespace esphome {
namespace thermia {

void IRAM_ATTR ThermiaSlave::on_sda_fall() {
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
  t_begin_ = t_begin;

  uint8_t ev = EV_STOP;
  bool restart = true;
  while (restart) {
    restart = false;
    if (expired() || !wait_scl(false)) {
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
          if (expired()) {
            r = EV_TIMEOUT;
            break;
          }
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
        if (expired()) {
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

bool IRAM_ATTR ThermiaSlave::wait_scl(bool high) {
  const uint32_t t0 = hw::ticks();
  for (;;) {
    if (((hw::read_bus() & scl_) != 0) == high)
      return true;
    if (hw::ticks() - t0 > edge_timeout_)
      return false;
  }
}

uint8_t IRAM_ATTR ThermiaSlave::rx_bit() {
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

uint8_t IRAM_ATTR ThermiaSlave::rx_byte(uint8_t &out) {
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

bool IRAM_ATTR ThermiaSlave::send_ack() {
  hw::sda_low(sda_);
  const bool ok = wait_scl(true) && wait_scl(false);
  hw::sda_release(sda_);
  return ok;
}

uint8_t IRAM_ATTR ThermiaSlave::tx_byte(uint8_t value) {
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

uint8_t IRAM_ATTR ThermiaSlave::skip_to_end() {
  for (uint16_t i = 0; i < 512 && !expired(); i++) {
    const uint8_t r = rx_bit();
    if (r > BIT_HIGH)
      return r;
  }
  return EV_TIMEOUT;
}

void IRAM_ATTR ThermiaSlave::sniff() {
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

void IRAM_ATTR ThermiaSlave::on_message(const uint8_t *buf, uint8_t stored, uint8_t total) {
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

void IRAM_ATTR ThermiaSlave::on_ping() {
  pings_++;
  if (poll_n_ == 0)
    return;
  if (req_reg_ != RESP_IDLE) {  // previous request was not answered
    if (++req_tries_ >= request_tries_) {
      unanswered_++;
      advance();
    }
  }
  if (req_reg_ == RESP_IDLE) {
    req_reg_ = poll_[poll_idx_];
    req_tries_ = 0;
  }
}

void IRAM_ATTR ThermiaSlave::on_data(uint8_t reg, uint16_t value) {
  data_frames_++;
  uint16_t seq = (slot_[reg] >> 16) + 1;
  if (seq == 0)
    seq = 1;
  slot_[reg] = ((uint32_t) seq << 16) | value;
  superseded_[reg] = false;
  if (reg == req_reg_)
    advance();
}

void IRAM_ATTR ThermiaSlave::advance() {
  req_reg_ = RESP_IDLE;
  req_tries_ = 0;
  if (++poll_idx_ >= poll_n_)
    poll_idx_ = 0;
}

void IRAM_ATTR ThermiaSlave::on_write_response_done(bool delivered) {
  if (delivered) {
    write_pending_ = false;
    superseded_[write_reg_] = true;
    writes_delivered_++;
  } else if (++write_tries_ >= WRITE_MAX_TRIES) {
    write_pending_ = false;
    writes_failed_++;
  }
}

void IRAM_ATTR ThermiaSlave::log_frame(char kind, const uint8_t *d, uint8_t len) {
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

}  // namespace thermia
}  // namespace esphome
