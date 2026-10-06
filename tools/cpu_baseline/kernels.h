// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Defines three native cost-kernel levels and dynamic structure evidence.
#pragma once
#include "pbqp/pbqp.h"
#include "accel_protocol.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <tuple>
#include <vector>

namespace cpu_baseline {
enum class Level { Scalar, Dense, Structured };
enum MatrixClass { Matching, ForbiddenRows, RowExceptions, DenseMatrix, MatrixClasses };
constexpr size_t kExceptions = 2;
struct SparseRow {
  int32_t background = 0;
  size_t count = 0;
  std::array<size_t, kExceptions> indices{};
  std::array<int32_t, kExceptions> values{};
};
struct Representation {
  int kind = DenseMatrix;
  bool changed = false;
  std::vector<SparseRow> rows;
};
struct ClassStatistics {
  uint64_t calls = 0;
  uint64_t elements = 0;
  uint64_t bytes = 0;
  uint64_t ns = 0;
  uint64_t evaluated = 0;
  uint64_t changed_calls = 0;
  uint64_t changed_elements = 0;
  uint64_t changed_ns = 0;
  uint64_t changed_bytes = 0;
  uint64_t preadd_elements = 0;
  uint64_t predicate_elements = 0;
};
struct Event {
  const char *kind;
  size_t count;
  uint64_t ns;
  uint64_t elements;
};
struct Projection {
  size_t rows;
  size_t columns;
  size_t column_stride;
  int kind;
  bool changed;
  bool add3;
  uint64_t ns;
};
class Kernels {
 public:
  explicit Kernels(Level level) : level_(level) {
    pbqp_make_software_kernel(&reference_, nullptr);
  }
  void Make(pbqp_cost_kernel_t &kernel);
  void Invalidate(const int32_t *base);
  void Reset();
  void SetR1(pbqp_matrix_view_t matrix, pbqp_vector_view_t unary,
             accel_min_argmin_result_t *results) {
    r1_matrix_ = matrix;
    r1_unary_ = unary;
    r1_results_ = results;
  }
  int Project(pbqp_matrix_view_t matrix, pbqp_vector_view_t vector, int32_t *result,
              accel_min_argmin_result_t *argmin = nullptr);
  int Map3(pbqp_vector_view_t unary, pbqp_vector_view_t fixed, pbqp_matrix_view_t matrix,
           accel_min_argmin_result_t *result);
  const Representation &Classify(pbqp_matrix_view_t matrix);
  bool record = false;
  bool class_record = false;
  std::array<ClassStatistics, MatrixClasses> classes{};
  std::vector<Event> events;
  std::vector<Projection> projections;

 private:
  using Key = std::tuple<const int32_t *, size_t, size_t, size_t, size_t>;
  struct KeyLess {
    bool operator()(const Key &left, const Key &right) const {
      if (std::get<0>(left) != std::get<0>(right))
        return std::less<const int32_t *>{}(std::get<0>(left), std::get<0>(right));
      return std::tie(std::get<1>(left), std::get<2>(left), std::get<3>(left), std::get<4>(left)) <
             std::tie(std::get<1>(right), std::get<2>(right), std::get<3>(right),
                      std::get<4>(right));
    }
  };
  class Batch;
  int Add(pbqp_vector_view_t a, pbqp_vector_view_t b, int32_t *result);
  int Reduce(pbqp_vector_view_t a, pbqp_vector_view_t b, pbqp_vector_view_t c,
             accel_min_argmin_result_t *result);
  int Fill(pbqp_vector_view_t a, pbqp_vector_view_t b, int32_t *result);
  int Dense(pbqp_matrix_view_t matrix, pbqp_vector_view_t vector, int32_t *result,
            accel_min_argmin_result_t *argmin);
  int Sparse(const Representation &rep, pbqp_vector_view_t vector, int32_t *result,
             accel_min_argmin_result_t *argmin);
  static int Min2(void *, pbqp_vector_view_t, pbqp_vector_view_t, accel_min_argmin_result_t *);
  static int Min3(void *, pbqp_vector_view_t, pbqp_vector_view_t, pbqp_vector_view_t,
                  accel_min_argmin_result_t *);
  static int Min2Batch(void *, const pbqp_min2_job_t *, size_t);
  static int Min3Batch(void *, const pbqp_min3_job_t *, size_t);
  static int Min2Value(void *, pbqp_vector_view_t, pbqp_vector_view_t, int32_t *);
  static int Min2ValueBatch(void *, const pbqp_min2_value_job_t *, size_t);
  static int AddVector(void *, pbqp_vector_view_t, pbqp_vector_view_t, int32_t *);
  static int ProjectCall(void *, pbqp_matrix_view_t, pbqp_vector_view_t, int32_t *);
  static int Map3Call(void *, pbqp_vector_view_t, pbqp_vector_view_t, pbqp_matrix_view_t,
                      accel_min_argmin_result_t *);
  static int ProjectBatch(void *, const pbqp_project_add_job_t *, size_t);
  static int Map3Batch(void *, const pbqp_map3_project_job_t *, size_t);
  uint64_t sparse_additions_ = 0;
  uint64_t predicate_elements_ = 0;
  int last_kind_ = DenseMatrix;
  bool add3_ = false;
  pbqp_matrix_view_t r1_matrix_{};
  pbqp_vector_view_t r1_unary_{};
  accel_min_argmin_result_t *r1_results_ = nullptr;
  Level level_;
  pbqp_cost_kernel_t reference_{};
  std::map<Key, Representation, KeyLess> cache_;
  std::map<const int32_t *, bool> changed_;
  std::vector<int32_t> values_;
  std::vector<int32_t> preadd_;
};
extern thread_local Kernels *active_kernels;
uint64_t Now();
}  // namespace cpu_baseline
