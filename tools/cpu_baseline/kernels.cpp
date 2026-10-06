// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Implements checked SIMD dense and bounded row-exception min-plus algorithms.
#include "kernels.h"
#include "cost_math_impl.h"
#include "profile.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <utility>
#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace cpu_baseline {
thread_local Profile *profile = nullptr;
thread_local Kernels *active_kernels = nullptr;
uint64_t Now() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
void Profile::Charge() {
  const auto next = std::chrono::steady_clock::now();
  ns[current] += std::chrono::duration_cast<std::chrono::nanoseconds>(next - stamp).count();
  stamp = next;
}
void MatrixChanged(const int32_t *base) {
  if (active_kernels)
    active_kernels->Invalidate(base);
}
void R1Projection(pbqp_matrix_view_t matrix, pbqp_vector_view_t unary,
                  accel_min_argmin_result_t *results) {
  if (active_kernels)
    active_kernels->SetR1(matrix, unary, results);
}
void Kernels::Reset() {
  cache_.clear();
  changed_.clear();
  classes = {};
  events.clear();
  projections.clear();
  r1_results_ = nullptr;
}
void Kernels::Invalidate(const int32_t *base) {
  if (level_ != Level::Structured)
    return;
  changed_[base] = true;
  auto entry = cache_.lower_bound(Key{base, 0, 0, 0, 0});
  while (entry != cache_.end() && std::get<0>(entry->first) == base) entry = cache_.erase(entry);
}
class Kernels::Batch {
 public:
  Batch(Kernels &owner, const char *kind, size_t count, uint64_t elements)
      : owner_(owner), kind_(kind), count_(count), elements_(elements), scope_(Kernel) {
    if (owner.record)
      start_ = Now();
  }
  ~Batch() {
    if (owner_.record) {
      const uint64_t end = Now();
      owner_.events.push_back({kind_, count_, end - start_, elements_});
    }
  }

 private:
  Kernels &owner_;
  const char *kind_;
  size_t count_;
  uint64_t elements_;
  uint64_t start_ = 0;
  Scope scope_;
};

int Kernels::Fill(pbqp_vector_view_t a, pbqp_vector_view_t b, int32_t *result) {
  if (!a.base || !b.base || !result || a.length == 0 || a.length != b.length)
    return PBQP_ARGUMENT_ERROR;
  size_t index = 0;
#if defined(__AVX2__)
  constexpr size_t kSimdCosts = 8;
  if (a.stride == 1 && b.stride == 1) {
    const auto inf = _mm256_set1_epi32(ACCEL_INF);
    const auto zero = _mm256_setzero_si256();
    for (; index + kSimdCosts <= a.length; index += kSimdCosts) {
      const auto left = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(a.base + index));
      const auto right = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(b.base + index));
      const auto sum = _mm256_add_epi32(left, right);
      const auto absorbing =
          _mm256_or_si256(_mm256_cmpeq_epi32(left, inf), _mm256_cmpeq_epi32(right, inf));
      const auto invalid =
          _mm256_or_si256(_mm256_cmpgt_epi32(left, inf), _mm256_cmpgt_epi32(right, inf));
      const auto negative =
          _mm256_and_si256(_mm256_cmpgt_epi32(zero, left), _mm256_cmpgt_epi32(zero, right));
      const auto overflow = _mm256_andnot_si256(_mm256_cmpgt_epi32(zero, sum), negative);
      if (_mm256_movemask_epi8(_mm256_or_si256(invalid, overflow)))
        return PBQP_COST_RANGE_ERROR;
      const auto value = _mm256_blendv_epi8(_mm256_min_epi32(sum, inf), inf, absorbing);
      _mm256_storeu_si256(reinterpret_cast<__m256i *>(result + index), value);
    }
  }
