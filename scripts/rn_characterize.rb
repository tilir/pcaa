#!/usr/bin/env ruby
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 PCAA contributors
# Characterizes public PBQP solver strategies by orchestrating the CLI tools.

require "csv"
require "open3"
require "optparse"
require "tempfile"

DEFAULT_SEEDS = (1001..1010).freeze
RN_POLICIES = %w[min-degree max-degree min-work].freeze
GENERAL_CORPUS = [
  ["random-sparse", "binary", 20], ["degree-3", "binary", 20],
  ["degree-4", "binary", 20], ["mixed-degree", "small", 20],
  ["tree", "register-like", 20], ["cycle", "register-like", 20]
].freeze
EXACT_CORPUS = [
  ["random-sparse", "binary", 8], ["degree-3", "binary", 8],
  ["degree-4", "binary", 8], ["mixed-degree", "small", 8]
].freeze

options = { runner: "build/pcaa_graph_run", generator: "build/pbqp_graph_generate",
            seeds: DEFAULT_SEEDS.to_a, output: nil, exact_limit: 100_000 }
OptionParser.new do |parser|
  parser.banner = "Usage: ruby scripts/rn_characterize.rb [options]"
  parser.on("--runner PATH", "pcaa_graph_run executable") { |value| options[:runner] = value }
  parser.on("--generator PATH", "pbqp_graph_generate executable") { |value| options[:generator] = value }
  parser.on("--seeds LIST", "Comma-separated seeds (default: 1001..1010)") do |value|
    options[:seeds] = value.split(",").map { |seed| Integer(seed, 10) }
  end
  parser.on("--exact-limit N", Integer, "Exact search-node limit (default: 100000)") do |value|
    options[:exact_limit] = value
  end
  parser.on("--output PATH", "Write CSV to PATH") { |value| options[:output] = value }
  parser.on("--help", "Show this help") { puts parser; exit 0 }
end.parse!
abort "unexpected argument: #{ARGV.first}" unless ARGV.empty?

def invoke(*command)
  output, diagnostics, status = Open3.capture3(*command)
  abort "#{command.join(' ')} failed: #{diagnostics}" unless status.success?
  [output, diagnostics]
end

def metrics(text, pattern, name)
  match = text.match(pattern)
  abort "missing #{name} in runner diagnostics" unless match
  match.captures.map(&:to_i)
end

def graph(generator, family, profile, nodes, seed)
  output, = invoke(generator, "--family", family, "--profile", profile, "--nodes", nodes.to_s,
                    "--seed", seed.to_s)
  output
end

def solve(runner, path, strategy, policy: "min-degree", exact_limit: 0)
  arguments = [runner, "--solver", "local", "--strategy", strategy, "--rn-policy", policy,
               "--verbose"]
  arguments += ["--maximum-search-nodes", exact_limit.to_s] unless exact_limit.zero?
  output, diagnostics = invoke(*arguments, path)
  status = output[/^status (\S+)$/, 1] || "OK"
  result = { status: status, objective: output[/^optimum (-?\d+)$/, 1]&.to_i }
  return result unless status == "OK"
  result[:reductions] = metrics(diagnostics,
    /reductions R0=(\d+) R1=(\d+) R2=(\d+) RN=(\d+) projections=(\d+) projection_primitives=(\d+) commits=(\d+)/,
    "reduction statistics")
  result[:condition] = metrics(diagnostics,
    /condition traffic operations=(\d+) elements=(\d+) matrix-read=(\d+) unary-read=(\d+) unary-write=(\d+)/,
    "conditioning traffic")
  result[:local] = metrics(diagnostics,
    /local search evaluations=(\d+) sweeps=(\d+) accepted-moves=(\d+) slices=(\d+) slice-elements=(\d+) argmin=(\d+)/,
    "local-search statistics")
  result[:operation_mix] = metrics(diagnostics,
    /operation mix project-elements=(\d+) project-accumulate-elements=(\d+) slice-elements=(\d+) map3-elements=(\d+) argmin-elements=(\d+) descriptors=(\d+) bytes=(\d+)/,
    "operation mix")
  result[:search] = metrics(diagnostics,
    /exact search nodes=(\d+) branches=(\d+) max-depth=(\d+) limit-hits=(\d+)/,
    "search statistics")
  result
end

header = %w[family profile nodes seed strategy policy status objective exact_objective gap
            r0 r1 r2 rn projections projection_primitives rn_commit_elements
            condition_operations condition_elements condition_matrix_read condition_unary_read condition_unary_write
            local_evaluations local_sweeps local_moves local_slices local_slice_elements local_argmin
            project_elements project_accumulate_elements slice_elements map3_elements argmin_elements descriptors bytes
            search_nodes search_branches search_depth search_limit_hits]
rows = []

def collect(rows, corpus, exact, options)
  corpus.each do |family, profile, nodes|
    options[:seeds].each do |seed|
      Tempfile.create(["pcaa-solver-", ".pbqp"]) do |file|
        file.write(graph(options[:generator], family, profile, nodes, seed))
        file.flush
        exact_result = exact ? solve(options[:runner], file.path, "exact-branch-reduce",
                                     exact_limit: options[:exact_limit]) : nil
        strategies = [["reduce-only", "min-degree"], ["local-search", "min-degree"]]
        RN_POLICIES.each do |policy|
          strategies << ["heuristic-rn", policy]
          strategies << ["heuristic-rn-local-search", policy]
        end
        strategies << ["exact-core-enumeration", "min-degree"] if exact
        strategies << ["exact-branch-reduce", "min-degree"] if exact
        strategies.each do |strategy, policy|
          result = strategy == "exact-branch-reduce" ? exact_result :
                   solve(options[:runner], file.path, strategy, policy: policy,
                         exact_limit: exact ? options[:exact_limit] : 0)
          reductions = result[:reductions] || Array.new(7)
          condition = result[:condition] || Array.new(5)
          local = result[:local] || Array.new(6)
          operation_mix = result[:operation_mix] || Array.new(7)
          search = result[:search] || Array.new(4)
          exact_objective = exact_result && exact_result[:status] == "OK" ? exact_result[:objective] : nil
          gap = result[:objective] && exact_objective ? result[:objective] - exact_objective : nil
          rows << [family, profile, nodes, seed, strategy, policy, result[:status], result[:objective],
                   exact_objective, gap, *reductions, *condition, *local, *operation_mix, *search]
        end
      end
    end
  end
end

collect(rows, GENERAL_CORPUS, false, options)
collect(rows, EXACT_CORPUS, true, options)
output = options[:output] ? File.open(options[:output], "w") : $stdout
CSV(output) { |csv| csv << header; rows.each { |row| csv << row } }
output.close if options[:output]

irreducible = rows.count { |row| row[4] == "reduce-only" && row[6] == "IRREDUCIBLE" }
warn "rows=#{rows.size} reduce-only-irreducible=#{irreducible}"
