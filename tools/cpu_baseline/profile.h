// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Provides optional exclusive host profiling for the isolated CPU experiment.
#pragma once
#include "pbqp/pbqp.h"
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace cpu_baseline {
enum Component {
  Other,
  Topology,
  Selection,
  Reduction,
  Reconstruction,
  Kernel,
  Metadata,
  Components
};
struct Profile {
  bool enabled = false;
  bool degrees = false;
  int current = Other;
  std::chrono::steady_clock::time_point stamp;
  std::array<uint64_t, Components> ns{};
  void Charge();
};
extern thread_local Profile *profile;
// Invalidates all oriented representations of an R2-modified/reused edge slot.
void MatrixChanged(const int32_t *base);
void R1Projection(pbqp_matrix_view_t matrix, pbqp_vector_view_t unary,
                  accel_min_argmin_result_t *results);
class Scope {
 public:
  explicit Scope(int component) : active_(profile && profile->enabled) {
    if (active_) {
      profile->Charge();
      previous_ = profile->current;
      profile->current = component;
    }
  }
  ~Scope() {
    if (active_) {
      profile->Charge();
      profile->current = previous_;
    }
  }

 private:
  bool active_;
  int previous_ = Other;
};
}  // namespace cpu_baseline
