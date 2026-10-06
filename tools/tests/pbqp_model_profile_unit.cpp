// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Checks measurement boundaries, batch cycles, staging and failed-call recovery.
#include "pbqp_model_kernel.h"
#include "accel_protocol.h"
#include "l2_accelerator.h"
#include "pbqp/pbqp.h"
#include "pcaa.h"
#include "timing_model.h"
#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>
#include <gtest/gtest.h>
#include <sysc/kernel/sc_simcontext.h>
#include <sysc/kernel/sc_time.h>

namespace {
ModelKernel *measured_model = nullptr;
ModelKernel *plain_model = nullptr;
pbqp_cost_kernel_t measured{}, plain{};
TEST(ModelProfile, DisabledMeasurementRecordsNothing) {
  const std::array<int32_t, 3> a{2, -4, 7}, b{0, 1, -8};
  accel_min_argmin_result_t result{};
  ASSERT_EQ(plain.min2_argmin(plain.context, {a.data(), 3, 1}, {b.data(), 3, 1}, &result),
            PCAA_STATUS_OK);
  EXPECT_EQ(result.value, -3);
  EXPECT_EQ(result.index, 1u);
  EXPECT_TRUE(plain_model->measurements().empty());
}
TEST(ModelProfile, BatchCyclesPartitionServiceAndPreparationStages) {
  const auto begin = measured_model->measurements().size();
  const auto cycle_begin = measured_model->l2_statistics().cycles;
  const std::array<int32_t, 3> a{2, -4, 7}, b{0, 1, -8};
  std::array<accel_min_argmin_result_t, 2> results{};
  const std::array<pbqp_min2_job_t, 2> jobs{{{{a.data(), 3, 1}, {b.data(), 3, 1}, &results[0]},
                                             {{b.data(), 3, 1}, {a.data(), 3, 1}, &results[1]}}};
  ASSERT_EQ(measured.min2_argmin_batch(measured.context, jobs.data(), jobs.size()), PCAA_STATUS_OK);
  std::array<int32_t, 3> sum{};
  ASSERT_EQ(
      measured.cost_add_vector(measured.context, {a.data(), 3, 1}, {b.data(), 3, 1}, sum.data()),
      PCAA_STATUS_OK);
  EXPECT_EQ(sum, (std::array<int32_t, 3>{2, -3, -1}));
  const auto &rows = measured_model->measurements();
  ASSERT_EQ(rows.size(), begin + 2);
  EXPECT_EQ(std::string(rows[begin].kind), "min2_batch");
  EXPECT_EQ(rows[begin].count, 2u);
  EXPECT_EQ(std::string(rows[begin + 1].kind), "add");
  EXPECT_EQ(rows[begin].cycles + rows[begin + 1].cycles,
            measured_model->l2_statistics().cycles - cycle_begin);
  for (size_t i = begin; i < rows.size(); ++i) {
    EXPECT_GT(rows[i].cycles, 0u);
    EXPECT_GT(rows[i].staging_ns, 0u);
    EXPECT_GT(rows[i].readback_ns, 0u);
    EXPECT_GE(rows[i].host_ns,
              rows[i].staging_ns + rows[i].submission_build_ns + rows[i].readback_ns);
  }
}
TEST(ModelProfile, OrderedScoreChainsPreserveAliasesAndReportOneBatch) {
  const auto begin = measured_model->measurements().size();
  const auto submissions = measured_model->l2_statistics().submissions;
  const std::array<int32_t, 6> matrix{0, ACCEL_INF, 4, 0, -2, 3};
  const std::array<int32_t, 3> unary{2, 1, 0};
  std::array<int32_t, 2> temporary{}, scores{10, -10};
  const pbqp_project_add_job_t job{
      {matrix.data(), 2, 3, 3, 1}, {unary.data(), 3, 1}, temporary.data(), scores.data()};
  const std::array<pbqp_project_add_job_t, 2> jobs{job, job};
  ASSERT_EQ(measured.project_add_batch(measured.context, jobs.data(), jobs.size()), PCAA_STATUS_OK);
  EXPECT_EQ(scores, (std::array<int32_t, 2>{14, -12}));
  ASSERT_EQ(measured_model->measurements().size(), begin + 1);
  EXPECT_EQ(measured_model->l2_statistics().submissions, submissions + 1);
  EXPECT_EQ(measured_model->measurements().back().count, 2u);
}
TEST(ModelProfile, BuilderFailureHasNoDeviceCyclesAndNextCallRecovers) {
  const auto begin = measured_model->measurements().size();
  const auto cycles = measured_model->l2_statistics().cycles;
  constexpr size_t kOversized = 65536;
  const std::vector<int32_t> too_long(kOversized, 0);
  int32_t output = 0;
  EXPECT_EQ(measured.min2_value(measured.context, {too_long.data(), kOversized, 1},
                                {too_long.data(), kOversized, 1}, &output),
            PCAA_STATUS_RANGE);
  ASSERT_EQ(measured_model->measurements().size(), begin + 1);
  EXPECT_EQ(measured_model->measurements().back().cycles, 0u);
  EXPECT_EQ(measured_model->l2_statistics().cycles, cycles);
  const std::array<int32_t, 1> value{ACCEL_INF};
  ASSERT_EQ(
      measured.min2_value(measured.context, {value.data(), 1, 1}, {value.data(), 1, 1}, &output),
      PCAA_STATUS_OK);
  EXPECT_EQ(output, ACCEL_INF);
}
}  // namespace
int sc_main(int argc, char **argv) {
  L2Config config;
  ModelKernel model(false, {}, std::numeric_limits<size_t>::max(), &config);
  ModelKernel ordinary(false, {});
  measured_model = &model;
  plain_model = &ordinary;
  model.make_kernel(measured);
  ordinary.make_kernel(plain);
  model.enable_measurements();
  sc_core::sc_start(sc_core::SC_ZERO_TIME);
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
