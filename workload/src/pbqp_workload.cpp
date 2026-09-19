// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 PCAA contributors
// Emits deterministic PBQP workload traces and aggregate interface statistics.

#include "pbqp_workload/analyzer.h"
#include "pbqp_workload/graph_generator.h"

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

using pcaa::workload::DomainProfile;
using pcaa::workload::GeneratorConfig;
using pcaa::workload::GraphFamily;

struct CorpusEntry {
  GraphFamily family;
  DomainProfile profile;
  int nodes;
  unsigned seed;
};

std::vector<CorpusEntry> RepresentativeCorpus() {
  return {
      {GraphFamily::kTree, DomainProfile::kUniformSmall, 10, 101},
      {GraphFamily::kTree, DomainProfile::kRegisterLike, 100, 102},
      {GraphFamily::kTree, DomainProfile::kLargeStress, 1000, 103},
      {GraphFamily::kPath, DomainProfile::kRegisterLike, 100, 201},
      {GraphFamily::kStar, DomainProfile::kRegisterLike, 100, 202},
      {GraphFamily::kCycle, DomainProfile::kUniformSmall, 10, 301},
      {GraphFamily::kCycle, DomainProfile::kRegisterLike, 100, 302},
      {GraphFamily::kTwoTree, DomainProfile::kUniformSmall, 100, 401},
      {GraphFamily::kTwoTree, DomainProfile::kRegisterLike, 250, 402},
      {GraphFamily::kTwoTree, DomainProfile::kLargeStress, 100, 403},
      {GraphFamily::kIrreducibleCore, DomainProfile::kRegisterLike, 8, 501},
  };
}

}  // namespace

int main(int argc, char **argv) {
  std::string trace_path = "pbqp-workload.csv";
  if (argc == 3 && std::string(argv[1]) == "--trace") {
    trace_path = argv[2];
  } else if (argc != 1) {
    std::cerr << "usage: pbqp_workload [--trace PATH]\n";
    return 2;
  }

  std::ofstream trace(trace_path);
  if (!trace) {
    std::cerr << "cannot write " << trace_path << '\n';
    return 1;
  }
  trace << pcaa::workload::CsvHeader();

  pcaa::workload::Aggregate aggregate;
  for (const CorpusEntry &entry : RepresentativeCorpus()) {
    const GeneratorConfig config = {entry.family, entry.profile, entry.nodes, entry.seed};
    const auto result = pcaa::workload::Analyze(config);
    for (const auto &record : result.trace) {
      trace << pcaa::workload::ToCsv(config, record);
    }
    pcaa::workload::AddToAggregate(result, &aggregate);
    pcaa::workload::Aggregate instance_aggregate;
    pcaa::workload::AddToAggregate(result, &instance_aggregate);
    std::cout << "family=" << pcaa::workload::ToString(entry.family)
              << " profile=" << pcaa::workload::ToString(entry.profile) << " seed=" << entry.seed
              << '\n'
              << pcaa::workload::FormatAggregate(instance_aggregate);
  }
  std::cout << "trace=" << trace_path << '\n' << pcaa::workload::FormatAggregate(aggregate);
  return 0;
}
