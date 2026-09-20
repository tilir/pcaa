#!/usr/bin/env ruby
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 PCAA contributors
# Runs reproducible RN characterization experiments through the public graph-runner CLI.

require "csv"
require "open3"
require "optparse"
require "tempfile"

DEFAULT_SEEDS = (1001..1010).freeze
POLICIES = %w[min-degree max-degree min-work].freeze

options = {
  runner: "build/pcaa_graph_run",
  timed_runner: "build/pcaa_graph_run_timed",
  generator: "build/pbqp_graph_generate",
  seeds: DEFAULT_SEEDS.to_a,
  timed: false,
  output: nil
}

OptionParser.new do |parser|
  parser.banner = "Usage: ruby scripts/rn_characterize.rb [options]"
  parser.on("--runner PATH", "Untimed pcaa_graph_run executable") { |path| options[:runner] = path }
  parser.on("--timed-runner PATH", "Timed pcaa_graph_run_timed executable") do |path|
    options[:timed_runner] = path
  end
  parser.on("--generator PATH", "pbqp_graph_generate executable") { |path| options[:generator] = path }
  parser.on("--seeds LIST", "Comma-separated synthetic seeds (default: 1001..1010)") do |list|
    options[:seeds] = list.split(",").map { |seed| Integer(seed, 10) }
  end
  parser.on("--timed", "Collect L1 output for min-degree RN as well") { options[:timed] = true }
  parser.on("--output PATH", "Write CSV to PATH instead of standard output") { |path| options[:output] = path }
  parser.on("--help", "Show this help text") do
    puts parser
    exit 0
  end
end.parse!

abort "unexpected argument: #{ARGV.first}" unless ARGV.empty?

def run_runner(runner, arguments)
  stdout, stderr, status = Open3.capture3(runner, *arguments)
  abort "#{runner} failed: #{stderr}" unless status.success?

  [stdout, stderr]
end

def metric(text, expression, name)
  match = text.match(expression)
  abort "missing #{name} in runner output" unless match

  match.captures.map(&:to_i)
end

def generate_graph(generator, seed)
  stdout, stderr, status = Open3.capture3(
    generator, "--family", "random-sparse", "--profile", "binary", "--nodes", "20", "--seed", seed.to_s
  )
  abort "#{generator} failed: #{stderr}" unless status.success?

  stdout
end

def solve(runner, input_path, strategy, policy: nil, verbose: false)
  arguments = ["--solver", "local", "--strategy", strategy]
  arguments += ["--rn-policy", policy] if policy
  arguments << "--verbose" if verbose
  arguments << input_path
  stdout, stderr = run_runner(runner, arguments)
  result = { status: stdout.include?("status IRREDUCIBLE") ? "IRREDUCIBLE" : "OK" }
  result[:objective] = metric(stdout, /^optimum (-?\d+)$/, "objective").first if result[:status] == "OK"
  return result unless verbose

  result[:reductions] = metric(
    stderr,
    /reductions R0=(\d+) R1=(\d+) R2=(\d+) RN=(\d+) projections=(\d+) projection_primitives=(\d+) commits=(\d+)/,
    "reduction statistics"
  )
  result[:core] = metric(
    stderr,
    /RN core first=(\d+) nodes\/(\d+) edges max=(\d+) nodes\/(\d+) edges episodes=(\d+) degree=(\d+)\.\.(\d+) after-RN R0=(\d+) R1=(\d+) R2=(\d+)/,
    "RN core statistics"
  )
  result[:traffic] = metric(
    stderr,
    /RN traffic projection-read=(\d+) projection-write=(\d+) score-accumulation=(\d+) commit-bytes=(\d+)/,
    "RN traffic statistics"
  )
  result[:search] = metric(
    stderr,
    /exact search nodes=(\d+) branches=(\d+) max-depth=(\d+) limit-hits=(\d+)/,
    "exact-search statistics"
  )
  result
end

def timing(runner, input_path)
  stdout, = run_runner(
    runner,
    ["--solver", "local", "--strategy", "heuristic-rn", "--rn-policy", "min-degree", input_path]
  )
  metric(
    stdout,
    /timing cycles=(\d+) descriptor=(\d+) operands=(\d+) compute=(\d+) result=(\d+) primitives=(\d+) batches=(\d+)/,
    "timing statistics"
  )
end

header = %w[
  seed reduce_only policy objective exact_objective absolute_gap
  exact_r0 exact_r1 exact_r2 exact_search_nodes exact_search_branches exact_search_maximum_depth exact_search_limit_hits
  r0 r1 r2 rn projections projection_primitives commits
  first_core_nodes first_core_edges maximum_core_nodes maximum_core_edges rn_episodes
  rn_degree_min rn_degree_max after_rn_r0 after_rn_r1 after_rn_r2
  projection_read_bytes projection_write_bytes score_accumulation_elements commit_bytes
  timing_cycles timing_descriptor timing_operands timing_compute timing_result timing_primitives timing_batches
]

rows = []
options[:seeds].each do |seed|
  Tempfile.create(["pcaa-rn-", ".pbqp"]) do |input|
    input.write(generate_graph(options[:generator], seed))
    input.flush
    reduce_only = solve(options[:runner], input.path, "reduce-only")[:status]
    exact = solve(options[:runner], input.path, "exact-branch-reduce", verbose: true)
    exact_reductions = exact.fetch(:reductions)
    exact_search = exact.fetch(:search)
    POLICIES.each do |policy|
      result = solve(options[:runner], input.path, "heuristic-rn", policy: policy, verbose: true)
      reductions = result.fetch(:reductions)
      core = result.fetch(:core)
      traffic = result.fetch(:traffic)
      timing_values = options[:timed] && policy == "min-degree" ? timing(options[:timed_runner], input.path) : Array.new(7)
      rows << [seed, reduce_only, policy, result.fetch(:objective), exact.fetch(:objective),
               result.fetch(:objective) - exact.fetch(:objective), *exact_reductions.first(3),
               *exact_search, *reductions, *core, *traffic, *timing_values]
    end
  end
end

output = options[:output] ? File.open(options[:output], "w") : $stdout
CSV(output) do |csv|
  csv << header
  rows.each { |row| csv << row }
end
output.close if options[:output]

irreducible = rows.count { |row| row[1] == "IRREDUCIBLE" } / POLICIES.size
optimal = rows.count { |row| row[5].zero? }
gaps = rows.map { |row| row[5] }
warn "synthetic-random-20 seeds=#{options[:seeds].join(',')} reduce-only-irreducible=#{irreducible}/#{options[:seeds].size} " \
     "heuristic-optimal=#{optimal}/#{rows.size} mean-gap=#{gaps.sum.to_f / gaps.size} max-gap=#{gaps.max}"
