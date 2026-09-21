// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Adapts PBQP vector views to contiguous PCAA primitive submissions.

#include "pbqp_accelerator.h"

#include "accel_driver.h"

static const int32_t *contiguous(pbqp_accelerator_kernel_context_t *context,
                                 pbqp_vector_view_t view, int32_t *scratch) {
  if (view.stride == 1)
    return view.base;
  for (size_t i = 0; i < view.length; ++i) scratch[i] = view.base[i * view.stride];
  ++context->statistics->scratch_packs;
  context->statistics->scratch_bytes += view.length * sizeof(*scratch);
  return scratch;
}

static void begin_batch(pbqp_accelerator_kernel_context_t *context) {
  context->packed_view_count = 0;
}

static const int32_t *batch_contiguous(pbqp_accelerator_kernel_context_t *context,
                                       pbqp_vector_view_t view) {
  if (view.stride == 1)
    return view.base;

  for (unsigned index = 0; index < context->packed_view_count; ++index) {
    if (context->packed_bases[index] == view.base &&
        context->packed_lengths[index] == view.length &&
        context->packed_strides[index] == view.stride)
      return context->packed_views[index];
  }
  if (context->packed_view_count == PBQP_MAX_BATCH_PACKED_VIEWS)
    return NULL;

  const unsigned index = context->packed_view_count++;
  int32_t *packed = context->packed_views[index];
  for (size_t element = 0; element < view.length; ++element)
    packed[element] = view.base[element * view.stride];
  context->packed_bases[index] = view.base;
  context->packed_lengths[index] = view.length;
  context->packed_strides[index] = view.stride;
  ++context->statistics->scratch_packs;
  context->statistics->scratch_bytes += view.length * sizeof(*packed);
  ++context->statistics->unique_packed_views;
  return packed;
}

static void record_batch_submission(pbqp_accelerator_kernel_context_t *context, size_t count) {
  ++context->statistics->top_level_submissions;
  ++context->statistics->batch_submissions;
  context->statistics->batch_primitive_descriptors += count;
  if (count > context->statistics->maximum_batch_size)
    context->statistics->maximum_batch_size = count;
  context->statistics->batch_descriptor_bytes += sizeof(accel_command_t) * (count + 1);
  context->statistics->batch_child_descriptor_bytes += count * sizeof(accel_command_t);
}

static int accel_min2(void *opaque, pbqp_vector_view_t a, pbqp_vector_view_t b,
                      accel_min_argmin_result_t *result) {
  pbqp_accelerator_kernel_context_t *context = opaque;
  const int32_t *first = contiguous(context, a, context->scratch0);
  const int32_t *second = contiguous(context, b, context->scratch1);
  return accel_min_add_argmin_checked(first, second, a.length, result);
}

static int accel_min3(void *opaque, pbqp_vector_view_t a, pbqp_vector_view_t b,
                      pbqp_vector_view_t c, accel_min_argmin_result_t *result) {
  pbqp_accelerator_kernel_context_t *context = opaque;
  const int32_t *first = contiguous(context, a, context->scratch0);
  const int32_t *second = contiguous(context, b, context->scratch1);
  const int32_t *third = contiguous(context, c, context->scratch2);
  return accel_min_add3_argmin_checked(first, second, third, a.length, result);
}

static int accel_min2_value(void *opaque, pbqp_vector_view_t a, pbqp_vector_view_t b,
                            int32_t *result) {
  pbqp_accelerator_kernel_context_t *context = opaque;
  const int32_t *first = contiguous(context, a, context->scratch0);
  const int32_t *second = contiguous(context, b, context->scratch1);
  if (first == NULL || second == NULL || result == NULL)
    return -1;
  return accel_min_add_checked(first, second, a.length, result);
}