#endif
  for (; index < a.length; ++index) {
    if (pcaa::CostAddChecked(a.base[index * a.stride], b.base[index * b.stride], &result[index]) !=
        0)
      return PBQP_COST_RANGE_ERROR;
  }
  return 0;
}
int Kernels::Add(pbqp_vector_view_t a, pbqp_vector_view_t b, int32_t *result) {
  if (!result)
    return PBQP_ARGUMENT_ERROR;
  if (level_ == Level::Scalar)
    return reference_.cost_add_vector(nullptr, a, b, result);
  // Validate before a possibly aliased destination is modified.
  if (values_.size() < a.length)
    values_.resize(a.length);
  const int status = Fill(a, b, values_.data());
  if (status == 0)
    std::copy_n(values_.begin(), a.length, result);
  return status;
}
static void Minimum(pbqp_vector_view_t values, accel_min_argmin_result_t *result) {
  *result = {ACCEL_INF, 0};
  for (size_t index = 0; index < values.length; ++index) {
    if (values.base[index * values.stride] < result->value)
      *result = {values.base[index * values.stride], static_cast<uint32_t>(index)};
  }
}
int Kernels::Reduce(pbqp_vector_view_t a, pbqp_vector_view_t b, pbqp_vector_view_t c,
                    accel_min_argmin_result_t *result) {
  if (level_ == Level::Scalar)
    return c.base ? reference_.min3_argmin(nullptr, a, b, c, result)
                  : reference_.min2_argmin(nullptr, a, b, result);
  if (values_.size() < a.length)
    values_.resize(a.length);
  int status = Fill(a, b, values_.data());
  if (status == 0 && c.base)
    status = Fill({values_.data(), a.length, 1}, c, values_.data());
  if (status == 0)
    Minimum({values_.data(), a.length, 1}, result);
  return status;
}
const Representation &Kernels::Classify(pbqp_matrix_view_t matrix) {
  const Key key{matrix.base, matrix.rows, matrix.columns, matrix.row_stride, matrix.column_stride};
  auto cached = cache_.find(key);
  if (cached != cache_.end())
    return cached->second;
  Scope scope(Metadata);
  predicate_elements_ += matrix.rows * matrix.columns;
  Representation rep;
  rep.changed = changed_.count(matrix.base) != 0;
  rep.rows.resize(matrix.rows);
  std::vector<size_t> forbidden_columns(matrix.columns);
  bool zero_inf = true;
  bool one_per_row = true;
  bool one_per_column = true;
  bool compact = true;
  for (size_t row = 0; row < matrix.rows; ++row) {
    SparseRow &sparse = rep.rows[row];
    // The first five entries have a majority for a row with at most two exceptions.
    // For shorter rows, choose the exact mode instead; verify every entry below.
    int32_t candidate = 0;
    int votes = 0;
    for (size_t column = 0; column < std::min(matrix.columns, 2 * kExceptions + 1); ++column) {
      const int32_t value = matrix.base[row * matrix.row_stride + column * matrix.column_stride];
      if (votes == 0)
        candidate = value;
      votes += value == candidate ? 1 : -1;
    }
    if (matrix.columns <= 2 * kExceptions) {
      size_t best_count = 0;
      for (size_t c = 0; c < matrix.columns; ++c) {
        const int32_t value = matrix.base[row * matrix.row_stride + c * matrix.column_stride];
        size_t count = 0;
        for (size_t j = 0; j < matrix.columns; ++j)
          count += matrix.base[row * matrix.row_stride + j * matrix.column_stride] == value;
        if (count > best_count) {
          best_count = count;
          candidate = value;
        }
      }
    }
    sparse.background = candidate;
    size_t forbidden = 0;
    size_t forbidden_index = 0;
    for (size_t column = 0; column < matrix.columns; ++column) {
      const int32_t value = matrix.base[row * matrix.row_stride + column * matrix.column_stride];
      zero_inf &= value == 0 || value == ACCEL_INF;
      if (value == ACCEL_INF) {
        ++forbidden;
        forbidden_index = column;
        one_per_column &= ++forbidden_columns[column] <= 1;
      }
      if (value != candidate) {
        if (sparse.count < kExceptions) {
          sparse.indices[sparse.count] = column;
          sparse.values[sparse.count] = value;
        }
        ++sparse.count;
      }
    }
    one_per_row &= forbidden <= 1;
    compact &= sparse.count <= kExceptions;
    // Force zero background for forbidden rows, including n=1 and n=2.
    if (zero_inf && forbidden <= 1) {
      sparse = {};
      if (forbidden) {
        sparse.indices[0] = forbidden_index;
        sparse.values[0] = ACCEL_INF;
        sparse.count = 1;
      }
    }
  }
  rep.kind = zero_inf && one_per_row ? (one_per_column ? Matching : ForbiddenRows)
                                     : (compact ? RowExceptions : DenseMatrix);
  return cache_.emplace(key, std::move(rep)).first->second;
}
int Kernels::Sparse(const Representation &rep, pbqp_vector_view_t vector, int32_t *result,
                    accel_min_argmin_result_t *argmin) {
  sparse_additions_ = 0;
  constexpr size_t kMinima = kExceptions + 1;
  const size_t minima_count = rep.kind == RowExceptions ? kMinima : kExceptions;
  using Choice = std::pair<int32_t, size_t>;
  std::array<Choice, kMinima> minima;
  minima.fill({ACCEL_INF, std::numeric_limits<size_t>::max()});
  for (size_t index = 0; index < vector.length; ++index) {
    const int32_t value = vector.base[index * vector.stride];
    if (value > ACCEL_INF)
      return PBQP_COST_RANGE_ERROR;
    Choice candidate{value, index};
    for (size_t slot = 0; slot < minima_count; ++slot) {
      Choice &best = minima[slot];
      if (candidate < best)
        std::swap(candidate, best);
    }
  }
  for (size_t row = 0; row < rep.rows.size(); ++row) {
    const SparseRow &sparse = rep.rows[row];
    if (sparse.background > ACCEL_INF)
      return PBQP_COST_RANGE_ERROR;
    Choice best{ACCEL_INF, 0};
    for (const Choice &minimum : minima) {
      bool excluded = false;
      for (size_t e = 0; e < sparse.count; ++e) excluded |= minimum.second == sparse.indices[e];
      if (excluded || minimum.second >= vector.length)
        continue;
      int32_t value = 0;
      if (sparse.background == 0)
        value = minimum.first;
      else {
        if (class_record)
          ++sparse_additions_;
        if (pcaa::CostAddChecked(minimum.first, sparse.background, &value) != 0)
          return PBQP_COST_RANGE_ERROR;
      }
      best = std::min(best, Choice{value, minimum.second});
      break;
    }
    for (size_t e = 0; e < sparse.count; ++e) {
      if (sparse.values[e] == ACCEL_INF)
        continue;
      int32_t value = 0;
      if (class_record)
        ++sparse_additions_;
      if (pcaa::CostAddChecked(vector.base[sparse.indices[e] * vector.stride], sparse.values[e],
                               &value) != 0)
        return PBQP_COST_RANGE_ERROR;
      best = std::min(best, Choice{value, sparse.indices[e]});
    }
    if (result)
      result[row] = best.first;
    if (argmin)
      argmin[row] = {best.first, static_cast<uint32_t>(best.second)};
  }
  return 0;
}
int Kernels::Dense(pbqp_matrix_view_t matrix, pbqp_vector_view_t vector, int32_t *result,
                   accel_min_argmin_result_t *argmin) {
  if (values_.size() < matrix.columns)
    values_.resize(matrix.columns);
  for (size_t row = 0; row < matrix.rows; ++row) {
    const int status =
        Fill({matrix.base + row * matrix.row_stride, matrix.columns, matrix.column_stride}, vector,
             values_.data());
    if (status != 0)
      return status;
    accel_min_argmin_result_t minimum;
    Minimum({values_.data(), matrix.columns, 1}, &minimum);
    if (result)
      result[row] = minimum.value;
    if (argmin)
      argmin[row] = minimum;
  }
  return 0;
}
int Kernels::Project(pbqp_matrix_view_t matrix, pbqp_vector_view_t vector, int32_t *result,
                     accel_min_argmin_result_t *argmin) {
  if (!matrix.base || !vector.base || matrix.rows == 0 || matrix.columns == 0 ||
      matrix.columns != vector.length || (!result && !argmin))
    return PBQP_ARGUMENT_ERROR;
  uint64_t start = class_record ? Now() : 0;
  predicate_elements_ = 0;
  int kind = DenseMatrix;
  bool changed = false;
  const Representation *rep = nullptr;
  if (level_ == Level::Structured) {
    rep = &Classify(matrix);
    kind = rep->kind;
    changed = rep->changed;
  }
  int status = 0;
  if (level_ == Level::Scalar && !argmin)
    status = reference_.minplus_project(nullptr, matrix, vector, result);
  else if (level_ == Level::Scalar) {
    for (size_t row = 0; row < matrix.rows && status == 0; ++row)
      status = reference_.min2_argmin(
          nullptr, vector,
          {matrix.base + row * matrix.row_stride, matrix.columns, matrix.column_stride},
          argmin + row);
  } else if (rep && kind != DenseMatrix)
    status = Sparse(*rep, vector, result, argmin);
  else
    status = Dense(matrix, vector, result, argmin);
  if (class_record) {
    const uint64_t elapsed = Now() - start;
    auto &stats = classes[kind];
    ++stats.calls;
    stats.elements += matrix.rows * matrix.columns;
    stats.bytes += (matrix.rows * matrix.columns + matrix.columns + matrix.rows) * sizeof(int32_t);
    stats.ns += elapsed;
    stats.changed_calls += changed;
    stats.changed_elements += changed ? matrix.rows * matrix.columns : 0;
    stats.changed_ns += changed ? elapsed : 0;
    stats.changed_bytes +=
        changed ? (matrix.rows * matrix.columns + matrix.columns + matrix.rows) * sizeof(int32_t)
                : 0;
    stats.predicate_elements += predicate_elements_;
    last_kind_ = kind;
    projections.push_back(
        {matrix.rows, matrix.columns, matrix.column_stride, kind, changed, add3_, elapsed});
    stats.evaluated += kind == DenseMatrix ? matrix.rows * matrix.columns : sparse_additions_;
  }
  return status;
}
int Kernels::Map3(pbqp_vector_view_t unary, pbqp_vector_view_t fixed, pbqp_matrix_view_t matrix,
                  accel_min_argmin_result_t *result) {
  if (level_ == Level::Scalar)
    return reference_.minplus_map3_project(nullptr, unary, fixed, matrix, result);
  if (unary.length != matrix.columns)
    return PBQP_ARGUMENT_ERROR;
  if (preadd_.size() < unary.length)
    preadd_.resize(unary.length);
  const int status = Fill(unary, fixed, preadd_.data());
  if (status != 0)
    return status;
  add3_ = true;
  const int projected = Project(matrix, {preadd_.data(), unary.length, 1}, nullptr, result);
  add3_ = false;
  if (class_record)
    classes[last_kind_].preadd_elements += unary.length;
  return projected;
}
int Kernels::Min2(void *p, pbqp_vector_view_t a, pbqp_vector_view_t b,
                  accel_min_argmin_result_t *r) {
  auto &k = *static_cast<Kernels *>(p);
  Batch batch(k, "min2", 1, a.length);
  return k.Reduce(a, b, {}, r);
}
int Kernels::Min3(void *p, pbqp_vector_view_t a, pbqp_vector_view_t b, pbqp_vector_view_t c,
                  accel_min_argmin_result_t *r) {
  auto &k = *static_cast<Kernels *>(p);
  Batch batch(k, "min3", 1, a.length);
  return k.Reduce(a, b, c, r);
}
int Kernels::Min2Batch(void *p, const pbqp_min2_job_t *jobs, size_t count) {
  auto &k = *static_cast<Kernels *>(p);
  uint64_t elements = 0;
  for (size_t i = 0; i < count; ++i) elements += jobs[i].a.length;
  Batch batch(k, "min2_batch", count, elements);
  // The solver supplies this explicit view only around an R1 callback. Never
  // infer a native matrix allocation from differences between unrelated pointers.
  if (count && k.level_ == Level::Structured && k.r1_results_ && count == k.r1_matrix_.rows) {
    bool matrix_jobs = true;
    for (size_t i = 0; i < count; ++i)
      matrix_jobs &= jobs[i].a.base == k.r1_unary_.base && jobs[i].a.length == k.r1_unary_.length &&
                     jobs[i].a.stride == k.r1_unary_.stride &&
                     jobs[i].b.length == k.r1_matrix_.columns &&
                     jobs[i].b.stride == k.r1_matrix_.column_stride &&
                     jobs[i].b.base == k.r1_matrix_.base + i * k.r1_matrix_.row_stride &&
                     jobs[i].result == k.r1_results_ + i;
    if (matrix_jobs)
      return k.Project(k.r1_matrix_, k.r1_unary_, nullptr, k.r1_results_);
  }
  for (size_t i = 0; i < count; ++i) {
    const int status = k.Reduce(jobs[i].a, jobs[i].b, {}, jobs[i].result);
    if (status)
      return status;
  }
  return 0;
}
int Kernels::Min3Batch(void *p, const pbqp_min3_job_t *jobs, size_t count) {
  auto &k = *static_cast<Kernels *>(p);
  Batch batch(k, "min3_batch", count, 0);
  for (size_t i = 0; i < count; ++i) {
    const int status = k.Reduce(jobs[i].a, jobs[i].b, jobs[i].c, jobs[i].result);
    if (status)
      return status;
  }
  return 0;
}
int Kernels::Min2Value(void *p, pbqp_vector_view_t a, pbqp_vector_view_t b, int32_t *r) {
  accel_min_argmin_result_t result{};
  const int status = Min2(p, a, b, &result);
  if (!status)
    *r = result.value;
  return status;
}
int Kernels::Min2ValueBatch(void *p, const pbqp_min2_value_job_t *jobs, size_t count) {
  auto &k = *static_cast<Kernels *>(p);
  Batch batch(k, "min2_value_batch", count, 0);
  for (size_t i = 0; i < count; ++i) {
    accel_min_argmin_result_t r{};
    const int s = k.Reduce(jobs[i].a, jobs[i].b, {}, &r);
    if (s)
      return s;
    *jobs[i].result = r.value;
  }
  return 0;
}
int Kernels::AddVector(void *p, pbqp_vector_view_t a, pbqp_vector_view_t b, int32_t *r) {
  auto &k = *static_cast<Kernels *>(p);
  Batch batch(k, "add", 1, a.length);
  return k.Add(a, b, r);
}
int Kernels::ProjectCall(void *p, pbqp_matrix_view_t m, pbqp_vector_view_t x, int32_t *r) {
  auto &k = *static_cast<Kernels *>(p);
  Batch batch(k, "project", 1, m.rows * m.columns);
  return k.Project(m, x, r);
}
int Kernels::Map3Call(void *p, pbqp_vector_view_t a, pbqp_vector_view_t b, pbqp_matrix_view_t m,
                      accel_min_argmin_result_t *r) {
  auto &k = *static_cast<Kernels *>(p);
  Batch batch(k, "map3", 1, m.rows * m.columns);
  return k.Map3(a, b, m, r);
}
int Kernels::ProjectBatch(void *p, const pbqp_project_add_job_t *jobs, size_t count) {
  auto &k = *static_cast<Kernels *>(p);
  uint64_t elements = 0;
  for (size_t i = 0; i < count; ++i)
    elements += jobs[i].matrix.rows * jobs[i].matrix.columns + jobs[i].matrix.rows;
  Batch batch(k, "project_add_batch", count, elements);
  for (size_t i = 0; i < count; ++i) {
    const auto &job = jobs[i];
    int status = k.Project(job.matrix, job.unary, job.temporary);
    if (!status)
      status =
          k.Add({job.temporary, job.matrix.rows, 1}, {job.scores, job.matrix.rows, 1}, job.scores);
    if (status)
      return status;
  }
  return 0;
}
int Kernels::Map3Batch(void *p, const pbqp_map3_project_job_t *jobs, size_t count) {
  auto &k = *static_cast<Kernels *>(p);
  uint64_t elements = 0;
  for (size_t i = 0; i < count; ++i)
    elements += jobs[i].varying_edge.rows * jobs[i].varying_edge.columns;
  Batch batch(k, "map3_project_batch", count, elements);
  for (size_t i = 0; i < count; ++i) {
    const auto &j = jobs[i];
    const int status = k.Map3(j.unary, j.fixed_edge, j.varying_edge, j.results);
    if (status)
      return status;
  }
  return 0;
}
void Kernels::Make(pbqp_cost_kernel_t &kernel) {
  kernel = {};
  kernel.context = this;
  kernel.min2_argmin = Min2;
  kernel.min3_argmin = Min3;
  kernel.min2_argmin_batch = Min2Batch;
  kernel.min3_argmin_batch = Min3Batch;
  kernel.min2_value = Min2Value;
  kernel.min2_value_batch = Min2ValueBatch;
  kernel.cost_add_vector = AddVector;
  kernel.minplus_project = ProjectCall;
  kernel.minplus_map3_project = Map3Call;
  kernel.project_add_batch = ProjectBatch;
  kernel.map3_project_batch = Map3Batch;
}
}  // namespace cpu_baseline
