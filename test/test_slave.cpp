// Host test for the Thermia I2C slave engine.
//   g++ -std=c++17 -DTHERMIA_HOST_TEST -I test -I components/thermia test/test_slave.cpp -o /tmp/test_slave && /tmp/test_slave

#include <cstdio>
#include <string>
#include <vector>

#include "thermia_bus_gate.h"
#include "thermia_slave.h"

using namespace esphome::thermia;
using esphome::thermia::hw::g_sim;
using esphome::thermia::hw::Step;

static int g_failed = 0;
static int g_checks = 0;
#define CHECK(cond, ...)                                  \
  do {                                                    \
    g_checks++;                                           \
    if (!(cond)) {                                        \
      g_failed++;                                         \
      printf("  FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond); \
      printf(__VA_ARGS__);                                \
      printf("\n");                                       \
    }                                                     \
  } while (0)

// Waveform builder. Tags >= 0 mark samples the master takes of SDA (ACK bits, data read from the slave).
struct Master {
  std::vector<Step> s;
  int next_tag = 0;

  void push(bool scl, bool sda, int tag = -1) { s.push_back({scl, sda, tag}); }
  void idle() {
    push(true, true);
    push(true, true);
  }
  void start() {  // from idle
    push(true, true);
    push(true, false);
    push(false, false);
  }
  void rstart() {  // repeated START, SCL is low on entry
    push(false, true);
    push(true, true);
    push(true, false);
    push(false, false);
  }
  void stop() {
    push(false, false);
    push(true, false);
    push(true, true);
  }
  void bit(bool b) {
    push(false, b);
    push(true, b);
    push(false, b);
  }
  // Master -> slave byte followed by the slave's ACK clock. Returns the tag of the ACK sample.
  int write_byte(uint8_t v) {
    for (int i = 7; i >= 0; i--)
      bit((v >> i) & 1);
    const int tag = next_tag++;
    push(false, true);
    push(true, true, tag);
    push(false, true);
    return tag;
  }
  // Slave -> master byte followed by the master's ACK/NACK. Returns the first of 8 consecutive tags.
  int read_byte(bool ack) {
    const int first = next_tag;
    for (int i = 0; i < 8; i++) {
      const int tag = next_tag++;
      push(false, true);
      push(true, true, tag);
      push(false, true);
    }
    bit(!ack);  // ACK = SDA low
    return first;
  }
};

struct Bench {
  ThermiaSlave slave;
  std::vector<uint8_t> polled;

  explicit Bench(std::vector<uint8_t> regs) : polled(regs) {
    slave.begin(4, 5);
    for (auto r : regs)
      slave.add_poll(r);
  }

  // Play the waveform; enter the slave ISR on every SDA falling edge seen outside of the ISR.
  void play(Master &m, int rep = 1, int isr_delay = 0) {
    g_sim.steps = m.s;
    g_sim.pos = 0;
    g_sim.sub = 0;
    g_sim.rep = rep;
    g_sim.sampled.clear();
    g_sim.slave_low = false;
    bool prev_sda = true;
    while (g_sim.pos < g_sim.steps.size()) {
      const bool sda = (hw::read_bus() & g_sim.sda_mask) != 0;
      if (prev_sda && !sda) {
        for (int i = 0; i < isr_delay && g_sim.pos < g_sim.steps.size(); i++)
          hw::read_bus();
        slave.on_sda_fall();
        prev_sda = true;  // ISR consumed the transaction; bus is idle again afterwards
      } else {
        prev_sda = sda;
      }
    }
  }

  int sample(int tag) const {
    for (auto &p : g_sim.sampled)
      if (p.first == tag)
        return p.second;
    return -1;
  }
  uint8_t read_value(int first_tag) const {
    uint8_t v = 0;
    for (int i = 0; i < 8; i++)
      v = (v << 1) | sample(first_tag + i);
    return v;
  }

  // Ping, then read exactly `n` bytes from us (ACK on all but the last). Returns the bytes read - used to exercise
  // multi-byte write responses ([reg|0x80, lo, hi]), which a plain register poll never produces.
  std::vector<uint8_t> ping_read_n(uint8_t cmd, int n, int rep, int isr_delay) {
    Master m;
    m.idle();
    m.start();
    m.write_byte(0x5C);
    m.write_byte(cmd);
    m.stop();
    m.idle();
    m.idle();
    m.start();
    const int tag_addr_r = m.write_byte(0x5D);
    std::vector<int> firsts;
    for (int i = 0; i < n; i++)
      firsts.push_back(m.read_byte(i < n - 1));
    m.stop();
    m.idle();
    play(m, rep, isr_delay);
    CHECK(sample(tag_addr_r) == 0, "address read ACK expected");
    std::vector<uint8_t> out;
    for (int f : firsts)
      out.push_back(read_value(f));
    return out;
  }

  // One full pump polling cycle. Returns the byte the pump read from us as response to the ping.
  uint8_t ping(uint8_t cmd, int rep, int isr_delay, bool combined = false) {
    Master m;
    m.idle();
    m.start();
    m.write_byte(0x5C);
    m.write_byte(cmd);
    int tag_addr_r, tag_data = -1;
    if (combined) {
      m.rstart();
    } else {
      m.stop();
      m.idle();
      m.idle();
      m.start();
    }
    tag_addr_r = m.write_byte(0x5D);
    tag_data = m.read_byte(false);
    m.stop();
    m.idle();
    play(m, rep, isr_delay);
    CHECK(sample(tag_addr_r) == 0, "address read ACK expected");
    return read_value(tag_data);
  }

  void send_data(uint8_t reg, uint16_t value, int rep, int isr_delay) {
    Master m;
    m.idle();
    m.start();
    const int t0 = m.write_byte(0x5C);
    const int t1 = m.write_byte(reg);
    const int t2 = m.write_byte(value & 0xFF);
    const int t3 = m.write_byte(value >> 8);
    m.stop();
    m.idle();
    play(m, rep, isr_delay);
    CHECK(sample(t0) == 0 && sample(t1) == 0 && sample(t2) == 0 && sample(t3) == 0, "all bytes ACKed");
  }
};

static void test_poll_cycle(int rep, int delay) {
  printf("poll cycle (rep=%d, isr_delay=%d)\n", rep, delay);
  Bench b({0x00, 0x06, 0x10});

  CHECK(b.ping(0xFD, rep, delay) == 0x00, "boot ping asks for first register");
  b.send_data(0x00, 0xFFFB, rep, delay);  // -5 C
  CHECK((b.slave.slot(0x00) & 0xFFFF) == 0xFFFB && (b.slave.slot(0x00) >> 16) == 1, "r00 stored");

  CHECK(b.ping(0xFE, rep, delay) == 0x06, "next register is r06");
  b.send_data(0x06, 0x0018, rep, delay);
  CHECK((int16_t)(b.slave.slot(0x06) & 0xFFFF) == 24, "r06 = 24");

  CHECK(b.ping(0xFE, rep, delay) == 0x10, "next register is r10");
  b.send_data(0x10, 0x0002, rep, delay);
  CHECK((b.slave.slot(0x10) & 0xFFFF) == 2, "r10 stored");

  CHECK(b.ping(0xFE, rep, delay) == 0x00, "wraps around to r00");
  b.send_data(0x00, 0x0001, rep, delay);
  CHECK((b.slave.slot(0x00) & 0xFFFF) == 1 && (b.slave.slot(0x00) >> 16) == 2, "r00 refreshed, seq 2");

  CHECK(b.slave.pings() == 4, "4 pings, got %u", b.slave.pings());
  CHECK(b.slave.data_frames() == 4, "4 data frames");
  CHECK(b.slave.errors() == 0, "no errors, got %u", b.slave.errors());
  CHECK(b.slave.unanswered() == 0, "nothing unanswered");
}

static void test_combined_transaction() {
  printf("write ping + read with repeated START\n");
  Bench b({0x05});
  CHECK(b.ping(0xFE, 2, 0, true) == 0x05, "response after repeated START");
  b.send_data(0x05, 22, 2, 0);
  CHECK(b.slave.errors() == 0, "no errors, got %u", b.slave.errors());
}

static void test_idle_and_readonly() {
  printf("idle answer and read-only guarantee\n");
  Bench none({});
  CHECK(none.ping(0xFE, 1, 0) == 0xFF, "no registers configured -> 0xFF");

  Bench b({0x7F, 0x80, 0xFF, 0x20});
  CHECK(b.slave.poll_count() == 2, "registers >= 0x80 are rejected, poll count %u", b.slave.poll_count());
  for (int i = 0; i < 12; i++) {
    const uint8_t r = b.ping(0xFE, 1, 0);
    CHECK(r <= 0x7F || r == 0xFF, "response 0x%02X would be a WRITE request", r);
  }
}

static void test_unanswered_register_is_skipped() {
  printf("register the pump never delivers is retried MAX_TRIES times, then skipped\n");
  Bench b({0x11, 0x22});
  std::vector<uint8_t> asked;
  for (int i = 0; i < 6; i++)
    asked.push_back(b.ping(0xFE, 1, 0));
  const std::vector<uint8_t> expect = {0x11, 0x11, 0x11, 0x22, 0x22, 0x22};
  CHECK(asked == expect, "asked sequence mismatch");
  CHECK(b.slave.unanswered() == 1, "one skip, got %u", b.slave.unanswered());
  // Late answer for a register we are not currently asking for is still stored.
  b.send_data(0x11, 7, 1, 0);
  CHECK((b.slave.slot(0x11) & 0xFFFF) == 7, "unsolicited data stored");
}

static void test_multi_byte_read() {
  printf("master reading several bytes\n");
  Bench b({0x0E});
  Master m;
  m.idle();
  m.start();
  m.write_byte(0x5C);
  m.write_byte(0xFE);
  m.stop();
  m.idle();
  m.start();
  const int ta = m.write_byte(0x5D);
  const int d0 = m.read_byte(true);
  const int d1 = m.read_byte(true);
  const int d2 = m.read_byte(false);
  m.stop();
  m.idle();
  b.play(m, 1, 0);
  CHECK(b.sample(ta) == 0, "ACK");
  CHECK(b.read_value(d0) == 0x0E, "first byte is the request");
  CHECK(b.read_value(d1) == 0xFF && b.read_value(d2) == 0xFF, "filler bytes are 0xFF, never a write request");
  CHECK(b.slave.errors() == 0, "no errors");
}

static void test_other_address_ignored() {
  printf("transactions to other addresses are ignored\n");
  Bench b({0x01});
  Master m;
  m.idle();
  m.start();
  const int t0 = m.write_byte(0x5A);  // 7-bit 0x2D
  m.write_byte(0x00);
  m.stop();
  m.idle();
  b.play(m, 1, 0);
  CHECK(b.sample(t0) == 1, "no ACK for foreign address");
  CHECK(b.slave.mismatches() == 1, "counted as mismatch");
  CHECK(!g_sim.slave_low, "SDA released");
  CHECK(b.slave.errors() == 0, "not an error");
  CHECK(b.ping(0xFE, 1, 0) == 0x01, "still works afterwards");
}

static void test_unknown_and_short_messages() {
  printf("unknown / malformed messages\n");
  Bench b({0x01});
  Master m;
  m.idle();
  m.start();
  m.write_byte(0x5C);
  m.write_byte(0x01);
  m.write_byte(0x55);  // reg + only one payload byte: malformed
  m.stop();
  m.idle();
  m.start();
  m.write_byte(0x5C);  // address only
  m.stop();
  m.idle();
  b.play(m, 1, 0);
  CHECK(b.slave.unknown_messages() == 1, "malformed frame counted, got %u", b.slave.unknown_messages());
  CHECK(b.slave.slot(0x01) == 0, "malformed frame not stored");
  CHECK(b.slave.errors() == 0, "no errors");
}

static void test_bus_stall() {
  printf("bus stalls in the middle of a byte\n");
  Bench b({0x01});
  Master m;
  m.idle();
  m.start();
  for (int i = 0; i < 4; i++)
    m.bit(1);  // 4 bits, then SCL stays low forever
  m.push(false, true);
  b.play(m, 1, 0);
  CHECK(b.slave.errors() == 1, "error counted, got %u", b.slave.errors());
  CHECK(!g_sim.slave_low, "SDA released after timeout");
  CHECK(b.ping(0xFE, 1, 0) == 0x01, "recovers on the next transaction");
}

static void test_late_isr_is_harmless() {
  printf("ISR entered after the first bit already went by\n");
  Bench b({0x01});
  // Too late: the first address bit is lost, the byte is misaligned and must not match our address.
  Master m;
  m.idle();
  m.start();
  const int t0 = m.write_byte(0x5C);
  m.write_byte(0xFE);
  m.stop();
  m.idle();
  b.play(m, 3, 8);
  CHECK(b.sample(t0) == 1, "misaligned address is not ACKed");
  CHECK(b.slave.pings() == 0 && b.slave.data_frames() == 0, "nothing interpreted");
  CHECK(b.slave.mismatches() == 1, "counted as mismatch so it shows up in the stats");
  CHECK(!g_sim.slave_low, "SDA released");
  CHECK(b.ping(0xFE, 3, 0) == 0x01, "next in-time transaction works");
}

static void test_sniff() {
  printf("sniff mode records the waveform and never drives the bus\n");
  Bench b({0x01});
  b.slave.set_sniff(true);
  Master m;
  m.idle();
  m.start();
  const int t0 = m.write_byte(0x5C);
  m.write_byte(0xFE);
  m.stop();
  m.idle();
  b.play(m, 3, 0);
  CHECK(b.slave.trace_ready(), "capture ready");
  CHECK(b.slave.trace_count() > 30, "many changes recorded, got %u", b.slave.trace_count());
  CHECK(b.slave.trace_at(0).sda == 0, "first state: SDA already low (START)");
  CHECK(b.sample(t0) == 1, "sniffer never ACKs");
  CHECK(!g_sim.slave_low, "SDA never driven");
  CHECK(b.slave.pings() == 0, "protocol disabled while sniffing");
  // Second transaction is ignored until the main loop re-arms the sniffer.
  const uint8_t n = b.slave.trace_count();
  b.play(m, 3, 0);
  CHECK(b.slave.trace_count() == n, "capture not overwritten before rearm");
  b.slave.trace_rearm();
  CHECK(!b.slave.trace_ready(), "rearmed");
}

static void reset_sim() {
  g_sim.steps.clear();
  g_sim.pos = 0;
  g_sim.sub = 0;
  g_sim.slave_low = false;
  g_sim.sda_stuck_high = false;
  g_sim.sda_stuck_low = false;
}

static void test_probe() {
  printf("SDA probe classifies healthy / shorted-high / held-low lines\n");
  Bench b({0x01});

  reset_sim();
  auto r = b.slave.probe_sda();
  CHECK(r.idle_high && r.low_reads_low && r.released_high, "healthy line");
  CHECK(!g_sim.slave_low, "SDA released after the probe");

  reset_sim();
  g_sim.sda_stuck_high = true;
  r = b.slave.probe_sda();
  CHECK(r.idle_high && !r.low_reads_low, "line hard-tied high: cannot be pulled low");

  reset_sim();
  g_sim.sda_stuck_low = true;
  r = b.slave.probe_sda();
  CHECK(!r.idle_high && !r.released_high, "line held low: never recovers");
  CHECK(r.rise_ns >= 2000000UL, "rise time capped at 2 ms, got %u ns", (unsigned) r.rise_ns);
  CHECK(!g_sim.slave_low, "SDA released even when the line is stuck");
  reset_sim();
}

static void test_write_delivered() {
  printf("a queued write is delivered as [reg|0x80, lo, hi] and read back correctly\n");
  Bench b({});
  CHECK(b.slave.queue_write(0x20, 0x1234), "queue_write accepts a valid register");
  CHECK(b.slave.write_pending(), "write is now pending");
  auto bytes = b.ping_read_n(0xFE, 3, 1, 0);
  CHECK(bytes.size() == 3 && bytes[0] == (0x20 | 0x80) && bytes[1] == 0x34 && bytes[2] == 0x12,
        "expected [A0 34 12], got [%02X %02X %02X]", bytes[0], bytes[1], bytes[2]);
  CHECK(!b.slave.write_pending(), "no longer pending after clean delivery");
  CHECK(b.slave.writes_delivered() == 1, "one delivery counted, got %u", (unsigned) b.slave.writes_delivered());
  CHECK(b.slave.writes_failed() == 0, "no failures");
  CHECK(!b.slave.queue_write(0x80, 1), "register >= 0x80 is rejected");
}

static void test_write_takes_priority_over_polling() {
  printf("a pending write pre-empts the normal read-poll cycle, which resumes once delivered\n");
  Bench b({0x05});
  CHECK(b.ping(0xFE, 1, 0) == 0x05, "poll cycle requests r05 as usual before any write is queued");
  b.slave.queue_write(0x10, 7);
  auto bytes = b.ping_read_n(0xFE, 3, 1, 0);
  CHECK(bytes[0] == (0x10 | 0x80), "write response pre-empts the poll register, got %02X", bytes[0]);
  CHECK(b.ping(0xFE, 1, 0) == 0x05, "poll cycle resumes at r05 once the write is out of the way");
}

static void test_write_retries_then_fails() {
  printf("a write the master never fully reads is retried WRITE_MAX_TRIES times, then abandoned\n");
  Bench b({});
  b.slave.queue_write(0x20, 1);
  for (int i = 0; i < ThermiaSlave::WRITE_MAX_TRIES; i++) {
    auto bytes = b.ping_read_n(0xFE, 1, 1, 0);  // master only reads the first byte, never the value
    CHECK(bytes[0] == (0x20 | 0x80), "still offering the write, attempt %d", i);
    CHECK(b.slave.write_pending() == (i + 1 < ThermiaSlave::WRITE_MAX_TRIES), "pending state after attempt %d", i);
  }
  CHECK(!b.slave.write_pending(), "gave up");
  CHECK(b.slave.writes_failed() == 1, "one failure counted, got %u", (unsigned) b.slave.writes_failed());
  CHECK(b.slave.writes_delivered() == 0, "no false delivery");
  CHECK(b.ping(0xFE, 1, 0) == 0xFF, "back to idle, nothing left to offer");
}

static void test_bus_gate() {
  printf("bus gate: waits for a grace period after the network connects, drops out instantly on disconnect\n");
  using esphome::thermia::BusGate;
  BusGate g(1000);

  CHECK(!g.update(false, 500), "not connected -> never attached");
  CHECK(!g.update(true, 1000), "just connected -> still within grace period");
  CHECK(!g.update(true, 1999), "1 ms short of the grace period -> still not attached");
  CHECK(g.update(true, 2000), "grace period elapsed -> now attached");
  CHECK(g.attached(), "attached() agrees");
  CHECK(g.update(true, 50000), "stays attached while connected");

  CHECK(!g.update(false, 50001), "disconnect -> detached immediately, no grace period on the way down");
  CHECK(!g.attached(), "attached() agrees");
  CHECK(!g.update(true, 50002), "reconnect starts a fresh grace period");
  CHECK(g.update(true, 51002), "...and completes exactly one grace period later");

  // millis() rollover: connecting right before the 32-bit wrap must not wedge the grace period forever.
  BusGate g2(1000);
  const uint32_t near_wrap = 0xFFFFFFF0u;
  CHECK(!g2.update(true, near_wrap), "connects just before the millis() rollover");
  CHECK(g2.update(true, near_wrap + 1500u /* wraps past 0 */), "grace period still elapses correctly across the wrap");
}

static void test_guard() {
  printf("guard trips on a permanently broken bus\n");
  Bench b({0x01});
  for (int i = 0; i < 60; i++) {
    Master m;
    m.idle();
    m.start();
    m.push(false, false);  // SCL low, then nothing
    b.play(m, 1, 0);
  }
  CHECK(b.slave.guard_tripped(), "guard active");
  const uint32_t calls = b.slave.isr_calls();
  Master m;
  m.idle();
  m.start();
  m.push(false, false);
  b.play(m, 1, 0);
  CHECK(b.slave.isr_calls() == calls, "guarded ISR returns immediately");
  b.slave.reset_guard();
  CHECK(!b.slave.guard_tripped(), "guard reset");
}

int main() {
  test_poll_cycle(1, 0);
  test_poll_cycle(3, 0);
  test_poll_cycle(3, 4);
  test_poll_cycle(1, 1);
  test_combined_transaction();
  test_idle_and_readonly();
  test_unanswered_register_is_skipped();
  test_multi_byte_read();
  test_other_address_ignored();
  test_unknown_and_short_messages();
  test_bus_stall();
  test_late_isr_is_harmless();
  test_sniff();
  test_probe();
  test_write_delivered();
  test_write_takes_priority_over_polling();
  test_write_retries_then_fails();
  test_bus_gate();
  test_guard();
  printf("\n%d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
