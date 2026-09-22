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

static accel_command_t project_command(pbqp_matrix_view_t matrix, pbqp_vector_view_t unary,
                                       int32_t *result) {
  accel_command_t command = {0};
  command.opcode = ACCEL_OPCODE_MINPLUS_PROJECT;
  command.n = (uint32_t)matrix.columns;
  command.m = (uint32_t)matrix.rows;
  command.src0 = (uintptr_t)matrix.base;
  command.src1 = (uintptr_t)unary.base;
  command.dst = (uintptr_t)result;
  command.src0_stride = (uint32_t)matrix.column_stride;
  command.src0_outer_stride = (uint32_t)matrix.row_stride;
  command.src1_stride = (uint32_t)unary.stride;
  command.dst_stride = 1;
  return command;
}

static accel_command_t add_vector_command(pbqp_vector_view_t first, pbqp_vector_view_t second,
                                          int32_t *result) {
  accel_command_t command = {0};
  command.opcode = ACCEL_OPCODE_COST_ADD_VECTOR;
  command.n = (uint32_t)first.length;
  command.src0 = (uintptr_t)first.base;
  command.src1 = (uintptr_t)second.base;
  command.dst = (uintptr_t)result;
  command.src0_stride = (uint32_t)first.stride;
  command.src1_stride = (uint32_t)second.stride;
  command.dst_stride = 1;
  return command;
}

static accel_command_t map3_project_command(pbqp_vector_view_t unary, pbqp_vector_view_t fixed_edge,
                                            pbqp_matrix_view_t varying_edge,
                                            accel_min_argmin_result_t *result) {
  accel_command_t command = {0};
  command.opcode = ACCEL_OPCODE_MINPLUS_MAP3_PROJECT;
  command.n = (uint32_t)unary.length;
  command.m = (uint32_t)varying_edge.rows;
  command.src0 = (uintptr_t)unary.base;
  command.src1 = (uintptr_t)fixed_edge.base;
  command.src2 = (uintptr_t)varying_edge.base;
  command.dst = (uintptr_t)result;
  command.src0_stride = (uint32_t)unary.stride;
  command.src1_stride = (uint32_t)fixed_edge.stride;
  command.src2_stride = (uint32_t)varying_edge.column_stride;
  command.src2_outer_stride = (uint32_t)varying_edge.row_stride;
  command.dst_stride = 1;
  return command;
}

static int accel_cost_add_vector(void *opaque, pbqp_vector_view_t first, pbqp_vector_view_t second,
                                 int32_t *result) {
  pbqp_accelerator_kernel_context_t *context = opaque;
  context->batch_commands[0] = add_vector_command(first, second, result);
  record_batch_submission(context, 1);
  return accel_submit_batch(context->batch_commands, 1, &context->batch_result);
}

static int accel_minplus_project(void *opaque, pbqp_matrix_view_t matrix, pbqp_vector_view_t unary,
                                 int32_t *result) {
  pbqp_accelerator_kernel_context_t *context = opaque;
  context->batch_commands[0] = project_command(matrix, unary, result);
  record_batch_submission(context, 1);
  return accel_submit_batch(context->batch_commands, 1, &context->batch_result);
}

static int accel_map3_project(void *opaque, pbqp_vector_view_t unary, pbqp_vector_view_t fixed_edge,
                              pbqp_matrix_view_t varying_edge, accel_min_argmin_result_t *result) {
  pbqp_accelerator_kernel_context_t *context = opaque;
  context->batch_commands[0] = map3_project_command(unary, fixed_edge, varying_edge, result);
  record_batch_submission(context, 1);
  return accel_submit_batch(context->batch_commands, 1, &context->batch_result);
}

static int accel_project_add_batch(void *opaque, const pbqp_project_add_job_t *jobs, size_t count) {
  pbqp_accelerator_kernel_context_t *context = opaque;
  if (count > PBQP_MAX_BATCH_JOBS / 2)
    return -1;
  for (size_t index = 0; index < count; ++index) {
    const pbqp_project_add_job_t *job = &jobs[index];
    context->batch_commands[2 * index] = project_command(job->matrix, job->unary, job->temporary);
    context->batch_commands[2 * index + 1] =
        add_vector_command((pbqp_vector_view_t){job->temporary, job->matrix.rows, 1},
                           (pbqp_vector_view_t){job->scores, job->matrix.rows, 1}, job->scores);
  }
  record_batch_submission(context, 2 * count);
  return accel_submit_batch(context->batch_commands, 2 * count, &context->batch_result);
}

static int accel_map3_project_batch(void *opaque, const pbqp_map3_project_job_t *jobs,
                                    size_t count) {
  pbqp_accelerator_kernel_context_t *context = opaque;
  if (count > PBQP_MAX_BATCH_JOBS)
    return -1;
  for (size_t index = 0; index < count; ++index) {
    const pbqp_map3_project_job_t *job = &jobs[index];
    context->batch_commands[index] =
        map3_project_command(job->unary, job->fixed_edge, job->varying_edge, job->results);
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
  kernel->cost_add_vector = accel_cost_add_vector;
  kernel->minplus_project = accel_minplus_project;
  kernel->minplus_map3_project = accel_map3_project;
  kernel->project_add_batch = accel_project_add_batch;
  kernel->map3_project_batch = accel_map3_project_batch;
  kernel->set_statistics = accel_set_statistics;
}
