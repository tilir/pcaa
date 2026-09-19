// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Checks structural properties of deterministic host-only workload families.

#include "pbqp_workload/analyzer.h"
#include "pbqp_workload/graph_generator.h"

#include <cstddef>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace pcaa::workload {

TEST(WorkloadAnalyzer, TreeMatchesIndexOrderedReductionPolicy) {
  const InstanceResult result =
      Analyze({GraphFamily::kTree, DomainProfile::kRegisterLike, 100, 17});
  EXPECT_EQ(result.r0_count, 1);
  EXPECT_EQ(result.r1_count + result.r2_count, 99);
  EXPECT_GT(result.r2_count, 0);
  EXPECT_EQ(result.irreducible_nodes, 0);
}

TEST(WorkloadAnalyzer, TwoTreeProducesR2Eliminations) {
  const InstanceResult result =
      Analyze({GraphFamily::kTwoTree, DomainProfile::kUniformSmall, 100, 23});
  EXPECT_EQ(result.r0_count + result.r1_count + result.r2_count, 100);
  EXPECT_GT(result.r2_count, 0);
  EXPECT_EQ(result.irreducible_nodes, 0);
}

TEST(WorkloadAnalyzer, BatchMetadataMatchesReductionTrace) {
  const InstanceResult result = Analyze({GraphFamily::kStar, DomainProfile::kUniformSmall, 3, 101});
  EXPECT_EQ(result.r1_count, 1);
  EXPECT_EQ(result.r2_count, 1);
  size_t batch_count = 0;
  size_t primitive_count = 0;
  size_t batch_primitive_count = 0;
  for (const TraceRecord &record : result.trace) {
    ++primitive_count;
    if (record.batch_start) {
      ++batch_count;
      batch_primitive_count += record.batch_size;
    }
  }
  EXPECT_EQ(batch_count, static_cast<size_t>(result.r1_count + result.r2_count));
  EXPECT_EQ(batch_primitive_count, primitive_count);
}

TEST(WorkloadAnalyzer, CompleteCoreStopsWithoutHeuristics) {
  const InstanceResult result =
      Analyze({GraphFamily::kIrreducibleCore, DomainProfile::kRegisterLike, 8, 29});
  EXPECT_TRUE(result.trace.empty());
  EXPECT_EQ(result.irreducible_nodes, 8);
}

}  // namespace pcaa::workload
