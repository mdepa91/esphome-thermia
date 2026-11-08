// Host test for the compressor start counter (start_counter.h).
#include <cstdio>

#include "start_counter.h"

using namespace esphome::thermia;

static int failures = 0;

static void check(bool ok, const char *what) {
  if (!ok) {
    std::printf("FAIL %s\n", what);
    failures++;
  }
}

static void check_counts(const StartCounter &c, uint32_t today, uint32_t month, const char *what) {
  if (c.state().today != today || c.state().month != month) {
    std::printf("FAIL %s: today=%u month=%u, expected %u/%u\n", what, (unsigned) c.state().today,
                (unsigned) c.state().month, (unsigned) today, (unsigned) month);
    failures++;
  }
}

int main() {
  {
    StartCounter c;
    check(!c.feed(true), "first reading on is only the baseline");
    check_counts(c, 0, 0, "baseline");
    check(!c.feed(true), "still running is not a start");
    check(!c.feed(false), "stop is not a start");
    check(c.feed(true), "off -> on is a start");
    check(!c.feed(true), "staying on is not another start");
    c.feed(false);
    c.feed(true);
    check_counts(c, 2, 2, "two starts");
  }
  {
    StartCounter c;
    c.feed(false);
    c.feed(true);
    check(!c.roll(0, 0), "invalid clock changes nothing");
    check_counts(c, 1, 1, "start before the clock is valid is kept");
    check(c.roll(20261108, 202611), "first valid date is taken over");
    check_counts(c, 1, 1, "counts kept when the date is taken over");
    check(!c.roll(20261108, 202611), "same date changes nothing");

    c.feed(false);
    c.feed(true);
    check_counts(c, 2, 2, "second start same day");

    check(c.roll(20261109, 202611), "new day");
    check_counts(c, 0, 2, "new day resets today only");

    c.feed(false);
    c.feed(true);
    check(c.roll(20261201, 202612), "new month");
    check_counts(c, 0, 0, "new month resets both");
  }
  {
    // State restored from flash: the next date change resets as usual.
    StartCounter c;
    c.state() = StartCounterState{20261130, 202611, 7, 90};
    check(!c.roll(20261130, 202611), "restored on the same day");
    check_counts(c, 7, 90, "restored counts kept");
    c.roll(20261201, 202612);
    check_counts(c, 0, 0, "restored counts reset on a new month");
  }

  if (failures == 0)
    std::printf("start counter: all tests passed\n");
  return failures == 0 ? 0 : 1;
}
