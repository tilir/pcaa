// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Implements the bare-metal polling driver for the accelerator MMIO device.

#include "accel_driver.h"
#include "pcaa_codec.h"

enum {
  kDescriptorAlignment = 8,
  kPhysicalAddressLowBits = 32,
  kDoorbellSubmit = 1,
};

static volatile uint32_t *const k_registers = (volatile uint32_t *)ACCEL_MMIO_BASE;
static pcaa_encoded_slot_t pending_command __attribute__((aligned(kDescriptorAlignment)));

static inline void fence_read_write(void) {
  __asm__ volatile("fence iorw, iorw" ::: "memory");
}

void accel_init(void) {
  fence_read_write();
}

static int submit_bytes(const void *source, size_t size) {
  if (source == NULL || size > sizeof(pending_command))
    return -1;
  const unsigned char *bytes = source;
  for (size_t index = 0; index < size; ++index) pending_command.bytes[index] = bytes[index];
  fence_read_write();

  const uintptr_t command_address = (uintptr_t)&pending_command;
  k_registers[ACCEL_MMIO_DESC_ADDR_LO / sizeof(uint32_t)] = (uint32_t)command_address;
  k_registers[ACCEL_MMIO_DESC_ADDR_HI / sizeof(uint32_t)] =
      (uint32_t)(command_address >> kPhysicalAddressLowBits);
  fence_read_write();
  k_registers[ACCEL_MMIO_DOORBELL / sizeof(uint32_t)] = kDoorbellSubmit;
  fence_read_write();
  return 0;
}

int accel_submit_encoded(const void *source, size_t size) {
  return submit_bytes(source, size);
}

int accel_submit_command(const pcaa_command_t *command) {
  pcaa_encoded_slot_t encoded;
  size_t bytes = 0;
  if (pcaa_encode_one(command, encoded.bytes, sizeof(encoded.bytes), &bytes) != PCAA_STATUS_OK)
    return -1;
  return submit_bytes(encoded.bytes, bytes);
}

int accel_wait(void) {
  uint32_t status = ACCEL_STATUS_IDLE;
  do {
    status = k_registers[ACCEL_MMIO_STATUS / sizeof(uint32_t)];
  } while (status == ACCEL_STATUS_IDLE || status == ACCEL_STATUS_BUSY);

  fence_read_write();
  return status == ACCEL_STATUS_DONE ? 0 : -1;
}

int accel_submit_command_batch(const pcaa_command_t *commands, size_t count,
                               pcaa_encoded_slot_t *encoded_workspace,
                               accel_batch_result_t *result) {
  if (commands == NULL || encoded_workspace == NULL || result == NULL || count == 0 ||
      count > SIZE_MAX / sizeof(*encoded_workspace))
    return -1;
  size_t child_bytes = 0;
  if (pcaa_encode_stream(commands, count, encoded_workspace, count * sizeof(*encoded_workspace),
                         &child_bytes) != PCAA_STATUS_OK)
    return -1;
  pcaa_encoded_slot_t batch;
  size_t parent_bytes = 0;
  if (pcaa_encode_batch((uintptr_t)encoded_workspace, count, child_bytes, (uintptr_t)result, &batch,
                        &parent_bytes) != PCAA_STATUS_OK)
    return -1;
  return submit_bytes(batch.bytes, parent_bytes) || accel_wait() ? -1 : 0;
}

int32_t accel_min_add(const int32_t *a, const int32_t *b, size_t n) {
  int32_t result = ACCEL_INF;
  accel_min_add_checked(a, b, n, &result);
  return result;
}

int accel_min_add_checked(const int32_t *a, const int32_t *b, size_t n, int32_t *result) {
  if (result == NULL)
    return -1;
  pcaa_command_t command;
  if (pcaa_make_reduce2(pcaa_cost_vector((uintptr_t)a, n, 1), pcaa_cost_vector((uintptr_t)b, n, 1),
                        (uintptr_t)result, 0, &command) != 0)
    return -1;
  return accel_submit_command(&command) || accel_wait() ? -1 : 0;
}

int32_t accel_min_add3(const int32_t *a, const int32_t *b, const int32_t *d, size_t n) {
  int32_t result = ACCEL_INF;
  pcaa_command_t command;
  if (pcaa_make_reduce3(pcaa_cost_vector((uintptr_t)a, n, 1), pcaa_cost_vector((uintptr_t)b, n, 1),
                        pcaa_cost_vector((uintptr_t)d, n, 1), (uintptr_t)&result, 0, &command) != 0)
    return ACCEL_INF;
  return accel_submit_command(&command) || accel_wait() ? ACCEL_INF : result;
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
  pcaa_command_t command;
  if (pcaa_make_reduce2(pcaa_cost_vector((uintptr_t)a, n, 1), pcaa_cost_vector((uintptr_t)b, n, 1),
                        (uintptr_t)result, 1, &command) != 0)
    return -1;
  return accel_submit_command(&command) || accel_wait() ? -1 : 0;
}

int accel_min_add3_argmin_checked(const int32_t *a, const int32_t *b, const int32_t *c, size_t n,
                                  accel_min_argmin_result_t *result) {
  if (result == NULL)
    return -1;
  pcaa_command_t command;
  if (pcaa_make_reduce3(pcaa_cost_vector((uintptr_t)a, n, 1), pcaa_cost_vector((uintptr_t)b, n, 1),
                        pcaa_cost_vector((uintptr_t)c, n, 1), (uintptr_t)result, 1, &command) != 0)
    return -1;
  return accel_submit_command(&command) || accel_wait() ? -1 : 0;
}
