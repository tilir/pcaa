// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Implements the bare-metal polling driver for the accelerator MMIO device.

#include "accel_driver.h"

enum {
  kDescriptorAlignment = 8,
  kPhysicalAddressLowBits = 32,
  kDoorbellSubmit = 1,
};

static volatile uint32_t *const k_registers = (volatile uint32_t *)ACCEL_MMIO_BASE;
static accel_command_t pending_command __attribute__((aligned(kDescriptorAlignment)));

static inline void fence_read_write(void) {
  __asm__ volatile("fence iorw, iorw" ::: "memory");
}

static accel_command_t make_command(uint32_t opcode, const int32_t *src0, const int32_t *src1,
                                    const int32_t *src2, void *dst, size_t n) {
  accel_command_t command = {
      .opcode = opcode,
      .n = (uint32_t)n,
      .src0 = (uintptr_t)src0,
      .src1 = (uintptr_t)src1,
      .src2 = (uintptr_t)src2,
      .dst = (uintptr_t)dst,
  };
  return command;
}

void accel_init(void) {
  fence_read_write();
}

int accel_submit(const accel_command_t *source) {
  pending_command = *source;
  fence_read_write();

  const uintptr_t command_address = (uintptr_t)&pending_command;
  k_registers[ACCEL_MMIO_DESC_ADDR_LO / sizeof(uint32_t)] = (uint32_t)command_address;
  k_registers[ACCEL_MMIO_DESC_ADDR_HI / sizeof(uint32_t)] =
      (uint32_t)(command_address >> kPhysicalAddressLowBits);
  fence_read_write();
  k_registers[ACCEL_MMIO_DOORBELL / sizeof(uint32_t)] = kDoorbellSubmit;
  return 0;
}

int accel_wait(void) {
  uint32_t status = ACCEL_STATUS_IDLE;
  do {
    status = k_registers[ACCEL_MMIO_STATUS / sizeof(uint32_t)];
  } while (status == ACCEL_STATUS_IDLE || status == ACCEL_STATUS_BUSY);

  fence_read_write();
  return status == ACCEL_STATUS_DONE ? 0 : -1;
}

int32_t accel_min_add(const int32_t *a, const int32_t *b, size_t n) {
  int32_t result = ACCEL_INF;
  const accel_command_t command =
      make_command(ACCEL_OPCODE_MAP_ADD_REDUCE_MIN, a, b, NULL, &result, n);
  return accel_submit(&command) || accel_wait() ? ACCEL_INF : result;
}

int32_t accel_min_add3(const int32_t *a, const int32_t *b, const int32_t *d, size_t n) {
  int32_t result = ACCEL_INF;
  const accel_command_t command =
      make_command(ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN, a, b, d, &result, n);
  return accel_submit(&command) || accel_wait() ? ACCEL_INF : result;
}

accel_min_argmin_result_t accel_min_add_argmin(const int32_t *a, const int32_t *b, size_t n) {
  accel_min_argmin_result_t result = {ACCEL_INF, 0};
  accel_min_add_argmin_checked(a, b, n, &result);
  return result;
}

accel_min_argmin_result_t accel_min_add3_argmin(const int32_t *a, const int32_t *b,
                                                const int32_t *c, size_t n) {
  accel_min_argmin_result_t result = {ACCEL_INF, 0};
  accel_min_add3_argmin_checked(a, b, c, n, &result);
  return result;
}

int accel_min_add_argmin_checked(const int32_t *a, const int32_t *b, size_t n,
                                 accel_min_argmin_result_t *result) {
  if (result == NULL)
    return -1;
  const accel_command_t command =
      make_command(ACCEL_OPCODE_MAP_ADD_REDUCE_MIN_ARGMIN, a, b, NULL, result, n);
  return accel_submit(&command) || accel_wait() ? -1 : 0;
}

int accel_min_add3_argmin_checked(const int32_t *a, const int32_t *b, const int32_t *c, size_t n,
                                  accel_min_argmin_result_t *result) {
  if (result == NULL)
    return -1;
  const accel_command_t command =
      make_command(ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN_ARGMIN, a, b, c, result, n);
  return accel_submit(&command) || accel_wait() ? -1 : 0;
}
