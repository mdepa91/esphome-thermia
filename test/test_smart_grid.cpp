// Host test for the smart grid mode -> output mapping (smart_grid_modes.h), checked against the table in the
// Thermia Legend installation guide (terminals 307/308 = SG1, 309/310 = SG2).
#include <cstdio>

#include "smart_grid_modes.h"

using namespace esphome::thermia;

static int failures = 0;

static void expect(SmartGridMode mode, uint8_t outputs, const char *name) {
  uint8_t got = smart_grid_outputs(mode);
  if (got != outputs) {
    std::printf("FAIL %s: outputs 0x%02X, expected 0x%02X\n", name, got, outputs);
    failures++;
  }
}

int main() {
  expect(SmartGridMode::NORMAL, 0, "normal: everything open");
  expect(SmartGridMode::REDUCTION, SG_OUT_SG1_10K, "reduction: 10k on SG1 only");
  expect(SmartGridMode::EVU_BLOCK, SG_OUT_SG1, "EVU: SG1 shorted only");
  expect(SmartGridMode::COMFORT, SG_OUT_SG2, "comfort: SG2 shorted only");
  expect(SmartGridMode::BOOST, SG_OUT_SG1 | SG_OUT_SG2, "boost: SG1 and SG2 shorted");
  expect(SmartGridMode::COUNT, 0, "out of range: everything open");

  // The 10k path and the SG1 short never go together: that would look like EVU, not reduction.
  for (int m = 0; m < static_cast<int>(SmartGridMode::COUNT); m++) {
    uint8_t out = smart_grid_outputs(static_cast<SmartGridMode>(m));
    if ((out & SG_OUT_SG1) && (out & SG_OUT_SG1_10K)) {
      std::printf("FAIL mode %d: SG1 short and 10k both closed\n", m);
      failures++;
    }
  }

  if (failures == 0)
    std::printf("smart grid: all tests passed\n");
  return failures == 0 ? 0 : 1;
}
