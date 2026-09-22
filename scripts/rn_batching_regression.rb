#!/usr/bin/env ruby
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 PCAA contributors
# Checks per-node RN batching against the retained per-edge implementation.

require "json"
require "open3"
require "tempfile"

runner, examples_dir, regalloc_dir = ARGV
abort "usage: rn_batching_regression.rb RUNNER EXAMPLES_DIR REGALLOC_DIR" unless ARGV.length == 3

def solve(runner, path, batching)
  output, diagnostics, status = Open3.capture3(
    runner, "--solver", "local", "--strategy", "heuristic-rn", "--rn-batching", batching,
    "--verbose", path
  )
  abort "#{path} (#{batching}) failed: #{diagnostics}" unless status.success?

  solution = output.lines.grep(/^(optimum|assignment) /).join
  match = diagnostics.match(/operation mix .* descriptors=(\d+) batches=(\d+)/)
  abort "#{path} (#{batching}) omitted operation-mix counters" unless match

  [solution, match[1].to_i, match[2].to_i]
end

top_level = Dir.glob(File.join(examples_dir, "*.pbqp")).sort
regalloc = Dir.glob(File.join(regalloc_dir, "*.pbqp")).sort
sample_count = [12, regalloc.length].min
sample = if sample_count <= 1
           regalloc.first(sample_count)
         else
           (0...sample_count).map { |index| regalloc[index * (regalloc.length - 1) / (sample_count - 1)] }
         end

collapsed = false
(top_level + sample).each do |path|
  per_node = solve(runner, path, "per-node")
  per_edge = solve(runner, path, "per-edge")
  abort "RN batching changed the solution for #{path}" unless per_node[0] == per_edge[0]
  abort "RN batching changed the child descriptor count for #{path}" unless per_node[1] == per_edge[1]
  abort "per-node batching increased submissions for #{path}" unless per_node[2] <= per_edge[2]
  collapsed ||= per_node[2] < per_edge[2]
end
abort "sample contained no graph on which per-node batching collapsed submissions" unless collapsed

# Existing consumers parse JSON objects and ignore additive fields. Exercise
# that path and require branch_domain on every new BRANCH_SELECT event.
Tempfile.create(["pcaa-branch-trace-", ".jsonl"]) do |trace|
  path = File.join(examples_dir, "chvatal.pbqp")
  _output, diagnostics, status = Open3.capture3(
    runner, "--solver", "local", "--strategy", "exact-branch-reduce",
    "--maximum-search-nodes", "100000", "--trace", trace.path, path
  )
  abort "branch trace run failed: #{diagnostics}" unless status.success?
  events = File.readlines(trace.path, chomp: true).reject(&:empty?).map { |line| JSON.parse(line) }
  branches = events.select { |event| event.fetch("type") == "BRANCH_SELECT" }
  abort "branch trace contained no BRANCH_SELECT event" if branches.empty?
  abort "branch trace omitted branch_domain" unless branches.all? { |event| event.fetch("branch_domain").positive? }
  events.each do |event|
    %w[sequence phase type policy node choice active_nodes active_edges].each { |key| event.fetch(key) }
  end
end
