// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Differentially verifies the predicates, kernels, ties and exceptional arithmetic.
#include "kernels.h"
#include "accel_protocol.h"
#include "pbqp/pbqp.h"
#include "profile.h"
#include <string>
#include <initializer_list>
#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>
#include <gtest/gtest.h>

namespace cpu_baseline {
TEST(CpuKernels, RandomStructuredAndDenseDifferential) {
  constexpr int kCases = 4000;
  std::mt19937 random(10062026);
  const std::array<int32_t, 12> values{0,
                                       0,
                                       1,
                                       -1,
                                       17,
                                       -91,
                                       ACCEL_INF,
                                       ACCEL_INF - 1,
                                       ACCEL_INF + 1,
                                       INT32_MIN,
                                       INT32_MIN + 1,
                                       INT32_MAX};
  for (int trial = 0; trial < kCases; ++trial) {
    const size_t n = 1 + random() % 64;
    const size_t m = 1 + random() % 49;
    const size_t vs = 1 + random() % 3;
    const bool transpose = trial % 2;
    const size_t rs = transpose ? 1 : n + 3;
    const size_t cs = transpose ? m + 3 : 1;
    std::vector<int32_t> matrix((m - 1) * rs + (n - 1) * cs + 1);
    std::vector<int32_t> unary(n * vs), fixed(n * vs);
    for (size_t j = 0; j < n; ++j) {
      unary[j * vs] = trial % 5 ? int(random() % 99) - 49 : values[random() % values.size()];
      fixed[j * vs] = trial % 7 ? int(random() % 13) - 6 : values[random() % values.size()];
    }
    for (size_t i = 0; i < m; ++i) {
      const int32_t background = trial % 3 == 0 ? 0 : values[random() % values.size()];
      for (size_t j = 0; j < n; ++j)
        matrix[i * rs + j * cs] = trial % 4 == 0 ? values[random() % values.size()] : background;
      for (size_t e = 0; e < kExceptions; ++e)
        matrix[i * rs + (random() % n) * cs] =
            trial % 3 == 0 ? ACCEL_INF : values[random() % values.size()];
    }
    const pbqp_matrix_view_t view{matrix.data(), m, n, rs, cs};
    Kernels scalar(Level::Scalar), dense(Level::Dense), structured(Level::Structured);
    const auto &rep = structured.Classify(view);
    if (rep.kind != DenseMatrix) {
      ASSERT_EQ(rep.rows.size(), m);
      for (size_t i = 0; i < m; ++i) {
        ASSERT_LE(rep.rows[i].count, kExceptions);
        for (size_t j = 0; j < n; ++j) {
          int32_t reconstructed = rep.rows[i].background;
          for (size_t e = 0; e < rep.rows[i].count; ++e)
            if (rep.rows[i].indices[e] == j)
              reconstructed = rep.rows[i].values[e];
          ASSERT_EQ(reconstructed, matrix[i * rs + j * cs]);
        }
      }
    }
    for (bool three : {false, true}) {
      std::vector<accel_min_argmin_result_t> reference(m), actual(m);
      const auto execute = [&](Kernels &kernel, auto &output) {
        return three
                   ? kernel.Map3({unary.data(), n, vs}, {fixed.data(), n, vs}, view, output.data())
                   : kernel.Project(view, {unary.data(), n, vs}, nullptr, output.data());
      };
      const int expected = execute(scalar, reference);
      for (Kernels *kernel : {&dense, &structured}) {
        const int status = execute(*kernel, actual);
        ASSERT_EQ(status == 0, expected == 0) << "case " << trial << " add3 " << three;
        if (!status)
          for (size_t i = 0; i < m; ++i) {
            ASSERT_EQ(actual[i].value, reference[i].value) << trial;
            ASSERT_EQ(actual[i].index, reference[i].index) << trial;
          }
      }
    }
  }
}
TEST(CpuKernels, ForbiddenFirstSecondTiesAndInvalidation) {
  std::array<int32_t, 12> matrix{ACCEL_INF, 0, 0, 0, 0, ACCEL_INF, 0, 0, 0, 0, 0, 0};
  std::array<int32_t, 4> unary{-5, -5, 0, 1};
  Kernels k(Level::Structured);
  const pbqp_matrix_view_t view{matrix.data(), 3, 4, 4, 1};
  ASSERT_EQ(k.Classify(view).kind, Matching);
  std::array<accel_min_argmin_result_t, 3> result{};
  ASSERT_EQ(k.Project(view, {unary.data(), 4, 1}, nullptr, result.data()), 0);
  EXPECT_EQ(result[0].index, 1u);
  EXPECT_EQ(result[1].index, 0u);
  EXPECT_EQ(result[2].index, 0u);
  matrix[0] = 7;
  k.Invalidate(matrix.data());
  EXPECT_TRUE(k.Classify(view).changed);
  EXPECT_EQ(k.Classify(view).kind, RowExceptions);
  ASSERT_EQ(k.Project(view, {unary.data(), 4, 1}, nullptr, result.data()), 0);
  EXPECT_EQ(result[0].index, 1u);
}
TEST(CpuKernels, SimdAddErrorsAliasesAndAdd3Order) {
  constexpr size_t kLength = 17;
  Kernels scalar(Level::Scalar), dense(Level::Dense), structured(Level::Structured);
  pbqp_cost_kernel_t reference{}, fast{};
  scalar.Make(reference);
  const std::array<int32_t, 7> values{0, ACCEL_INF, ACCEL_INF - 1, ACCEL_INF + 1, INT32_MIN, -1, 1};
  for (int32_t a : values)
    for (int32_t b : values)
      for (int32_t c : values) {
        std::array<int32_t, kLength> first, second, third;
        first.fill(a);
        second.fill(b);
        third.fill(c);
        accel_min_argmin_result_t expected{}, actual{};
        const int status = reference.min3_argmin(reference.context, {first.data(), kLength, 1},
                                                 {second.data(), kLength, 1},
                                                 {third.data(), kLength, 1}, &expected);
        for (Kernels *kernel : {&dense, &structured}) {
          kernel->Make(fast);
          const int got =
              fast.min3_argmin(fast.context, {first.data(), kLength, 1},
                               {second.data(), kLength, 1}, {third.data(), kLength, 1}, &actual);
          ASSERT_EQ(got == 0, status == 0);
          if (!got) {
            EXPECT_EQ(actual.value, expected.value);
            EXPECT_EQ(actual.index, expected.index);
          }
          auto x = first, y = second, z = first;
          const int ref = reference.cost_add_vector(reference.context, {x.data(), kLength, 1},
                                                    {y.data(), kLength, 1}, x.data());
          const int opt = fast.cost_add_vector(fast.context, {z.data(), kLength, 1},
                                               {y.data(), kLength, 1}, z.data());
          ASSERT_EQ(ref == 0, opt == 0);
          if (!ref)
            EXPECT_EQ(x, z);
        }
      }
}
TEST(CpuKernels, AllInfAndSaturatedTieChoosesZero) {
  std::array<int32_t, 12> matrix;
  matrix.fill(ACCEL_INF - 1);
  std::array<int32_t, 4> unary{9, 8, 7, 6};
  Kernels k(Level::Structured);
  std::array<accel_min_argmin_result_t, 3> result{};
  ASSERT_EQ(k.Project({matrix.data(), 3, 4, 4, 1}, {unary.data(), 4, 1}, nullptr, result.data()),
            0);
  for (auto r : result) {
    EXPECT_EQ(r.value, ACCEL_INF);
    EXPECT_EQ(r.index, 0u);
  }
  unary.fill(ACCEL_INF);
  k.Invalidate(matrix.data());
  ASSERT_EQ(k.Project({matrix.data(), 3, 4, 4, 1}, {unary.data(), 4, 1}, nullptr, result.data()),
            0);
  for (auto r : result) EXPECT_EQ(r.index, 0u);
}
}  // namespace cpu_baseline

