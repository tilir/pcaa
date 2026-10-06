// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Retains warmed native kernel samples for corpus-sized shapes and two layouts.
#include "kernels.h"
#include "accel_protocol.h"
#include "pbqp/pbqp.h"
#include <initializer_list>
#include <cstdlib>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <utility>
#include <vector>

using namespace cpu_baseline;
namespace {
constexpr int kSamples = 15;
constexpr int kIterations = 2000;
constexpr int kWarmup = 200;
volatile int32_t sink = 0;
}  // namespace
int main() {
  for (auto shape : std::array<std::pair<size_t, size_t>, 9>{
           {{1, 1}, {3, 7}, {7, 7}, {7, 16}, {16, 7}, {16, 16}, {17, 16}, {17, 17}, {49, 49}}})
    for (int layout = 0; layout < 2; ++layout)
      for (int form = 0; form < 3; ++form)
        for (int operation = 0; operation < 2; ++operation) {
          const size_t m = shape.first, n = shape.second;
          const size_t rs = layout ? 1 : n, cs = layout ? m : 1;
          std::vector<int32_t> matrix(m * n), unary(n), fixed(n), values(m);
          std::vector<accel_min_argmin_result_t> results(m);
          for (size_t j = 0; j < n; ++j) {
            unary[j] = int(j % 7) - 3;
            fixed[j] = int(j % 3) - 1;
          }
          for (size_t i = 0; i < m; ++i)
            for (size_t j = 0; j < n; ++j)
              matrix[i * rs + j * cs] = form == 0   ? (i % n == j ? ACCEL_INF : 0)
                                        : form == 1 ? (i % n == j ? -7 : int(i % 5) - 2)
                                                    : int((i * 17 + j * 13) % 31) - 15;
          for (Level level : {Level::Scalar, Level::Dense, Level::Structured}) {
            Kernels kernel(level);
            const pbqp_matrix_view_t view{matrix.data(), m, n, rs, cs};
            auto execute = [&] {
              const int status = operation
                                     ? kernel.Map3({unary.data(), n, 1}, {fixed.data(), n, 1}, view,
                                                   results.data())
                                     : kernel.Project(view, {unary.data(), n, 1}, values.data());
              if (status)
                std::abort();
              sink = operation ? results[0].value : values[0];
            };
            for (int i = 0; i < kWarmup; ++i) execute();
            for (int sample = 0; sample < kSamples; ++sample) {
              const auto start = Now();
              for (int i = 0; i < kIterations; ++i) execute();
              const auto ns = Now() - start;
              std::cout << "{\"m\":" << m << ",\"n\":" << n << ",\"layout\":" << layout
                        << ",\"form\":" << form << ",\"operation\":" << operation
                        << ",\"level\":" << int(level) << ",\"sample\":" << sample
                        << ",\"iterations\":" << kIterations << ",\"ns\":" << ns << "}\n";
            }
          }
        }
}
