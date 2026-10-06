// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Provides typed copying where the RV64 toolchain has no standard C++ library.
#pragma once
#include <stddef.h>
namespace pcaa::pbqp {
template <typename Input, typename Output>
Output copy_n(Input first, size_t count, Output result) {
  for (size_t index = 0; index < count; ++index) *result++ = *first++;
  return result;
}
}  // namespace pcaa::pbqp
