#!/usr/bin/env ruby
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 PCAA contributors
# Projects vector-output primitive cycles from timed scalar PBQP executions.

require "csv"
require "open3"
require "optparse"
require "tempfile"

FAMILIES = [["degree-3", "binary"], ["degree-4", "binary"],
            ["mixed-degree", "small"]].freeze
NODES = [20, 50, 100, 200].freeze
DOMAINS = [2, 4, 8, 16, 32].freeze
SEEDS = [1001, 1002, 1003].freeze
COMPONENTS = %w[project-scalar project-vector map3-scalar map3-partial map3-full].freeze

options = { runner: "build/pcaa_graph_run_timed", generator: "build/pbqp_graph_generate",
            corpus_dir: "examples/regalloc", output: "build/vector-cycle-projection.csv",
            rn_batching: "per-node" }
OptionParser.new do |parser|
  parser.on("--runner PATH") { |value| options[:runner] = value }
  parser.on("--generator PATH") { |value| options[:generator] = value }
  parser.on("--corpus-dir PATH") { |value| options[:corpus_dir] = value }
  parser.on("--output PATH") { |value| options[:output] = value }
  parser.on("--rn-batching MODE", %w[per-node per-edge]) do |value|
    options[:rn_batching] = value
  end
end.parse!
abort "unexpected argument: #{ARGV.first}" unless ARGV.empty?

def invoke(*command)
  Open3.capture3(*command)
end

def characterize(runner, path, batching)
  output, diagnostics, status = invoke(runner, "--solver", "local", "--strategy", "heuristic-rn",
                                        "--rn-batching", batching, "--verbose", path)
  abort "timed solve failed for #{path}: #{diagnostics}" unless status.success?
  text = output + diagnostics
  timing = text.match(/timing cycles=(\d+) descriptor=(\d+) operands=(\d+) compute=(\d+) result=(\d+) primitives=(\d+) batches=(\d+)/)
  abort "missing timing for #{path}" unless timing
  components = COMPONENTS.to_h do |name|
    match = text.match(/projection #{name} cycles=(\d+) descriptor=(\d+) operands=(\d+) compute=(\d+) result=(\d+) descriptors=(\d+)/)
    abort "missing #{name} projection for #{path}" unless match
    [name, match.captures.map(&:to_i)]
  end
  actual = timing[1].to_i
  project_scalar = components.fetch("project-scalar")[0]
  project_vector = components.fetch("project-vector")[0]
  map3_scalar = components.fetch("map3-scalar")[0]
  map3_partial = components.fetch("map3-partial")[0]
  map3_full = components.fetch("map3-full")[0]
  [*timing.captures.map(&:to_i),
   actual - project_scalar + project_vector,
   actual - map3_scalar + map3_partial,
   actual - map3_scalar + map3_full,
   actual - project_scalar - map3_scalar + project_vector + map3_partial,
   actual - project_scalar - map3_scalar + project_vector + map3_full,
   *COMPONENTS.flat_map { |name| components.fetch(name) }]
end

rows = []
FAMILIES.each do |family, profile|
  NODES.each do |nodes|
    DOMAINS.each do |domain|
      SEEDS.each do |seed|
        graph_text, diagnostics, status = invoke(options[:generator], "--family", family,
                                                  "--profile", profile, "--nodes", nodes.to_s,
                                                  "--domain-size", domain.to_s,
                                                  "--seed", seed.to_s)
        abort "graph generation failed: #{diagnostics}" unless status.success?
        Tempfile.create(["pcaa-vector-cycle-", ".pbqp"]) do |file|
          file.write(graph_text)
          file.flush
          rows << [family, profile, nodes, domain, seed, file.path,
                   *characterize(options[:runner], file.path, options[:rn_batching])]
        end
      end
    end
  end
end

Dir.glob(File.join(options[:corpus_dir], "*.pbqp")).sort.each_with_index do |path, index|
  rows << ["llvm-regalloc", File.basename(path), nil, nil, index, path,
           *characterize(options[:runner], path, options[:rn_batching])]
end

timing_fields = %w[actual_cycles descriptor_cycles operand_cycles compute_cycles result_cycles
                   primitive_descriptors batch_submissions]
projection_fields = %w[project_only_total_cycles map3_partial_only_total_cycles
                       map3_full_only_total_cycles combined_partial_total_cycles
                       combined_full_total_cycles]
component_fields = COMPONENTS.flat_map do |name|
  %w[cycles descriptor operands compute result descriptors].map { |field| "#{name.tr('-', '_')}_#{field}" }
end
CSV.open(options[:output], "w") do |csv|
  csv << %w[family profile nodes domain seed graph] + timing_fields + projection_fields +
         component_fields
  rows.each { |row| csv << row }
end