namespace cpu_baseline {
TEST(CpuKernels, IrreducibleSolvePreservesRnDecisionsAcrossBackends) {
  constexpr unsigned kNodes = 4, kDomain = 4, kEdges = 6;
  std::array<unsigned, kNodes> expected{};
  int32_t objective = 0;
  unsigned rn_count = 0, r2_count = 0;
  bool first = true;
  for (bool degrees : {false, true})
    for (Level level : {Level::Scalar, Level::Dense, Level::Structured}) {
      struct Owner {
        pbqp_problem_t problem{};
        ~Owner() {
          pbqp_destroy(&problem);
        }
      } graph;
      ASSERT_EQ(pbqp_init(&graph.problem, pbqp_heap_allocator(), kNodes, kEdges + 1, kDomain),
                PBQP_OK);
      std::array<int32_t, kDomain> unary{0, -1, -1, 0};
      std::array<int32_t, kDomain * kDomain> edge{};
      for (unsigned i = 0; i < kDomain; ++i) edge[i * kDomain + i] = ACCEL_INF;
      for (unsigned i = 0; i < kNodes; ++i)
        ASSERT_EQ(pbqp_add_node(&graph.problem, kDomain, unary.data()), PBQP_OK);
      for (unsigned i = 0; i < kNodes; ++i)
        for (unsigned j = i + 1; j < kNodes; ++j)
          ASSERT_EQ(pbqp_add_edge(&graph.problem, i, j, edge.data()), PBQP_OK);
      Kernels kernels(level);
      Profile timing;
      timing.degrees = degrees;
      profile = &timing;
      active_kernels = &kernels;
      pbqp_cost_kernel_t api{};
      kernels.Make(api);
      auto config = pbqp_solver_default_config();
      config.strategy = PBQP_STRATEGY_HEURISTIC_RN;
      pbqp_solver_t solver{};
      ASSERT_EQ(pbqp_solver_create_with_config(&solver, PBQP_MODE_SOFTWARE, &api, &config),
                PBQP_OK);
      std::array<unsigned, kNodes> assignment{};
      pbqp_solution_t solution{};
      pbqp_solution_init(&solution, assignment.data(), assignment.size());
      const auto status =
          pcaa::pbqp::SolveWithExecution(solver, graph.problem, solution, MakeExecution(timing));
      profile = nullptr;
      active_kernels = nullptr;
      ASSERT_EQ(status, PBQP_OK);
      ASSERT_GT(graph.problem.statistics.rn_count, 0u);
      if (first) {
        expected = assignment;
        objective = solution.optimum;
        rn_count = graph.problem.statistics.rn_count;
        r2_count = graph.problem.statistics.r2_count;
        first = false;
      }
      EXPECT_EQ(assignment, expected);
      EXPECT_EQ(solution.optimum, objective);
      EXPECT_EQ(graph.problem.statistics.rn_count, rn_count);
      EXPECT_EQ(graph.problem.statistics.r2_count, r2_count);
    }
}
}  // namespace cpu_baseline

namespace cpu_baseline {
TEST(CpuKernels, IndependentMin2JobsDoNotImplySharedMatrixStorage) {
  const std::array<int32_t, 3> unary{0, -1, 2}, first{1, 3, -5}, second{4, -7, 0};
  std::array<accel_min_argmin_result_t, 2> results{};
  const std::array<pbqp_min2_job_t, 2> jobs{
      {{{unary.data(), 3, 1}, {first.data(), 3, 1}, &results[0]},
       {{unary.data(), 3, 1}, {second.data(), 3, 1}, &results[1]}}};
  Kernels kernels(Level::Structured);
  pbqp_cost_kernel_t api{};
  kernels.Make(api);
  ASSERT_EQ(api.min2_argmin_batch(api.context, jobs.data(), jobs.size()), 0);
  EXPECT_EQ(results[0].value, -3);
  EXPECT_EQ(results[0].index, 2u);
  EXPECT_EQ(results[1].value, -8);
  EXPECT_EQ(results[1].index, 1u);
}
}  // namespace cpu_baseline