static int accel_min2_batch(void *opaque, const pbqp_min2_job_t *jobs, size_t count) {
  pbqp_accelerator_kernel_context_t *context = opaque;
  if (count > PBQP_MAX_BATCH_JOBS)
    return -1;
  begin_batch(context);
  for (size_t index = 0; index < count; ++index) {
    const int32_t *first = batch_contiguous(context, jobs[index].a);
    const int32_t *second = batch_contiguous(context, jobs[index].b);
    if (first == NULL || second == NULL)
      return -1;
    context->batch_commands[index] = (accel_command_t){
        .opcode = ACCEL_OPCODE_MAP_ADD_REDUCE_MIN_ARGMIN,
        .n = (uint32_t)jobs[index].a.length,
        .src0 = (uintptr_t)first,
        .src1 = (uintptr_t)second,
        .dst = (uintptr_t)jobs[index].result,
    };
  }
  record_batch_submission(context, count);
  return accel_submit_batch(context->batch_commands, count, &context->batch_result);
}

static int accel_min3_batch(void *opaque, const pbqp_min3_job_t *jobs, size_t count) {
  pbqp_accelerator_kernel_context_t *context = opaque;
  if (count > PBQP_MAX_BATCH_JOBS)
    return -1;
  begin_batch(context);
  for (size_t index = 0; index < count; ++index) {
    const int32_t *first = batch_contiguous(context, jobs[index].a);
    const int32_t *second = batch_contiguous(context, jobs[index].b);
    const int32_t *third = batch_contiguous(context, jobs[index].c);
    if (first == NULL || second == NULL || third == NULL)
      return -1;
    context->batch_commands[index] = (accel_command_t){
        .opcode = ACCEL_OPCODE_MAP_ADD3_REDUCE_MIN_ARGMIN,
        .n = (uint32_t)jobs[index].a.length,
        .src0 = (uintptr_t)first,
        .src1 = (uintptr_t)second,
        .src2 = (uintptr_t)third,
        .dst = (uintptr_t)jobs[index].result,
    };
  }
  record_batch_submission(context, count);
  return accel_submit_batch(context->batch_commands, count, &context->batch_result);
}

static int accel_min2_value_batch(void *opaque, const pbqp_min2_value_job_t *jobs, size_t count) {
  pbqp_accelerator_kernel_context_t *context = opaque;
  if (count > PBQP_MAX_BATCH_JOBS)
    return -1;
  begin_batch(context);
  for (size_t index = 0; index < count; ++index) {
    const int32_t *first = batch_contiguous(context, jobs[index].a);
    const int32_t *second = batch_contiguous(context, jobs[index].b);
    if (first == NULL || second == NULL || jobs[index].result == NULL)
      return -1;
    context->batch_commands[index] = (accel_command_t){
        .opcode = ACCEL_OPCODE_MAP_ADD_REDUCE_MIN,
        .n = (uint32_t)jobs[index].a.length,
        .src0 = (uintptr_t)first,
        .src1 = (uintptr_t)second,
        .dst = (uintptr_t)jobs[index].result,
    };
  }
  record_batch_submission(context, count);
  return accel_submit_batch(context->batch_commands, count, &context->batch_result);
}

static void accel_set_statistics(void *opaque, pbqp_statistics_t *statistics) {
  pbqp_accelerator_kernel_context_t *context = opaque;
  context->statistics = statistics;
}

void pbqp_make_accelerator_kernel(pbqp_cost_kernel_t *kernel,
                                  pbqp_accelerator_kernel_context_t *context,
                                  pbqp_statistics_t *statistics) {
  context->statistics = statistics;
  kernel->context = context;
  kernel->min2_argmin = accel_min2;
  kernel->min3_argmin = accel_min3;
  kernel->min2_argmin_batch = accel_min2_batch;
  kernel->min3_argmin_batch = accel_min3_batch;
  kernel->min2_value = accel_min2_value;
  kernel->min2_value_batch = accel_min2_value_batch;
  kernel->set_statistics = accel_set_statistics;
}
