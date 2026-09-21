// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Implements PBQP heap/arena allocation and graph-state lifetime operations.

#include "pbqp_storage.h"
#include "accel_protocol.h"
#include "pbqp.h"

#include <stdint.h>

#if defined(__riscv)
extern "C" void *memcpy(void *destination, const void *source, size_t size);
extern "C" void *memset(void *destination, int value, size_t size);
#else
#include <stdlib.h>
#include <string.h>
#endif

namespace pcaa::pbqp_storage {
namespace {

constexpr size_t kStorageAlignment = sizeof(uint64_t);

bool AddStorage(size_t *total, size_t count, size_t element_size) {
  if (count != 0 && element_size > SIZE_MAX / count)
    return false;
  const size_t bytes = count * element_size;
  if (bytes > SIZE_MAX - (kStorageAlignment - 1))
    return false;
  const size_t aligned = (bytes + kStorageAlignment - 1) & ~(kStorageAlignment - 1);
  if (*total > SIZE_MAX - aligned)
    return false;
  *total += aligned;
  return true;
}

bool Multiply(size_t first, size_t second, size_t *result) {
  if (first != 0 && second > SIZE_MAX / first)
    return false;
  *result = first * second;
  return true;
}

size_t ProblemStorageSize(unsigned node_capacity, unsigned edge_capacity,
                          unsigned domain_capacity) {
  if (domain_capacity != 0 && domain_capacity > SIZE_MAX / domain_capacity)
    return 0;
  const size_t domain_square = static_cast<size_t>(domain_capacity) * domain_capacity;
  size_t statistics_count = 0;
  size_t unary_count = 0;
  size_t edge_cost_count = 0;
  size_t reconstruction_count = 0;
  if (!Multiply(node_capacity, 7, &statistics_count) ||
      statistics_count > SIZE_MAX - domain_capacity ||
      statistics_count + domain_capacity > SIZE_MAX - 3 ||
      !Multiply(node_capacity, domain_capacity, &unary_count) ||
      !Multiply(edge_capacity, domain_square, &edge_cost_count) ||
      !Multiply(node_capacity, domain_square, &reconstruction_count))
    return 0;
  statistics_count += domain_capacity + 3;
  size_t size = 0;
  if (!AddStorage(&size, node_capacity, sizeof(pbqp_node_t)) ||
      !AddStorage(&size, edge_capacity, sizeof(pbqp_edge_t)) ||
      !AddStorage(&size, unary_count, sizeof(int32_t)) ||
      !AddStorage(&size, edge_cost_count, sizeof(int32_t)) ||
      !AddStorage(&size, node_capacity, sizeof(unsigned)) ||
      !AddStorage(&size, reconstruction_count, sizeof(unsigned)) ||
      !AddStorage(&size, statistics_count, sizeof(unsigned)))
    return 0;
  return size;
}

void BindProblemStorage(pbqp_problem_t *problem) {
  unsigned char *cursor = static_cast<unsigned char *>(problem->storage);
  problem->nodes = reinterpret_cast<pbqp_node_t *>(cursor);
  size_t section = 0;
  AddStorage(&section, problem->node_capacity, sizeof(pbqp_node_t));
  cursor += section;
  problem->edges = reinterpret_cast<pbqp_edge_t *>(cursor);
  section = 0;
  AddStorage(&section, problem->edge_capacity, sizeof(pbqp_edge_t));
  cursor += section;
  int32_t *unary = reinterpret_cast<int32_t *>(cursor);
  section = 0;
  AddStorage(&section, static_cast<size_t>(problem->node_capacity) * problem->domain_capacity,
             sizeof(int32_t));
  cursor += section;
  int32_t *cost = reinterpret_cast<int32_t *>(cursor);
  const size_t domain_square =
      static_cast<size_t>(problem->domain_capacity) * problem->domain_capacity;
  section = 0;
  AddStorage(&section, static_cast<size_t>(problem->edge_capacity) * domain_square,
             sizeof(int32_t));
  cursor += section;
  problem->elimination_order = reinterpret_cast<unsigned *>(cursor);
  section = 0;
  AddStorage(&section, problem->node_capacity, sizeof(unsigned));
  cursor += section;
  problem->reconstruction = reinterpret_cast<unsigned *>(cursor);
  section = 0;
  AddStorage(&section, static_cast<size_t>(problem->node_capacity) * domain_square,
             sizeof(unsigned));
  cursor += section;
  for (unsigned node = 0; node < problem->node_capacity; ++node)
    problem->nodes[node].unary = unary + static_cast<size_t>(node) * problem->domain_capacity;
  for (unsigned edge = 0; edge < problem->edge_capacity; ++edge) {
    problem->edges[edge].stride = problem->domain_capacity;
    problem->edges[edge].cost = cost + static_cast<size_t>(edge) * domain_square;
  }
  unsigned *statistics = reinterpret_cast<unsigned *>(cursor);
  problem->statistics.rn_degree_histogram = statistics;
  statistics += problem->node_capacity + 1;
  problem->statistics.rn_nodes = statistics;
  statistics += problem->node_capacity;
  problem->statistics.rn_choices = statistics;
  statistics += problem->node_capacity;
  problem->statistics.rn_cascade_r0 = statistics;
  statistics += problem->node_capacity;
  problem->statistics.rn_cascade_r1 = statistics;
  statistics += problem->node_capacity;
  problem->statistics.rn_cascade_r2 = statistics;
  statistics += problem->node_capacity;
  problem->statistics.rn_cascade_length_histogram = statistics;
  statistics += problem->node_capacity + 1;
  problem->statistics.vector_length_histogram = statistics;
}

void *ArenaAllocate(void *context, size_t size) {
  pbqp_arena_t *arena = static_cast<pbqp_arena_t *>(context);
  if (arena == nullptr || arena->storage == nullptr)
    return nullptr;
  if (arena->used > SIZE_MAX - sizeof(size_t) - (kStorageAlignment - 1))
    return nullptr;
  const uintptr_t base = reinterpret_cast<uintptr_t>(arena->storage);
  const uintptr_t current = base + arena->used + sizeof(size_t);
  if (current < base)
    return nullptr;
  const uintptr_t aligned =
      (current + kStorageAlignment - 1) & ~(static_cast<uintptr_t>(kStorageAlignment) - 1);
  if (aligned < current)
    return nullptr;
  const size_t allocation_offset = aligned - base;
  if (allocation_offset > arena->capacity || size > arena->capacity - allocation_offset)
    return nullptr;
  const size_t previous_used = arena->used;
  memcpy(arena->storage + allocation_offset - sizeof(size_t), &previous_used, sizeof(size_t));
  arena->used = allocation_offset + size;
  return arena->storage + allocation_offset;
}

void ArenaDeallocate(void *context, void *allocation, size_t size) {
  pbqp_arena_t *arena = static_cast<pbqp_arena_t *>(context);
  if (arena == nullptr || arena->storage == nullptr || allocation == nullptr)
    return;
  unsigned char *bytes = static_cast<unsigned char *>(allocation);
  if (bytes + size != arena->storage + arena->used)
    return;
  size_t previous_used = 0;
  memcpy(&previous_used, bytes - sizeof(size_t), sizeof(size_t));
  arena->used = previous_used;
}

#if !defined(__riscv)
void *HeapAllocate(void *, size_t size) {
  return malloc(size);
}

void HeapDeallocate(void *, void *allocation, size_t) {
  free(allocation);
}
#endif

}  // namespace

unsigned *Reconstruction(pbqp_problem_t &problem, unsigned node) {
  return problem.reconstruction +
         static_cast<size_t>(node) * problem.domain_capacity * problem.domain_capacity;
}

const unsigned *Reconstruction(const pbqp_problem_t &problem, unsigned node) {
  return problem.reconstruction +
         static_cast<size_t>(node) * problem.domain_capacity * problem.domain_capacity;
}

void CopyStatistics(pbqp_statistics_t *destination, const pbqp_statistics_t &source,
                    unsigned node_capacity, unsigned domain_capacity) {
  unsigned *rn_degree_histogram = destination->rn_degree_histogram;
  unsigned *rn_nodes = destination->rn_nodes;
  unsigned *rn_choices = destination->rn_choices;
  unsigned *rn_cascade_r0 = destination->rn_cascade_r0;
  unsigned *rn_cascade_r1 = destination->rn_cascade_r1;
  unsigned *rn_cascade_r2 = destination->rn_cascade_r2;
  unsigned *rn_cascade_length_histogram = destination->rn_cascade_length_histogram;
  unsigned *vector_length_histogram = destination->vector_length_histogram;
  *destination = source;
  destination->rn_degree_histogram = rn_degree_histogram;
  destination->rn_nodes = rn_nodes;
  destination->rn_choices = rn_choices;
  destination->rn_cascade_r0 = rn_cascade_r0;
  destination->rn_cascade_r1 = rn_cascade_r1;
  destination->rn_cascade_r2 = rn_cascade_r2;
  destination->rn_cascade_length_histogram = rn_cascade_length_histogram;
  destination->vector_length_histogram = vector_length_histogram;
  memcpy(rn_degree_histogram, source.rn_degree_histogram,
         (static_cast<size_t>(node_capacity) + 1) * sizeof(unsigned));
  memcpy(rn_nodes, source.rn_nodes, static_cast<size_t>(node_capacity) * sizeof(unsigned));
  memcpy(rn_choices, source.rn_choices, static_cast<size_t>(node_capacity) * sizeof(unsigned));
  memcpy(rn_cascade_r0, source.rn_cascade_r0,
         static_cast<size_t>(node_capacity) * sizeof(unsigned));
  memcpy(rn_cascade_r1, source.rn_cascade_r1,
         static_cast<size_t>(node_capacity) * sizeof(unsigned));
  memcpy(rn_cascade_r2, source.rn_cascade_r2,
         static_cast<size_t>(node_capacity) * sizeof(unsigned));
  memcpy(rn_cascade_length_histogram, source.rn_cascade_length_histogram,
         (static_cast<size_t>(node_capacity) + 1) * sizeof(unsigned));
  memcpy(vector_length_histogram, source.vector_length_histogram,
         (static_cast<size_t>(domain_capacity) + 1) * sizeof(unsigned));
}

}  // namespace pcaa::pbqp_storage

