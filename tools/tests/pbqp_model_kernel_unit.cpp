// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Regressions for hosted PBQP callback staging, diagnostics, and ordered batches.
#include "pbqp_model_kernel.h"
#include "accel_protocol.h"
#include "pbqp/pbqp.h"
#include "pcaa.h"
#include "timing_model.h"

#include <string>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>
#include <sysc/kernel/sc_externs.h>
#include <sysc/kernel/sc_simcontext.h>
#include <sysc/kernel/sc_time.h>

namespace {
constexpr size_t kStagingLimit = 1024 * 1024;
constexpr size_t kUnencodableLength = 65536;
pbqp_cost_kernel_t kernel{};
pbqp_cost_kernel_t exhausted_kernel{};

TEST(ModelKernel, PreservesBuilderRangeAndCommandErrors) {
  std::vector<int32_t> values(kUnencodableLength, 0);
  const pbqp_vector_view_t view{values.data(), values.size(), 1};
  accel_min_argmin_result_t argmin{};
  int32_t value = -1;
  EXPECT_EQ(kernel.min2_argmin(kernel.context, view, view, &argmin), PCAA_STATUS_RANGE);
  EXPECT_EQ(kernel.min3_argmin(kernel.context, view, view, view, &argmin), PCAA_STATUS_RANGE);
  EXPECT_EQ(kernel.min2_value(kernel.context, view, view, &value), PCAA_STATUS_RANGE);
  EXPECT_EQ(kernel.cost_add_vector(kernel.context, view, view, values.data()), PCAA_STATUS_RANGE);
  const pbqp_matrix_view_t matrix{values.data(), 1, values.size(), values.size(), 1};
  EXPECT_EQ(kernel.minplus_project(kernel.context, matrix, view, &value), PCAA_STATUS_RANGE);
  EXPECT_EQ(kernel.minplus_map3_project(kernel.context, view, view, matrix, &argmin),
            PCAA_STATUS_RANGE);
  int32_t temporary = 0;
  const pbqp_project_add_job_t job{matrix, view, &temporary, &value};
  EXPECT_EQ(kernel.project_add_batch(kernel.context, &job, 1), PCAA_STATUS_RANGE);
  const pbqp_vector_view_t shorter{values.data(), 1, 1};
  EXPECT_EQ(
      kernel.min2_argmin(kernel.context, pbqp_vector_view_t{values.data(), 2, 1}, shorter, &argmin),
      PCAA_STATUS_INVALID_COMMAND);
}

TEST(ModelKernel, PreservesStagingAllocationFailure) {
  const std::array<int32_t, 2> values{1, 2};
  const pbqp_vector_view_t view{values.data(), values.size(), 1};
  accel_min_argmin_result_t result{};
  EXPECT_EQ(exhausted_kernel.min2_argmin(exhausted_kernel.context, view, view, &result),
            PCAA_STATUS_NO_SPACE);
}

TEST(ModelKernel, SolverRetainsTypedStagingFailure) {
  struct Problem {
    pbqp_problem_t value{};
    ~Problem() {
      pbqp_destroy(&value);
    }
  } problem;
  constexpr unsigned kDomain = 2;
  ASSERT_EQ(pbqp_init(&problem.value, pbqp_heap_allocator(), 2, 1, kDomain), PBQP_OK);
  const std::array<int32_t, kDomain> unary{0, 0};
  ASSERT_EQ(pbqp_add_node(&problem.value, kDomain, unary.data()), PBQP_OK);
  ASSERT_EQ(pbqp_add_node(&problem.value, kDomain, unary.data()), PBQP_OK);
  const std::array<int32_t, kDomain * kDomain> edge{};
  ASSERT_EQ(pbqp_add_edge(&problem.value, 0, 1, edge.data()), PBQP_OK);
  pbqp_solver_t solver{};
  ASSERT_EQ(pbqp_solver_create(&solver, PBQP_MODE_ACCELERATOR, &exhausted_kernel), PBQP_OK);
  std::array<unsigned, 2> assignment{};
  pbqp_solution_t solution{};
  pbqp_solution_init(&solution, assignment.data(), assignment.size());
  EXPECT_EQ(pbqp_solver_solve(&solver, &problem.value, &solution), PBQP_KERNEL_ERROR);
  EXPECT_EQ(solver.last_kernel_status, PCAA_STATUS_NO_SPACE);
}

TEST(ModelKernel, DeviceErrorIsDistinctFromInfinityAndAllowsRecovery) {
  std::array<int32_t, 1> first{ACCEL_INF + 1};
  const std::array<int32_t, 1> second{ACCEL_INF};
  const pbqp_vector_view_t a{first.data(), first.size(), 1};
  const pbqp_vector_view_t b{second.data(), second.size(), 1};
  int32_t result = 0;
  EXPECT_EQ(kernel.min2_value(kernel.context, a, b, &result), PCAA_STATUS_DEVICE_ERROR);
  first[0] = 0;
  ASSERT_EQ(kernel.min2_value(kernel.context, a, b, &result), PCAA_STATUS_OK);
  EXPECT_EQ(result, ACCEL_INF);
}

TEST(ModelKernel, ProjectAddUsesEachDestinationAndItsOwnDimensions) {
  const std::array<int32_t, 6> matrix{1, 4, -2, 3, 6, 0};
  const std::array<int32_t, 2> unary{0, 1};
  std::array<int32_t, 2> first_scores{10, 20};
  std::array<int32_t, 3> second_scores{100, 200, 300};
  std::array<int32_t, 2> first_temporary{};
  std::array<int32_t, 3> second_temporary{};
  const std::array<pbqp_project_add_job_t, 3> jobs{{
      {{matrix.data(), 2, 2, 2, 1},
       {unary.data(), 2, 1},
       first_temporary.data(),
       first_scores.data()},
      {{matrix.data(), 3, 2, 2, 1},
       {unary.data(), 2, 1},
       second_temporary.data(),
       second_scores.data()},
      {{matrix.data(), 2, 2, 2, 1},
       {unary.data(), 2, 1},
       first_temporary.data(),
       first_scores.data()},
  }};
  ASSERT_EQ(kernel.project_add_batch(kernel.context, jobs.data(), jobs.size()), PCAA_STATUS_OK);
  EXPECT_EQ(first_scores, (std::array<int32_t, 2>{12, 16}));
  EXPECT_EQ(second_scores, (std::array<int32_t, 3>{101, 198, 301}));
}
}  // namespace

int sc_main(int argc, char **argv) {
  const AccelTimingConfig timing{};
  ModelKernel model(false, timing, kStagingLimit);
  // Smaller than the first guest allocation address: deterministic exhaustion.
  constexpr size_t kExhaustedStagingBytes = 128;
  ModelKernel exhausted(false, timing, kExhaustedStagingBytes);
  model.make_kernel(kernel);
  exhausted.make_kernel(exhausted_kernel);
  sc_core::sc_start(sc_core::SC_ZERO_TIME);
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
