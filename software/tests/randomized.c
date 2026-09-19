// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Runs fixed-seed differential checks over varied short vector lengths.

#include "accel_driver.h"
#include "common.h"

enum {
  kMaximumVectorLength = 64,
  kRandomTestRounds = 100,
  kRandomValueSpan = 401,
  kRandomValueOffset = 200,
  kMinAddFailureBase = 0x100,
  kMinAdd3FailureBase = 0x200,
};

static const uint32_t kRandomSeed = 0x51a7c0deu;
static const uint32_t kLcgMultiplier = 1664525u;
static const uint32_t kLcgIncrement = 1013904223u;
static uint32_t random_state = kRandomSeed;

static uint32_t next_random(void) {
  random_state = random_state * kLcgMultiplier + kLcgIncrement;
  return random_state;
}

static int32_t random_cost(uint32_t inf_divisor) {
  if (next_random() % inf_divisor == 0) {
    return ACCEL_INF;
  }
  return (int32_t)(next_random() % kRandomValueSpan) - kRandomValueOffset;
}

int main(void) {
  static const int sizes[] = {1, 2, 3, 7, 8, 15, 16, 17, 31, 32, 63, 64};
  int32_t first[kMaximumVectorLength];
  int32_t second[kMaximumVectorLength];
  int32_t third[kMaximumVectorLength];

  accel_init();
  for (int round = 0; round < kRandomTestRounds; ++round) {
    for (size_t size_index = 0; size_index < sizeof(sizes) / sizeof(sizes[0]); ++size_index) {
      const int count = sizes[size_index];
      for (int index = 0; index < count; ++index) {
        first[index] = random_cost(19);
        second[index] = random_cost(23);
        third[index] = random_cost(29);
      }

      if (accel_min_add(first, second, count) != ref_min2(first, second, count)) {
        finish(kMinAddFailureBase + round);
      }
      if (accel_min_add3(first, second, third, count) != ref_min3(first, second, third, count)) {
        finish(kMinAdd3FailureBase + round);
      }
    }
  }

  finish(0);
}
