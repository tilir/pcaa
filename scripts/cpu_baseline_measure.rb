#!/usr/bin/env ruby
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 PCAA contributors
# Archives fresh Release native samples and matched L1/L2 streams on the retained corpus.
require "digest"
require "fileutils"
require "json"
require "open3"
require "optparse"
require "pathname"
require "rbconfig"
require "zlib"
require "time"
require_relative "l2_characterize"

options = { build: "build-cpu-release", output: "doc/reports/data/cpu-baseline", cpu: 0, samples: 11,
            model_samples: 3, smoke: false }
invocation = ARGV.dup
OptionParser.new do |parser|
  parser.on("--build PATH") { |v| options[:build] = v }
  parser.on("--output PATH") { |v| options[:output] = v }
  parser.on("--cpu N", Integer) { |v| options[:cpu] = v }
  parser.on("--samples N", Integer) { |v| options[:samples] = v }
  parser.on("--model-samples N", Integer) { |v| options[:model_samples] = v }
  parser.on("--smoke") { options[:smoke] = true }
end.parse!
abort "unexpected arguments" unless ARGV.empty?
abort "insufficient repetitions" unless options[:samples] >= 3 && options[:model_samples] >= 3
$stdout.sync = true
out = ROOT.join(options[:output])
build = ROOT.join(options[:build])
abort "archive exists; use a fresh output directory (no revision mixing)" if out.join("metadata.json").exist?
FileUtils.mkdir_p(out)
cache = build.join("CMakeCache.txt").read
abort "Release build required" unless cache.include?("CMAKE_BUILD_TYPE:STRING=Release")
corpus = ROOT.glob("examples/regalloc/*.pbqp").sort
abort "expected 491 retained graphs" unless corpus.length == 491
inputs = options[:smoke] ? [ROOT.join("examples/triangle.pbqp"), *corpus.first(2)] : corpus
sources = ROOT.glob("{accelerator,pcaalib,software,tools,cmake}/**/*").select(&:file?).reject do |p|
  p.to_s.include?("/testdata/")
end + %w[CMakeLists.txt scripts/cpu_baseline_measure.rb scripts/cpu_baseline_report.rb
         scripts/l2_characterize.rb].map { |p| ROOT.join(p) }
sources = sources.select(&:exist?).uniq.sort
binaries = %w[pcaa_cpu_bench pcaa_cpu_current pcaa_cpu_microbench pcaa_graph_run_l2_model
              pcaa_graph_run_timed_model].map { |p| build.join(p) }
metadata = {
  "base_commit" => invoke(%w[git rev-parse HEAD]).fetch("stdout").strip,
  "started_utc" => Time.now.utc.iso8601,
  "command" => [RbConfig.ruby, $PROGRAM_NAME, *invocation], "build" => relative(build),
  "cpu" => options[:cpu], "samples" => options[:samples], "model_samples" => options[:model_samples],
  "warmups" => 2, "profile_samples" => 5, "kernel_samples" => 15, "kernel_iterations" => 2000,
  "solver_policy" => "heuristic-rn/min-degree/per-node/local capacities", "smoke" => options[:smoke],
  "environment" => [%w[lscpu], %w[uname -a], %w[c++ --version], %w[ruby --version],
                    ["taskset", "-pc", Process.pid.to_s]].to_h do |command|
    [command.join(" "), invoke(command).slice("returncode", "stdout", "stderr")]
  end,
  "implementation_sha256" => sources.to_h { |p| [relative(p), Digest::SHA256.file(p).hexdigest] },
  "input_sha256" => inputs.to_h { |p| [relative(p), Digest::SHA256.file(p).hexdigest] },
  "binary_sha256" => binaries.to_h { |p| [relative(p), Digest::SHA256.file(p).hexdigest] },
  "systemc" => invoke(["pkg-config", "--modversion", "systemc"]).fetch("stdout").strip,
  "cpuinfo" => File.read("/proc/cpuinfo"), "proc_version" => File.read("/proc/version"),
  "native_libraries" => invoke(["ldd", build.join("pcaa_cpu_bench")]).fetch("stdout"),
  "failures" => []
}
# Persist configuration before taking any timing sample.
write_json(out.join("metadata.json"), metadata)
FileUtils.cp(build.join("CMakeCache.txt"), out.join("CMakeCache.txt"))
FileUtils.cp(build.join("compile_commands.json"), out.join("compile_commands.json"))
result = invoke(["tar", "-czf", out.join("implementation.tar.gz"), *sources.map { |p| relative(p) }])
raise result.inspect unless result.fetch("status") == "ok"
result = invoke(["tar", "-czf", out.join("inputs.tar.gz"), *inputs.map { |p| relative(p) }])
raise result.inspect unless result.fetch("status") == "ok"
pin = ["taskset", "-c", options[:cpu].to_s]
Zlib::GzipWriter.open(out.join("native-runs.jsonl.gz")) do |stream|
  inputs.each_with_index do |path, index|
    %w[current_original current scalar dense structured].each do |variant|
      executable = variant == "current_original" ? "pcaa_cpu_current" : "pcaa_cpu_bench"
      argument = variant == "current_original" ? "current" : variant
      result = invoke([*pin, build.join(executable), argument, path, options[:samples]], timeout: 600)
      record(stream, { "input" => relative(path), "variant" => variant }.merge(result))
      if result.fetch("status") != "ok"
        metadata["failures"] << [relative(path), variant, result.fetch("status")]
      end
    end
    puts "native #{index + 1}/#{inputs.length} #{path.basename}" if index % 10 == 0
  end
end
result = invoke([*pin, build.join("pcaa_cpu_microbench")], timeout: 600)
Zlib::GzipWriter.open(out.join("kernel-run.jsonl.gz")) { |stream| record(stream, result) }
metadata["failures"] << ["microbench", result.fetch("status")] unless result.fetch("status") == "ok"
Zlib::GzipWriter.open(out.join("model-runs.jsonl.gz")) do |stream|
  inputs.each_with_index do |path, index|
    %w[l1 l2].each do |model|
      count = model == "l1" ? 1 : options[:model_samples]
      count.times do |sample|
        events = out.join("temporary-model-events.jsonl")
        executable = model == "l1" ? "pcaa_graph_run_timed" : "pcaa_graph_run_l2"
        result = invoke([*pin, build.join(executable), "--solver", "local", "--strategy", "heuristic-rn", "--rn-policy", "min-degree", "--rn-batching", "per-node", "--verbose",
                         "--kernel-profile", events, path], timeout: 600)
        event_text = events.exist? ? events.read : ""
        events.delete if events.exist?
        record(stream, { "input" => relative(path), "model" => model,
                         "sample" => sample, "events" => event_text }.merge(result))
        metadata["failures"] << [relative(path), model, sample, result.fetch("status")] unless result.fetch("status") == "ok"
      end
    end
    puts "models #{index + 1}/#{inputs.length} #{path.basename}" if index % 10 == 0
  end
end
metadata["finished_utc"] = Time.now.utc.iso8601
write_json(out.join("metadata.json"), metadata)
raise "failed runs retained: #{metadata['failures'].inspect}" unless metadata["failures"].empty?
puts "complete; regenerate and verify with scripts/cpu_baseline_report.rb"
