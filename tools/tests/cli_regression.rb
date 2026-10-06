#!/usr/bin/env ruby
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 PCAA contributors
# Exercises runner CLI errors and verifies optional evidence does not change solves/cycles.
require "json"
require "pathname"
require "tmpdir"
require_relative "../../scripts/l2_characterize"

build = Pathname.new(ARGV.fetch(0)).expand_path
checks = 0
runners = %w[pcaa_graph_run pcaa_graph_run_timed pcaa_graph_run_l2]
triangle = ROOT.join("examples/triangle.pbqp")
expected = ["optimum -3", "assignment 1 0 1", "solution heuristic", "strategy HEURISTIC_RN"]
Dir.mktmpdir("pcaa-tools-cli-") do |directory|
  runners.each do |name|
    runner = build.join(name)
    plain = invoke([runner, "--solver", "local", triangle], timeout: 15)
    raise "#{name} plain failed" unless plain.fetch("status") == "ok" && answer(plain.fetch("stdout")) == expected
    checks += 1
    profile = File.join(directory, "#{name}-profile.jsonl")
    trace = File.join(directory, "#{name}-trace.jsonl")
    recorded = invoke([runner, "--solver", "local", "--kernel-profile", profile, "--trace", trace, triangle], timeout: 15)
    raise "#{name} optional recording changed output" unless recorded.fetch("status") == "ok" && recorded.fetch("stdout") == plain.fetch("stdout")
    batches = File.readlines(profile).map { |line| JSON.parse(line) }
    events = File.readlines(trace).map { |line| JSON.parse(line) }
    raise "#{name} missing batch/trace records" unless batches.length == 2 && events.length == 3
    raise "#{name} wrong callback stream" unless batches.map { |r| r.values_at("kind", "count") } == [["map3_project_batch", 2], ["min2_batch", 2]]
    checks += 1
    [["--help"], ["--version"]].each do |args|
      result = invoke([runner, *args], timeout: 15)
      raise "#{name} #{args} failed" unless result.fetch("status") == "ok" && !result.fetch("stdout").empty?
      checks += 1
    end
    [[], [triangle], ["--solver", "unknown", triangle], ["--solver", "local", "--strategy", "unknown", triangle],
     ["--solver", "local", "--rn-policy", "unknown", triangle], ["--solver", "local", "--maximum-search-nodes", "-1", triangle],
     ["--solver", "local", "--kernel-profile"], ["--solver", "local", "--unknown", triangle],
     ["--solver", "local", "--kernel-profile", File.join(directory, "missing", "profile"), triangle]].each do |args|
      result = invoke([runner, *args], timeout: 15)
      raise "#{name} accepted #{args.inspect}" unless result.fetch("status") == "failed" && !result.fetch("stderr").empty?
      checks += 1
    end
  end
  %w[--l2-lanes --l2-mem-bytes --l2-tm --l2-tn --l2-memory-latency].each do |flag|
    %w[0 65536 broken].each do |value|
      result = invoke([build.join("pcaa_graph_run_l2"), "--solver", "local", flag, value, triangle], timeout: 15)
      raise "L2 accepted #{flag} #{value}" unless result.fetch("status") == "failed" && result.fetch("stderr").include?("invalid L2 option")
      checks += 1
    end
  end
  %w[unknown current].each do |variant|
    result = invoke([build.join("pcaa_cpu_bench"), variant, triangle, "0"], timeout: 15)
    raise "native accepted invalid arguments" unless result.fetch("status") == "failed"
    checks += 1
  end
end
puts "#{checks} CLI, optional-recording and configuration checks passed"
