#!/usr/bin/env ruby
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 PCAA contributors
# Checks exact driver CLI, oracle, seeded search and phase/cycle correspondence.
require "json"
require "open3"
require "pathname"
root = Pathname.new(__dir__).parent.parent
build = Pathname.new(ARGV.fetch(0)).expand_path
def run(root, build, executable, *arguments)
  out, error, status = Open3.capture3(build.join(executable).to_s, *arguments.map(&:to_s), chdir: root.to_s)
  raise "#{executable}: #{error}" unless status.success?
  out.lines.map { |l| JSON.parse(l) }
end
checks = 0
%w[triangle petersen chvatal].each do |name|
  graph = "examples/#{name}.pbqp"
  %w[none heuristic].each do |seed|
    cpu = run(root,build,"pcaa_exact_cpu","dense",graph,seed,10000,3,"vector")
    sample = cpu.find { |r| r["type"] == "sample" }
    event = cpu.find { |r| r["type"] == "events" }
    raise "incomplete CPU" unless sample.fetch("status") == 0
    model = run(root,build,"pcaa_exact_l2","dense",graph,seed,10000,3,"vector")
    actual = model.find { |r| r["type"] == "model" }
    baseline = model.find { |r| r["type"] == "baseline" }.fetch("l2")
    raise "answer mismatch" unless actual.values_at("objective","assignment","work") == event.values_at("objective","assignment","work")
    signature = ->(rows) { rows.map { |r| r.slice("phase","kind","count","elements") } }
    raise "callback mismatch" unless signature.call(actual.fetch("events")) == signature.call(event.fetch("events"))
    cycles = actual.fetch("events").sum { |r| r.fetch("cycles") }
    raise "cycle partition" unless cycles == actual.fetch("l2").fetch("cycles")-baseline.fetch("cycles")
    raise "phase partition" unless actual.fetch("events").all? { |r| r.fetch("phases").sum == r.fetch("cycles") }
    checks += 4
  end
  oracle = run(root,build,"pcaa_exact_cpu","dense",graph,"none",1000000,3,"vector","enumeration").find { |r| r["type"] == "sample" }
  exact = run(root,build,"pcaa_exact_cpu","structured",graph,"none",10000,3,"vector").find { |r| r["type"] == "sample" }
  raise "oracle mismatch" unless oracle.fetch("status") == 0 && exact.fetch("objective") == oracle.fetch("objective")
  checks += 1
end
limited = run(root,build,"pcaa_exact_cpu","dense","examples/petersen.pbqp","none",1,3).find { |r| r["type"] == "sample" }
raise "search limit mislabeled" unless limited.fetch("status") == -5 && limited.fetch("assignment").nil?
%w[0 -1 4294967296].each do |limit|
  _, _, status = Open3.capture3(build.join("pcaa_exact_cpu").to_s,"dense","examples/triangle.pbqp","none",limit,"3",chdir:root.to_s)
  raise "invalid limit accepted" if status.success?
  checks += 1
end
puts "#{checks+1} exact CLI/oracle/seed/phase/limit checks passed"
