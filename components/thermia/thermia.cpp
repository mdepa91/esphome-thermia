#include "thermia.h"

#include <cmath>
#include <cstdio>

#include "esphome/core/log.h"

#include "thermia_number.h"
#include "thermia_select.h"

#ifdef USE_NETWORK
#include "esphome/components/network/util.h"
#endif

namespace esphome {
namespace thermia {

static const char *const TAG = "thermia";

// This only queues the write for the next time the pump reads from us (see ThermiaSlave::queue_write) - it is not
// a confirmation the pump applied it. We publish_state() right away for a responsive UI, but the state gets
// corrected on the next real reading of the register if the pump disagrees (rejects, clamps, or ignores it).
void ThermiaNumber::control(float value) {
  this->publish_state(value);
  if (this->parent_ == nullptr)
    return;
  const float raw_f = lroundf(value / this->scale_);
  this->parent_->queue_write(this->reg_, this->signed_ ? (uint16_t) (int16_t) raw_f : (uint16_t) raw_f);
}

void ThermiaSelect::control(size_t index) {
  this->publish_state(index);
  if (this->parent_ == nullptr)
    return;
  this->parent_->queue_write(this->reg_, (uint16_t) index);
}

// The pump pings us every ~110 ms; if it has been quiet this long the link is considered down.
static const uint32_t LINK_TIMEOUT_MS = 5000;
// After the ISR guard trips (bus is permanently broken / swapped lines) wait this long before retrying.
static const uint32_t GUARD_RETRY_MS = 10000;
// Debug frame log: print at most this often, regardless of how many entries piled up (a noisy bus must not turn
// into a flood of synchronous serial writes that starves everything else, WiFi included).
static const uint32_t LOG_DRAIN_INTERVAL_MS = 50;

#ifdef USE_NETWORK
static bool network_is_connected() { return network::is_connected(); }
#else
static bool network_is_connected() { return true; }  // no network component configured - do not gate on it
#endif

void IRAM_ATTR ThermiaComponent::isr_(ThermiaComponent *self) { self->slave_.on_sda_fall(); }

void ThermiaComponent::setup() {
  for (uint8_t reg : this->registers_)
    this->slave_.add_poll(reg);

  // Both pins come in as input + pull-up from the YAML schema; begin() then turns SDA into an open-drain output
  // the platform-specific way (see thermia_hw.h), so it must run after the pins' own setup().
  this->sda_pin_->setup();
  this->scl_pin_->setup();
  this->slave_.begin(this->sda_pin_->get_pin(), this->scl_pin_->get_pin());
  this->slave_.set_log_enabled(this->debug_frames_);
  this->slave_.set_sniff(this->sniff_);
  if (this->selftest_)
    this->probe_ = this->slave_.probe_sda();  // harmless at boot, done before anything is attached

  // The interrupt is NOT attached here. See update_bus_gate_(): it only starts once the network is confirmed up,
  // so the very first WiFi connection attempt is never competing with pump traffic for the CPU.
  ESP_LOGI(TAG, "Waiting for the network before talking to the heat pump");
}

void ThermiaComponent::update_bus_gate_(uint32_t now) {
  const bool should_be_attached = this->bus_gate_.update(network_is_connected(), now);
  if (should_be_attached == this->bus_attached_)
    return;
  this->bus_attached_ = should_be_attached;
  if (should_be_attached) {
    this->sda_pin_->attach_interrupt(ThermiaComponent::isr_, this, gpio::INTERRUPT_FALLING_EDGE);
    if (this->sniff_)  // sniffer wakes up on either line, so it works whichever way round they are wired
      this->scl_pin_->attach_interrupt(ThermiaComponent::isr_, this, gpio::INTERRUPT_FALLING_EDGE);
    ESP_LOGI(TAG, "Network is up - starting to talk to the heat pump");
  } else {
    this->sda_pin_->detach_interrupt();
    if (this->sniff_)
      this->scl_pin_->detach_interrupt();
    ESP_LOGW(TAG, "Network is down - pausing bus handling so it does not get in the way of reconnecting");
    this->last_ping_ms_ = 0;  // link is down regardless of what the (now frozen) ISR counters still say
  }
}

void ThermiaComponent::loop() {
  const uint32_t now = millis();

  this->update_bus_gate_(now);

  if (this->slave_.guard_tripped()) {
    if (this->guard_since_ms_ == 0) {
      this->guard_since_ms_ = now ? now : 1;
      ESP_LOGW(TAG, "Bus looks broken (%u consecutive failed transactions) - pausing for %us. Are SDA and SCL swapped?",
               (unsigned) ThermiaSlave::FAIL_STREAK_LIMIT, (unsigned) (GUARD_RETRY_MS / 1000));
    } else if (now - this->guard_since_ms_ > GUARD_RETRY_MS) {
      this->guard_since_ms_ = 0;
      this->slave_.reset_guard();
    }
  }

  if (now - this->last_log_drain_ms_ >= LOG_DRAIN_INTERVAL_MS) {
    this->last_log_drain_ms_ = now;
    this->drain_frame_log_();
  }
  if (this->sniff_)
    this->dump_trace_();
  if (this->selftest_)
    this->watch_pins_(now);

  if (now - this->last_scan_ms_ >= 100) {
    this->last_scan_ms_ = now;
    this->refresh_timestamps_(now);
    this->update_link_(now);
  }
}

void ThermiaComponent::drain_frame_log_() {
  ThermiaSlave::FrameLog f;
  for (uint8_t n = 0; n < 8 && this->slave_.pop_log(f); n++) {
    char hex[16] = "";
    for (uint8_t i = 0; i < f.len; i++)
      snprintf(hex + i * 3, sizeof(hex) - i * 3, "%02X ", f.data[i]);
    switch (f.kind) {
      case 'W':
        ESP_LOGD(TAG, "pump -> us : %s", hex);
        break;
      case 'R':
        ESP_LOGD(TAG, "us -> pump : %s", hex);
        break;
      case 'X':
        ESP_LOGD(TAG, "foreign address byte: %s", hex);
        break;
      default:
        ESP_LOGD(TAG, "bus error code: %s", hex);
        break;
    }
  }
}

// Selftest: report every change of the SDA pin level right away (rate limited) plus a status line every 2 s.
// SDA should sit high while the pump is silent; touching the wire to GND must show up here.
void ThermiaComponent::watch_pins_(uint32_t now) {
  const int sda = this->sda_pin_->digital_read();
  if (sda != this->watch_last_sda_) {
    this->watch_last_sda_ = sda;
    this->watch_changes_++;
    if (now - this->watch_last_log_ms_ >= 100) {
      this->watch_last_log_ms_ = now;
      ESP_LOGI(TAG, ">>> SDA pin GPIO%u went %s (change #%u)", this->sda_pin_->get_pin(), sda ? "HIGH" : "LOW",
               (unsigned) this->watch_changes_);
    }
  }
  if (now - this->watch_status_ms_ >= 2000) {
    this->watch_status_ms_ = now;
    ESP_LOGI(TAG, "pins now: SDA(GPIO%u)=%d SCL(GPIO%u)=%d | SDA changes seen: %u | edge interrupts: %u",
             this->sda_pin_->get_pin(), sda, this->scl_pin_->get_pin(), this->scl_pin_->digital_read(),
             (unsigned) this->watch_changes_,
             (unsigned) this->slave_.isr_calls());
  }
}

// Print one captured waveform per second: "<microseconds since previous change>:<SDA><SCL>" after each change.
void ThermiaComponent::dump_trace_() {
  if (!this->slave_.trace_ready())
    return;
  const uint32_t now = millis();
  if (now - this->last_trace_ms_ < 1000)
    return;  // stay armed-off for a moment so we do not flood the log
  this->last_trace_ms_ = now;

  const uint8_t n = this->slave_.trace_count();
  const uint32_t tpu = this->slave_.ticks_per_us();
  ESP_LOGD(TAG, "sniff #%u: %u changes, first state SDA=%u SCL=%u (items are <us since previous>:<SDA><SCL>)",
           (unsigned) ++this->trace_count_, (unsigned) n, (unsigned) this->slave_.trace_at(0).sda,
           (unsigned) this->slave_.trace_at(0).scl);
  char line[112];
  size_t pos = 0;
  line[0] = '\0';
  for (uint8_t i = 1; i < n; i++) {
    const auto &e = this->slave_.trace_at(i);
    pos += snprintf(line + pos, sizeof(line) - pos, "%u:%u%u ", (unsigned) (e.dt_ticks / tpu), e.sda, e.scl);
    if (pos > sizeof(line) - 16 || i == n - 1) {
      ESP_LOGD(TAG, "  %s", line);
      pos = 0;
      line[0] = '\0';
    }
  }
  this->slave_.trace_rearm();
}

// Remember when each register last changed its sequence number (the ISR has no access to millis()).
void ThermiaComponent::refresh_timestamps_(uint32_t now) {
  for (uint8_t reg : this->registers_) {
    const uint16_t seq = this->slave_.slot(reg) >> 16;
    if (seq != this->seen_seq_[reg]) {
      this->seen_seq_[reg] = seq;
      this->rx_ms_[reg] = now ? now : 1;
    }
  }
}

void ThermiaComponent::update_link_(uint32_t now) {
  const uint32_t pings = this->slave_.pings();
  if (pings != this->last_pings_) {
    this->last_pings_ = pings;
    this->last_ping_ms_ = now ? now : 1;
  }
  const bool up = this->last_ping_ms_ != 0 && (now - this->last_ping_ms_) < LINK_TIMEOUT_MS;
  if (up != this->link_up_) {
    this->link_up_ = up;
    ESP_LOGI(TAG, "Heat pump %s", up ? "is talking to us" : "went silent");
  }
  if (this->link_ != nullptr && (!this->link_published_ || up != this->link_->state)) {
    this->link_->publish_state(up);
    this->link_published_ = true;
  }
}

bool ThermiaComponent::fresh_value_(uint8_t reg, uint16_t &raw) const {
  const uint32_t slot = this->slave_.slot(reg);
  if ((slot >> 16) == 0)
    return false;  // never received
  if (millis() - this->rx_ms_[reg] > this->stale_timeout_ms_)
    return false;
  raw = slot & 0xFFFF;
  return true;
}

void ThermiaComponent::update() {
  for (auto &b : this->sensors_) {
    uint16_t raw;
    if (!this->fresh_value_(b.reg, raw)) {
      if (b.sensor->has_state())
        b.sensor->publish_state(NAN);  // stale -> shows as unavailable/unknown in HA
      continue;
    }
    const float value = (b.is_signed ? (float) (int16_t) raw : (float) raw) * b.scale;
    if (value < b.min || value > b.max) {
      ESP_LOGW(TAG, "r%02X = 0x%04X (%.1f) is outside the plausible range [%.0f, %.0f], ignored", b.reg, raw, value,
               b.min, b.max);
      continue;
    }
    b.sensor->publish_state(value);
  }

  for (auto &b : this->binary_sensors_) {
    uint16_t raw;
    if (this->fresh_value_(b.reg, raw))
      b.sensor->publish_state((raw & b.mask) != 0);
  }

  // Numbers/selects: correct the optimistic state set by control() with what the pump actually reports. This is
  // the only way the user finds out a write was ignored or clamped - the bus protocol gives no other confirmation.
  // Skipped while superseded(): the cached reading predates our last write, it would undo the optimistic state.
  for (auto &b : this->numbers_) {
    uint16_t raw;
    if (this->slave_.superseded(b.reg) || !this->fresh_value_(b.reg, raw))
      continue;
    const float value = (b.is_signed ? (float) (int16_t) raw : (float) raw) * b.scale;
    if (value < b.min || value > b.max) {
      ESP_LOGW(TAG, "r%02X = 0x%04X (%.1f) is outside the plausible range [%.0f, %.0f], ignored", b.reg, raw, value,
               b.min, b.max);
      continue;
    }
    b.number->publish_state(value);
  }
  for (auto &b : this->selects_) {
    uint16_t raw;
    if (!this->slave_.superseded(b.reg) && this->fresh_value_(b.reg, raw) && b.select->has_index(raw))
      b.select->publish_state((size_t) raw);
    // else: the pump reported a value outside our known options (e.g. one of the undocumented modes 5-16) -
    // leave the select showing its last known-good state rather than erroring or guessing a label for it.
  }

  ESP_LOGD(TAG,
           "link=%s pings=%u data=%u reads=%u ok=%u err=%u foreign=%u unknown=%u unanswered=%u writes=%u/%u "
           "isr=%u max_isr=%uus idle levels SDA=%d SCL=%d",
           this->link_up_ ? "up" : "DOWN", (unsigned) this->slave_.pings(), (unsigned) this->slave_.data_frames(),
           (unsigned) this->slave_.reads(), (unsigned) this->slave_.transactions(), (unsigned) this->slave_.errors(),
           (unsigned) this->slave_.mismatches(), (unsigned) this->slave_.unknown_messages(),
           (unsigned) this->slave_.unanswered(), (unsigned) this->slave_.writes_delivered(),
           (unsigned) this->slave_.writes_failed(), (unsigned) this->slave_.isr_calls(),
           (unsigned) this->slave_.max_isr_us(), this->sda_pin_->digital_read(), this->scl_pin_->digital_read());

  // Raw dump of everything received so far - handy to verify the register mapping against the pump's display.
  // Age (seconds since the last successful answer for that register) is included and marked with '!' once it
  // exceeds stale_timeout - this is what actually gates publishing (see fresh_value_()), not just "ever received".
  // A register can sit here with an unchanged value for a long time perfectly legitimately (the pump's own
  // reading did not change) - the age/'!' is what tells them apart from one that stopped being answered. '~' marks a
  // register we wrote to and the pump has not reported since (see ThermiaSlave::superseded()).
  const uint32_t now_ms = millis();
  char line[112];
  size_t pos = 0;
  line[0] = '\0';
  for (uint8_t reg : this->registers_) {
    const uint32_t slot = this->slave_.slot(reg);
    if ((slot >> 16) == 0)
      continue;
    const uint32_t age_ms = now_ms - this->rx_ms_[reg];
    pos += snprintf(line + pos, sizeof(line) - pos, "r%02X=%04X@%us%s%s ", reg, (unsigned) (slot & 0xFFFF),
                     (unsigned) (age_ms / 1000), age_ms > this->stale_timeout_ms_ ? "!" : "",
                     this->slave_.superseded(reg) ? "~" : "");
    if (pos > sizeof(line) - 20) {
      ESP_LOGD(TAG, "regs: %s", line);
      pos = 0;
      line[0] = '\0';
    }
  }
  if (pos > 0)
    ESP_LOGD(TAG, "regs: %s", line);
}

void ThermiaComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Thermia/Danfoss EXT port (software I2C slave, address 0x%02X):", ThermiaSlave::SLAVE_ADDR);
  LOG_PIN("  SDA pin: ", this->sda_pin_);
  LOG_PIN("  SCL pin: ", this->scl_pin_);
  ESP_LOGCONFIG(TAG, "  Stale timeout: %us", (unsigned) (this->stale_timeout_ms_ / 1000));
  ESP_LOGCONFIG(TAG, "  Starts talking to the pump %us after the network comes up (and pauses if it drops)",
                (unsigned) (ThermiaComponent::BUS_GATE_GRACE_MS / 1000));
  ESP_LOGCONFIG(TAG, "  Debug frames: %s", YESNO(this->debug_frames_));
  ESP_LOGCONFIG(TAG, "  Selftest: %s", YESNO(this->selftest_));
  if (this->selftest_) {
    ESP_LOGCONFIG(TAG, "  SDA probe (GPIO%u pulled low by us at boot): idle=%s, while driven low it reads %s, "
                       "after release it reads %s, rise time %u ns",
                  this->sda_pin_->get_pin(), this->probe_.idle_high ? "HIGH" : "LOW",
                  this->probe_.low_reads_low ? "LOW (ok)" : "HIGH (!)",
                  this->probe_.released_high ? "HIGH (ok)" : "LOW (!)", (unsigned) this->probe_.rise_ns);
    if (!this->probe_.low_reads_low)
      ESP_LOGW(TAG, "  -> SDA cannot be pulled low: it is hard-tied high (short to 3.3V/5V?) or the pin is damaged");
    if (!this->probe_.idle_high || !this->probe_.released_high)
      ESP_LOGW(TAG, "  -> SDA is held LOW by something (short to GND, converter or pump holding the line)");
  }
  ESP_LOGCONFIG(TAG, "  Sniff mode (passive waveform capture, protocol disabled): %s", YESNO(this->sniff_));
  ESP_LOGCONFIG(TAG, "  Polled registers: %u", (unsigned) this->registers_.size());
  LOG_UPDATE_INTERVAL(this);
  LOG_BINARY_SENSOR("  ", "Link", this->link_);
  for (auto &b : this->sensors_)
    LOG_SENSOR("  ", "Sensor", b.sensor);
  for (auto &b : this->binary_sensors_)
    LOG_BINARY_SENSOR("  ", "Binary sensor", b.sensor);
  for (auto &b : this->numbers_) {
    LOG_NUMBER("  ", "Number (writable)", b.number);
    ESP_LOGCONFIG(TAG, "    Register: 0x%02X", b.reg);
  }
  for (auto &b : this->selects_) {
    LOG_SELECT("  ", "Select (writable)", b.select);
    ESP_LOGCONFIG(TAG, "    Register: 0x%02X", b.reg);
  }
}

}  // namespace thermia
}  // namespace esphome
