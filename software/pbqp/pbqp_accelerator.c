// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Adapts PBQP vector views to contiguous PCAA primitive submissions.

#include "pbqp_accelerator.h"

static void record_batch_submission(pbqp_accelerator_kernel_context_t *context, size_t count);

static pcaa_cost_vector_view_t vector_view(pbqp_vector_view_t view) {
  return pcaa_cost_vector((pcaa_guest_address_t)(uintptr_t)view.base, view.length, view.stride);
}

static int fail(pbqp_accelerator_kernel_context_t *context, pcaa_status_t status) {
  context->last_status = status;
  return status;
}

static int submit_batch(pbqp_accelerator_kernel_context_t *context, size_t count) {
  record_batch_submission(context, count);
  context->last_status = pcaa_device_submit_batch(context->device, context->batch_commands, count);
  if (context->last_status == PCAA_STATUS_OK)
    context->last_status = pcaa_device_wait(context->device, &context->last_completion);
  return context->last_status;
}

static int submit_reduce2(pbqp_accelerator_kernel_context_t *context, const int32_t *first,
                          const int32_t *second, size_t length, void *result, int with_argmin) {
  pcaa_command_t command;
  context->last_status =
      pcaa_make_reduce2(pcaa_cost_vector((pcaa_guest_address_t)(uintptr_t)first, length, 1),
                        pcaa_cost_vector((pcaa_guest_address_t)(uintptr_t)second, length, 1),
                        (pcaa_guest_address_t)(uintptr_t)result, with_argmin, &command);
  if (context->last_status != PCAA_STATUS_OK)
    return context->last_status;
  context->last_status = pcaa_device_submit(context->device, &command);
  if (context->last_status == PCAA_STATUS_OK)
    context->last_status = pcaa_device_wait(context->device, &context->last_completion);
  return context->last_status;
}

static int submit_reduce3(pbqp_accelerator_kernel_context_t *context, const int32_t *first,
                          const int32_t *second, const int32_t *third, size_t length,
                          void *result) {
  pcaa_command_t command;
  context->last_status =
      pcaa_make_reduce3(pcaa_cost_vector((pcaa_guest_address_t)(uintptr_t)first, length, 1),
                        pcaa_cost_vector((pcaa_guest_address_t)(uintptr_t)second, length, 1),
                        pcaa_cost_vector((pcaa_guest_address_t)(uintptr_t)third, length, 1),
                        (pcaa_guest_address_t)(uintptr_t)result, 1, &command);
  if (context->last_status != PCAA_STATUS_OK)
    return context->last_status;
  context->last_status = pcaa_device_submit(context->device, &command);
  if (context->last_status == PCAA_STATUS_OK)
    context->last_status = pcaa_device_wait(context->device, &context->last_completion);
  return context->last_status;
}

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
  context->statistics->batch_descriptor_bytes += pcaa_encoded_batch_bytes(count);
  context->statistics->batch_child_descriptor_bytes += count * pcaa_encoded_command_bytes();
}

static int accel_min2(void *opaque, pbqp_vector_view_t a, pbqp_vector_view_t b,
                      accel_min_argmin_result_t *result) {
  pbqp_accelerator_kernel_context_t *context = opaque;
  const int32_t *first = contiguous(context, a, context->scratch0);
  const int32_t *second = contiguous(context, b, context->scratch1);
  return submit_reduce2(context, first, second, a.length, result, 1);
}

static int accel_min3(void *opaque, pbqp_vector_view_t a, pbqp_vector_view_t b,
                      pbqp_vector_view_t c, accel_min_argmin_result_t *result) {
  pbqp_accelerator_kernel_context_t *context = opaque;
  const int32_t *first = contiguous(context, a, context->scratch0);
  const int32_t *second = contiguous(context, b, context->scratch1);
  const int32_t *third = contiguous(context, c, context->scratch2);
  return submit_reduce3(context, first, second, third, a.length, result);
}

static int accel_min2_value(void *opaque, pbqp_vector_view_t a, pbqp_vector_view_t b,
                            int32_t *result) {
  pbqp_accelerator_kernel_context_t *context = opaque;
  const int32_t *first = contiguous(context, a, context->scratch0);
  const int32_t *second = contiguous(context, b, context->scratch1);
  if (first == NULL || second == NULL || result == NULL)
    return fail(context, PCAA_STATUS_INVALID_ARGUMENT);
  return submit_reduce2(context, first, second, a.length, result, 0);
}

