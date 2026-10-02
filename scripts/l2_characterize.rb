#!/usr/bin/env ruby
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 PCAA contributors
# Runs current streams and controlled MAS sweeps, retaining evidence and failures.

require "digest"
require "fileutils"
require "json"
require "open3"
require "optparse"
require "pathname"
require "rbconfig"
require "zlib"

ROOT = Pathname.new(__dir__).parent.expand_path
BASE = { "lanes" => 4, "mem_bytes" => 16, "tm" => 8, "tn" => 16,
         "memory_latency" => 1 }.freeze
FACTORS = { "lanes" => [1, 2, 4, 8, 16], "tm" => [1, 2, 4, 8, 16, 32],
            "tn" => [4, 8, 16, 32, 64], "mem_bytes" => [4, 8, 16, 32],
            "memory_latency" => [1, 2, 4, 8, 16] }.freeze
FLAGS = { "lanes" => "--l2-lanes", "mem_bytes" => "--l2-mem-bytes",
          "tm" => "--l2-tm", "tn" => "--l2-tn",
          "memory_latency" => "--l2-memory-latency" }.freeze
REQUEST_TIMEOUT_SECONDS = 180
SUBSET_SIZE = 8
SYNTHETIC_NODES = 20
SYNTHETIC_SEED = 1001

# Drain both pipes concurrently; a timeout terminates the entire child process group
# and retains partial output without inventing completion counters.
def invoke(command, timeout: REQUEST_TIMEOUT_SECONDS)
  command = command.map(&:to_s)
  result = { "command" => command }
  Open3.popen3(*command, chdir: ROOT.to_s, pgroup: true) do |stdin, stdout, stderr, waiter|
    stdin.close
    output = Thread.new { stdout.read }
    diagnostics = Thread.new { stderr.read }
    if waiter.join(timeout)
      status = waiter.value
      result["returncode"] = status.exitstatus || -status.termsig
      result["status"] = status.success? ? "ok" : "failed"
    else
      result["status"] = "timeout"
      begin
        Process.kill("TERM", -waiter.pid)
        Process.kill("KILL", -waiter.pid) unless waiter.join(1)
      rescue Errno::ESRCH
        # The process may have exited between the timeout and termination.
      end
      waiter.join
    end
    result["stdout"] = output.value
    result["stderr"] = diagnostics.value
  end
  result
end

def configurations(smoke)
  result = [BASE.dup]
  return result if smoke

  FACTORS.each do |key, values|
    values.each do |value|
      config = BASE.merge(key => value)
      result << config unless result.include?(config)
    end
  end
  FACTORS.fetch("lanes").product(FACTORS.fetch("mem_bytes")).each do |lanes, width|
    config = BASE.merge("lanes" => lanes, "mem_bytes" => width)
    result << config unless result.include?(config)
  end
  result
end

def relative(path)
  Pathname.new(path).relative_path_from(ROOT).to_s
end

def write_json(path, value)
  File.write(path, JSON.pretty_generate(value) + "\n")
end

def record(stream, row)
  stream.puts(JSON.generate(row))
end

def answer(text)
  text.lines.map(&:chomp).select { |line| line.start_with?("optimum ", "assignment ", "solution ", "strategy ") }
end