extern "C" {

pbqp_allocator_t pbqp_heap_allocator(void) {
#if defined(__riscv)
  return {};
#else
  return {nullptr, pcaa::pbqp_storage::HeapAllocate, pcaa::pbqp_storage::HeapDeallocate};
#endif
}

void pbqp_arena_init(pbqp_arena_t *arena, void *storage, size_t size) {
  if (arena == nullptr)
    return;
  arena->storage = static_cast<unsigned char *>(storage);
  arena->capacity = size;
  arena->used = 0;
}

pbqp_allocator_t pbqp_arena_allocator(pbqp_arena_t *arena) {
  if (arena == nullptr)
    return {};
  return {arena, pcaa::pbqp_storage::ArenaAllocate, pcaa::pbqp_storage::ArenaDeallocate};
}

size_t pbqp_problem_storage_size(unsigned node_capacity, unsigned edge_capacity,
                                 unsigned domain_capacity) {
  return pcaa::pbqp_storage::ProblemStorageSize(node_capacity, edge_capacity, domain_capacity);
}

pbqp_status_t pbqp_init(pbqp_problem_t *problem, pbqp_allocator_t allocator, unsigned node_capacity,
                        unsigned edge_capacity, unsigned domain_capacity) {
  if (problem == nullptr || allocator.allocate == nullptr || allocator.deallocate == nullptr ||
      node_capacity == 0 || domain_capacity == 0)
    return PBQP_ARGUMENT_ERROR;
  *problem = {};
  problem->allocator = allocator;
  problem->node_capacity = node_capacity;
  problem->edge_capacity = edge_capacity;
  problem->domain_capacity = domain_capacity;
  problem->storage_size =
      pcaa::pbqp_storage::ProblemStorageSize(node_capacity, edge_capacity, domain_capacity);
  if (problem->storage_size == 0) {
    *problem = {};
    return PBQP_CAPACITY_ERROR;
  }
  problem->storage = allocator.allocate(allocator.context, problem->storage_size);
  if (problem->storage == nullptr) {
    *problem = {};
    return PBQP_CAPACITY_ERROR;
  }
  memset(problem->storage, 0, problem->storage_size);
  pcaa::pbqp_storage::BindProblemStorage(problem);
  return PBQP_OK;
}

void pbqp_destroy(pbqp_problem_t *problem) {
  if (problem == nullptr)
    return;
  const pbqp_allocator_t allocator = problem->allocator;
  void *storage = problem->storage;
  const size_t storage_size = problem->storage_size;
  *problem = {};
  if (storage != nullptr && allocator.deallocate != nullptr)
    allocator.deallocate(allocator.context, storage, storage_size);
}

pbqp_status_t pbqp_problem_clone(pbqp_problem_t *destination, const pbqp_problem_t *source,
                                 pbqp_allocator_t allocator) {
  if (destination == nullptr || source == nullptr || source->storage == nullptr)
    return PBQP_ARGUMENT_ERROR;
  const pbqp_status_t status = pbqp_init(destination, allocator, source->node_capacity,
                                         source->edge_capacity, source->domain_capacity);
  if (status != PBQP_OK)
    return status;
  memcpy(destination->storage, source->storage, source->storage_size);
  pcaa::pbqp_storage::BindProblemStorage(destination);
  destination->node_count = source->node_count;
  destination->edge_count = source->edge_count;
  destination->objective_offset = source->objective_offset;
  destination->elimination_count = source->elimination_count;
  pcaa::pbqp_storage::CopyStatistics(&destination->statistics, source->statistics,
                                     source->node_capacity, source->domain_capacity);
  return PBQP_OK;
}

void pbqp_solution_init(pbqp_solution_t *solution, unsigned *assignment,
                        size_t assignment_capacity) {
  if (solution == nullptr)
    return;
  solution->optimum = ACCEL_INF;
  solution->assignment = assignment;
  solution->assignment_capacity = assignment_capacity;
}

}  // extern "C"