static int accel_min2_batch(void *opaque, const pbqp_min2_job_t *jobs, size_t count) {
  pbqp_accelerator_kernel_context_t *context = opaque;
  if (count > PBQP_MAX_BATCH_JOBS)
    return fail(context, PCAA_STATUS_NO_SPACE);
  begin_batch(context);
  for (size_t index = 0; index < count; ++index) {
    const int32_t *first = batch_contiguous(context, jobs[index].a);
    const int32_t *second = batch_contiguous(context, jobs[index].b);
    if (first == NULL || second == NULL)
      return fail(context, PCAA_STATUS_NO_SPACE);
    context->last_status = pcaa_make_reduce2(
        pcaa_cost_vector((pcaa_guest_address_t)(uintptr_t)first, jobs[index].a.length, 1),
        pcaa_cost_vector((pcaa_guest_address_t)(uintptr_t)second, jobs[index].b.length, 1),
        (pcaa_guest_address_t)(uintptr_t)jobs[index].result, 1, &context->batch_commands[index]);
    if (context->last_status != PCAA_STATUS_OK)
      return context->last_status;
  }
  return submit_batch(context, count);
}

static int accel_min3_batch(void *opaque, const pbqp_min3_job_t *jobs, size_t count) {
  pbqp_accelerator_kernel_context_t *context = opaque;
  if (count > PBQP_MAX_BATCH_JOBS)
    return fail(context, PCAA_STATUS_NO_SPACE);
  begin_batch(context);
  for (size_t index = 0; index < count; ++index) {
    const int32_t *first = batch_contiguous(context, jobs[index].a);
    const int32_t *second = batch_contiguous(context, jobs[index].b);
    const int32_t *third = batch_contiguous(context, jobs[index].c);
    if (first == NULL || second == NULL || third == NULL)
      return fail(context, PCAA_STATUS_NO_SPACE);
    context->last_status = pcaa_make_reduce3(
        pcaa_cost_vector((pcaa_guest_address_t)(uintptr_t)first, jobs[index].a.length, 1),
        pcaa_cost_vector((pcaa_guest_address_t)(uintptr_t)second, jobs[index].b.length, 1),
        pcaa_cost_vector((pcaa_guest_address_t)(uintptr_t)third, jobs[index].c.length, 1),
        (pcaa_guest_address_t)(uintptr_t)jobs[index].result, 1, &context->batch_commands[index]);
    if (context->last_status != PCAA_STATUS_OK)
      return context->last_status;
  }
  return submit_batch(context, count);
}

static int accel_min2_value_batch(void *opaque, const pbqp_min2_value_job_t *jobs, size_t count) {
  pbqp_accelerator_kernel_context_t *context = opaque;
  if (count > PBQP_MAX_BATCH_JOBS)
    return fail(context, PCAA_STATUS_NO_SPACE);
  begin_batch(context);
  for (size_t index = 0; index < count; ++index) {
    const int32_t *first = batch_contiguous(context, jobs[index].a);
    const int32_t *second = batch_contiguous(context, jobs[index].b);
    if (first == NULL || second == NULL || jobs[index].result == NULL)
      return fail(context, first == NULL || second == NULL ? PCAA_STATUS_NO_SPACE
                                                           : PCAA_STATUS_INVALID_ARGUMENT);
    context->last_status = pcaa_make_reduce2(
        pcaa_cost_vector((pcaa_guest_address_t)(uintptr_t)first, jobs[index].a.length, 1),
        pcaa_cost_vector((pcaa_guest_address_t)(uintptr_t)second, jobs[index].b.length, 1),
        (pcaa_guest_address_t)(uintptr_t)jobs[index].result, 0, &context->batch_commands[index]);
    if (context->last_status != PCAA_STATUS_OK)
      return context->last_status;
  }
  return submit_batch(context, count);
}

static pcaa_status_t project_command(pbqp_matrix_view_t matrix, pbqp_vector_view_t unary,
                                     int32_t *result, pcaa_command_t *command) {
  return pcaa_make_minplus_project(
      pcaa_cost_matrix((pcaa_guest_address_t)(uintptr_t)matrix.base, matrix.rows, matrix.columns,
                       matrix.row_stride, matrix.column_stride),
      vector_view(unary), pcaa_cost_output((pcaa_guest_address_t)(uintptr_t)result, matrix.rows, 1),
      command);
}