def characterize(options, invocation)
  out = ROOT.join(options[:output]).cleanpath
  build = ROOT.join(options[:build]).cleanpath
  FileUtils.mkdir_p(out.join("inputs"))
  corpus = ROOT.glob("examples/regalloc/*.pbqp").sort
  ordered = corpus.sort_by do |path|
    line = path.each_line.find { |item| item.start_with?("nodes ") }
    [Integer(line.split[1]), path.basename.to_s]
  end
  subset = SUBSET_SIZE.times.map do |index|
    # Python's original driver used ties-to-even rounding; retain that selection.
    ordered[(index * (ordered.length - 1).fdiv(SUBSET_SIZE - 1)).round(half: :even)]
  end
  examples = %w[triangle petersen chvatal random-20].map { |name| ROOT.join("examples/#{name}.pbqp") }
  synthetic = []
  %w[degree-3 degree-4 mixed-degree].each do |family|
    (options[:smoke] ? [4] : [2, 4, 8, 16]).each do |domain|
      generated = invoke([build.join("pbqp_graph_generate"), "--family", family, "--profile", "small",
                          "--nodes", SYNTHETIC_NODES, "--seed", SYNTHETIC_SEED, "--domain-size", domain])
      raise generated.inspect unless generated.fetch("status") == "ok"

      path = out.join("inputs/#{family}-d#{domain}.pbqp")
      path.write(generated.fetch("stdout"))
      synthetic << path
    end
  end
  source_names = %w[accelerator/tests/l2_support.h accelerator/tests/l2_unit.cpp
                    tools/l2_microbench.cpp tools/pbqp_model_kernel.cpp tools/pbqp_model_kernel.h
                    tools/pbqp_run.cpp tools/CMakeLists.txt accelerator/CMakeLists.txt
                    scripts/l2_characterize.rb]
  sources = (ROOT.glob("accelerator/include/l2_*.h") + ROOT.glob("accelerator/src/l2_*.cpp") +
             source_names.map { |path| ROOT.join(path) }).uniq.sort
  configs = configurations(options[:smoke])
  metadata = {
    "base_commit" => invoke(%w[git rev-parse HEAD]).fetch("stdout").strip,
    "mas" => "1.0.0", "isa" => "1.0.0", "baseline" => BASE, "configurations" => configs,
    "selection" => "8 evenly spaced ranks after sorting by (nodes, filename), including endpoints",
    "subset" => subset.map { |path| relative(path) },
    "implementation_sha256" => sources.to_h { |path| [relative(path), Digest::SHA256.file(path).hexdigest] },
    "input_sha256" => (corpus + examples + synthetic).to_h do |path|
      [relative(path), Digest::SHA256.file(path).hexdigest]
    end,
    "command" => [RbConfig.ruby, $PROGRAM_NAME, *invocation], "build" => relative(build)
  }
  write_json(out.join("metadata.json"), metadata)
  failures = 0
  Zlib::GzipWriter.open(out.join("microbench.jsonl.gz")) do |rows|
    Zlib::GzipWriter.open(out.join("runs.jsonl.gz")) do |raw|
      configs.each do |config|
        flags = config.flat_map { |key, value| [FLAGS.fetch(key), value.to_s] }
        result = invoke([build.join("pcaa_l2_microbench"), *flags, *(options[:smoke] ? ["--smoke"] : [])])
        record(raw, { "kind" => "microbench", "config" => config }.merge(result))
        if result.fetch("status") != "ok"
          failures += 1
        else
          result.fetch("stdout").each_line do |line|
            row = JSON.parse(line)
            raise "unverified microbenchmark" unless row.fetch("verified")

            record(rows, row)
          end
        end
        puts "microbench #{config} #{result.fetch('status')}"
      end
    end
  end
  l1_cache = {}
  Zlib::GzipWriter.open(out.join("workloads.jsonl.gz")) do |rows|
    Zlib::GzipWriter.open(out.join("workload-runs.jsonl.gz")) do |raw|
      configs.each_with_index do |config, ci|
        inputs = if options[:smoke]
                   [examples.first, subset.first, synthetic.first]
                 elsif ci.zero?
                   examples + corpus + synthetic
                 else
                   examples + subset + synthetic.select { |path| path.basename.to_s.match?(/-d(?:4|16)\./) }
                 end
        flags = config.flat_map { |key, value| [FLAGS.fetch(key), value.to_s] }
        inputs.each_with_index do |path, index|
          identity = relative(path)
          result = invoke([build.join("pcaa_graph_run_l2"), "--solver", "local", "--verbose", *flags, path])
          record(raw, { "kind" => "l2", "input" => identity, "config" => config }.merge(result))
          unless l1_cache.key?(identity)
            l1_cache[identity] = invoke([build.join("pcaa_graph_run_timed"), "--solver", "local", "--verbose", path])
            record(raw, { "kind" => "l1", "input" => identity }.merge(l1_cache.fetch(identity)))
          end
          l1 = l1_cache.fetch(identity)
          row = { "input" => identity, "config" => config, "status" => result.fetch("status") }
          if result.fetch("status") == "ok" && l1.fetch("status") == "ok"
            row["l2"] = JSON.parse(result.fetch("stdout").lines.find { |line| line.start_with?("l2 ") }.delete_prefix("l2 "))
            timing = l1.fetch("stdout").lines.find { |line| line.start_with?("timing cycles=") }
            row["l1"] = timing.split.drop(1).to_h { |field| key, value = field.split("=", 2); [key, Integer(value)] }
            row["verified"] = answer(result.fetch("stdout")) == answer(l1.fetch("stdout"))
            row["answer"] = answer(result.fetch("stdout"))
            row["logical"] = result.fetch("stderr").lines.find { |line| line.start_with?("pcaa: operation mix ") }.chomp
            unless row.fetch("verified")
              row["status"] = "differential_failure"
              failures += 1
            end
          else
            failures += 1
          end
          record(rows, row)
          puts "workloads #{ci} #{index} #{inputs.length} #{identity} #{row.fetch('status')}" if (index % 50).zero?
        end
      end
    end
  end
  metadata["failures"] = failures
  write_json(out.join("metadata.json"), metadata)
  raise "#{failures} failed/time-limited rows; headline conclusions prohibited" unless failures.zero?

  puts "complete: no failed or time-limited rows"
end

if $PROGRAM_NAME == __FILE__
  invocation = ARGV.dup
  options = { build: "build-release", output: "doc/reports/data/l2", smoke: false }
  OptionParser.new do |parser|
    parser.banner = "Usage: ruby scripts/l2_characterize.rb [options]"
    parser.on("--build PATH") { |value| options[:build] = value }
    parser.on("--output PATH") { |value| options[:output] = value }
    parser.on("--smoke") { options[:smoke] = true }
  end.parse!
  abort "unexpected arguments: #{ARGV.join(' ')}" unless ARGV.empty?
  $stdout.sync = true
  characterize(options, invocation)
end
