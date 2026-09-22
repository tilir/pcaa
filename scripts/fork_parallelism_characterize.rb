#!/usr/bin/env ruby
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 PCAA contributors
# Crosses exact-search fork domains with heuristic Model-C epoch work.

require "csv"
require "json"
require "open3"
require "optparse"
require "tempfile"

SYNTHETIC_POINTS = [
  ["degree-3", "binary", 20, 2], ["degree-3", "binary", 20, 4],
  ["degree-3", "binary", 30, 2], ["degree-3", "binary", 30, 4],
  ["degree-3", "binary", 50, 2],
  ["degree-4", "binary", 20, 2], ["degree-4", "binary", 20, 4],
  ["degree-4", "binary", 50, 2], ["degree-4", "binary", 50, 4],
  ["degree-4", "binary", 100, 2],
  ["mixed-degree", "small", 15, 2], ["mixed-degree", "small", 20, 2],
  ["mixed-degree", "small", 25, 2]
].freeze
ELEMENT_FIELDS = %w[project project_accumulate slice map3 argmin].freeze
STRUCTURAL_TYPES = %w[R0 R1 R2 RN_SELECT RN_SCORE RN_COMMIT].freeze

options = { runner: "build/pcaa_graph_run", generator: "build/pbqp_graph_generate",
            scaling_csv: "build/scaling-runs.csv", corpus_dir: "examples/regalloc",
            output: "build/fork-parallelism.csv" }
OptionParser.new do |parser|
  parser.on("--runner PATH") { |value| options[:runner] = value }
  parser.on("--generator PATH") { |value| options[:generator] = value }
  parser.on("--scaling-csv PATH") { |value| options[:scaling_csv] = value }
  parser.on("--corpus-dir PATH") { |value| options[:corpus_dir] = value }
  parser.on("--output PATH") { |value| options[:output] = value }
end.parse!
abort "unexpected argument: #{ARGV.first}" unless ARGV.empty?

def invoke(*command)
  Open3.capture3(*command)
end

def generate_graph(generator, family, profile, nodes, domain, seed)
  output, diagnostics, status = invoke(generator, "--family", family, "--profile", profile,
                                        "--nodes", nodes.to_s, "--domain-size", domain.to_s,
                                        "--seed", seed.to_s)
  abort "graph generation failed: #{diagnostics}" unless status.success?
  output
end

def model_c_epoch_elements(events)
  groups = [[]]
  events.each do |event|
    groups << [] if event.fetch("type") == "RN_SELECT" && !groups.last.empty?
    has_work = ELEMENT_FIELDS.any? { |field| event.fetch(field, 0).positive? }
    groups.last << event if has_work || STRUCTURAL_TYPES.include?(event.fetch("type"))
  end
  groups.reject(&:empty?).map do |group|
    group.sum { |event| ELEMENT_FIELDS.sum { |field| event.fetch(field, 0) } }
  end
end

def median(values)
  return 0 if values.empty?
  sorted = values.sort
  middle = sorted.length / 2
  sorted.length.odd? ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) / 2.0
end

def trace_run(runner, graph_path, strategy, trace_path, seconds, maximum_search_nodes = 0)
  command = ["timeout", "#{seconds}s", runner, "--solver", "local", "--strategy", strategy,
             "--trace", trace_path]
  command += ["--maximum-search-nodes", maximum_search_nodes.to_s] if maximum_search_nodes.positive?
  command << graph_path
  output, diagnostics, status = invoke(*command)
  [output, diagnostics, status]
end

def characterize(rows, runner, graph_path, label, family, profile, seed, timeout_seconds,
                 maximum_search_nodes)
  Tempfile.create(["pcaa-model-c-", ".jsonl"]) do |heuristic_trace|
    _output, diagnostics, status =
      trace_run(runner, graph_path, "heuristic-rn", heuristic_trace.path, timeout_seconds)
    abort "heuristic trace failed for #{label}: #{diagnostics}" unless status.success?
    heuristic_events = File.readlines(heuristic_trace.path, chomp: true)
                           .reject(&:empty?).map { |line| JSON.parse(line) }
    epoch_elements = model_c_epoch_elements(heuristic_events)
    per_child = median(epoch_elements)

    Tempfile.create(["pcaa-branch-", ".jsonl"]) do |branch_trace|
      branch_output, _branch_diagnostics, branch_status =
        trace_run(runner, graph_path, "exact-branch-reduce", branch_trace.path, timeout_seconds,
                  maximum_search_nodes)
      branch_events = File.readlines(branch_trace.path, chomp: true)
                          .reject(&:empty?).map { |line| JSON.parse(line) }
      branches = branch_events.select { |event| event.fetch("type") == "BRANCH_SELECT" }
      search_status = if !branch_status.success?
                        "timeout"
                      elsif branch_output.match?(/^status SEARCH_LIMIT$/)
                        "search-limit"
                      else
                        "complete"
                      end
      branches.each_with_index do |event, index|
        domain = event.fetch("branch_domain")
        rows << [family, profile, label, seed, index, domain, per_child,
                 domain * per_child, search_status]
      end
    end
  end
end

rows = []
SYNTHETIC_POINTS.each do |family, profile, nodes, domain|
  [1001, 1002].each do |seed|
    Tempfile.create(["pcaa-fork-", ".pbqp"]) do |graph|
      graph.write(generate_graph(options[:generator], family, profile, nodes, domain, seed))
      graph.flush
      label = "N#{nodes}-D#{domain}"
      characterize(rows, options[:runner], graph.path, label, family, profile, seed, 30, 2_000_000)
    end
  end
end

corpus_rows = CSV.read(options[:scaling_csv], headers: true)
                 .select { |row| row["family"] == "llvm-regalloc" && row["status"] == "ok" }
low = corpus_rows.select { |row| (1..3).cover?(row["rn"].to_i) }.first(30)
high = corpus_rows.select { |row| (4..8).cover?(row["rn"].to_i) }.first(9)
abort "scaling CSV does not contain the expected 30+9 real sample" unless low.length == 30 && high.length == 9
(low + high).each do |row|
  filename = row.fetch("profile")
  characterize(rows, options[:runner], File.join(options[:corpus_dir], filename), filename,
               "llvm-regalloc", "real", row.fetch("seed").to_i, 45, 1_000_000)
end

CSV.open(options[:output], "w") do |csv|
  csv << %w[family profile graph seed fork branch_domain model_c_elements_per_child
            independent_elements_at_fork search_status]
  rows.each { |row| csv << row }
end