static pcaa_status_t add_vector_command(pbqp_vector_view_t first, pbqp_vector_view_t second,
                                        int32_t *result, pcaa_command_t *command) {
  return pcaa_make_cost_add_vector(
      vector_view(first), vector_view(second),
      pcaa_cost_output((pcaa_guest_address_t)(uintptr_t)result, first.length, 1), command);
}

static pcaa_status_t map3_project_command(pbqp_vector_view_t unary, pbqp_vector_view_t fixed_edge,
                                          pbqp_matrix_view_t varying_edge,
                                          accel_min_argmin_result_t *result,
                                          pcaa_command_t *command) {
  return pcaa_make_minplus_map3_project(
      vector_view(unary), vector_view(fixed_edge),
      pcaa_cost_matrix((pcaa_guest_address_t)(uintptr_t)varying_edge.base, varying_edge.rows,
                       varying_edge.columns, varying_edge.row_stride, varying_edge.column_stride),
      pcaa_argmin_output((pcaa_guest_address_t)(uintptr_t)result, varying_edge.rows, 1), command);
}

static int accel_cost_add_vector(void *opaque, pbqp_vector_view_t first, pbqp_vector_view_t second,
                                 int32_t *result) {
  pbqp_accelerator_kernel_context_t *context = opaque;
  context->last_status = add_vector_command(first, second, result, &context->batch_commands[0]);
  if (context->last_status != PCAA_STATUS_OK)
    return context->last_status;
  return submit_batch(context, 1);
}

static int accel_minplus_project(void *opaque, pbqp_matrix_view_t matrix, pbqp_vector_view_t unary,
                                 int32_t *result) {
  pbqp_accelerator_kernel_context_t *context = opaque;
  context->last_status = project_command(matrix, unary, result, &context->batch_commands[0]);
  if (context->last_status != PCAA_STATUS_OK)
    return context->last_status;
  return submit_batch(context, 1);
}

static int accel_map3_project(void *opaque, pbqp_vector_view_t unary, pbqp_vector_view_t fixed_edge,
                              pbqp_matrix_view_t varying_edge, accel_min_argmin_result_t *result) {
  pbqp_accelerator_kernel_context_t *context = opaque;
  context->last_status =
      map3_project_command(unary, fixed_edge, varying_edge, result, &context->batch_commands[0]);
  if (context->last_status != PCAA_STATUS_OK)
    return context->last_status;
  return submit_batch(context, 1);
}

static int accel_project_add_batch(void *opaque, const pbqp_project_add_job_t *jobs, size_t count) {
  pbqp_accelerator_kernel_context_t *context = opaque;
  if (count > PBQP_MAX_BATCH_JOBS / 2)
    return fail(context, PCAA_STATUS_NO_SPACE);
  for (size_t index = 0; index < count; ++index) {
    const pbqp_project_add_job_t *job = &jobs[index];
    context->last_status = project_command(job->matrix, job->unary, job->temporary,
                                           &context->batch_commands[2 * index]);
    if (context->last_status != PCAA_STATUS_OK)
      return context->last_status;
    context->last_status =
        add_vector_command((pbqp_vector_view_t){job->temporary, job->matrix.rows, 1},
                           (pbqp_vector_view_t){job->scores, job->matrix.rows, 1}, job->scores,
                           &context->batch_commands[2 * index + 1]);
    if (context->last_status != PCAA_STATUS_OK)
      return context->last_status;
  }
  return submit_batch(context, 2 * count);
}

static int accel_map3_project_batch(void *opaque, const pbqp_map3_project_job_t *jobs,
                                    size_t count) {
  pbqp_accelerator_kernel_context_t *context = opaque;
  if (count > PBQP_MAX_BATCH_JOBS)
    return fail(context, PCAA_STATUS_NO_SPACE);
  for (size_t index = 0; index < count; ++index) {
    const pbqp_map3_project_job_t *job = &jobs[index];
    context->last_status = map3_project_command(job->unary, job->fixed_edge, job->varying_edge,
                                                job->results, &context->batch_commands[index]);
    if (context->last_status != PCAA_STATUS_OK)
      return context->last_status;
  }
  return submit_batch(context, count);
}

static void accel_set_statistics(void *opaque, pbqp_statistics_t *statistics) {
  pbqp_accelerator_kernel_context_t *context = opaque;
  context->statistics = statistics;
  context->device = pcaa_baremetal_device_init(&context->backend, context->encoded_workspace,
                                               PBQP_MAX_BATCH_JOBS);
  context->last_status = PCAA_STATUS_OK;
  context->last_completion.has_batch_result = 0;
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
