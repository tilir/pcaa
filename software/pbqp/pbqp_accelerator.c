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

static int accel_min2(void *opaque, pbqp_vector_view_t a, pbqp_vector_view_t b,
                      accel_min_argmin_result_t *result) {
  pbqp_accelerator_kernel_context_t *context = opaque;
  const int32_t *first = contiguous(context, a, context->scratch0);
  const int32_t *second = contiguous(context, b, context->scratch1);
  *result = accel_min_add_argmin(first, second, a.length);
  return 0;
}

static int accel_min3(void *opaque, pbqp_vector_view_t a, pbqp_vector_view_t b,
                      pbqp_vector_view_t c, accel_min_argmin_result_t *result) {
  pbqp_accelerator_kernel_context_t *context = opaque;
  const int32_t *first = contiguous(context, a, context->scratch0);
  const int32_t *second = contiguous(context, b, context->scratch1);
  const int32_t *third = contiguous(context, c, context->scratch2);
  *result = accel_min_add3_argmin(first, second, third, a.length);
  return 0;
}

void pbqp_make_accelerator_kernel(pbqp_cost_kernel_t *kernel,
                                  pbqp_accelerator_kernel_context_t *context,
                                  pbqp_statistics_t *statistics) {
  context->statistics = statistics;
  kernel->context = context;
  kernel->min2_argmin = accel_min2;
  kernel->min3_argmin = accel_min3;
}
