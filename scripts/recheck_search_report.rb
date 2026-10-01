#!/usr/bin/env ruby
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 PCAA contributors
# Recollects explicit historical search inputs, counters, and raw solver traces.

require "csv"
require "digest"
require "fileutils"
require "json"
require "open3"
require "optparse"
require "zlib"

ROOT = File.expand_path("..", __dir__)
ELEMENTS = %w[project project_accumulate slice map3 argmin].freeze
STRUCTURAL = %w[R0 R1 R2 RN_SELECT RN_SCORE RN_COMMIT].freeze
DEVICE_LOG = ["pcaa: doorbell", "pcaa: batch children", "pcaa: child=", "pcaa: primitive"].freeze
FIELDS = %w[family profile graph seed input_sha256 status wall_seconds nodes branches depth
            pruned dfs_open_bound forks branch_domain_min branch_domain_max optimum
            model_c_median_elements].freeze

# Read stderr concurrently: verbose device diagnostics must not block stdout.
def invoke(command, filter_device: false)
  Open3.popen3(*command) do |input, output, errors, process|
    input.close
    reader = Thread.new do
      errors.each_line.reject { |line| filter_device && line.start_with?(*DEVICE_LOG) }.join
    end
    text = output.read
    [text, reader.value, process.value]
  end
end

def median(values)
  sorted = values.sort
  return 0 if sorted.empty?

  middle = sorted.length / 2
  sorted.length.odd? ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) / 2.0
end

def collect(options)
  output = File.expand_path(options.fetch(:output))
  FileUtils.mkdir_p(output)
  runner = File.expand_path(options.fetch(:runner))
  generator = File.expand_path(options.fetch(:generator))
  samples = Zlib::GzipReader.open(File.join(ROOT, "doc/reports/data/historical/fork-parallelism.csv.gz")) do |raw|
    CSV.new(raw, headers: true).map { |row| row.values_at("family", "profile", "graph", "seed") }.uniq
  end
  heuristic = options[:heuristic_only]
  name = heuristic ? "fork-heuristic.csv" : "branch-bound.csv"
  CSV.open(File.join(output, name), "w") do |summary|
    summary << FIELDS
    samples.each_with_index do |(family, profile, label, seed), index|
      stem = format("%s-%02d", heuristic ? "heuristic" : "search", index)
      if family == "llvm-regalloc"
        path = File.join(ROOT, "examples/regalloc", label)
        seconds, limit = 45, 1_000_000
        domains = File.readlines(path).filter_map do |line|
          Integer(line.split[1], 10) if line.start_with?("node ")
        end
      else
        match = label.match(/\AN(\d+)-D(\d+)\z/)
        raise "invalid synthetic input label: #{label}" unless match

        nodes, domain = match.captures.map { |value| Integer(value, 10) }
        domains, seconds, limit = Array.new(nodes, domain), 30, 2_000_000
        path = File.join(output, "#{stem}.pbqp")
        command = [generator, "--family", family, "--profile", profile, "--nodes", nodes.to_s,
                   "--domain-size", domain.to_s, "--seed", seed]
        text, diagnostics, status = invoke(command)
        raise "graph generation failed: #{diagnostics}" unless status.success?

        File.write(path, text)
      end
      trace = File.join(output, "#{stem}.jsonl")
      strategy = heuristic ? "heuristic-rn" : "exact-branch-reduce"
      command = ["timeout", "#{seconds}s", runner, "--solver", "local", "--strategy", strategy,
                 "--trace", trace]
      command += ["--maximum-search-nodes", limit.to_s, "--verbose"] unless heuristic
      command << path
      start = Process.clock_gettime(Process::CLOCK_MONOTONIC)
      stdout, diagnostics, process = invoke(command, filter_device: true)
      elapsed = Process.clock_gettime(Process::CLOCK_MONOTONIC) - start
      text = stdout + diagnostics
      File.write(File.join(output, "#{stem}.txt"), "command #{JSON.generate(command)}\n#{text}")
      status = if process.exitstatus == 124
                 "timeout"
               elsif !process.success?
                 raise "#{label}: exit #{process.exitstatus || process.termsig}: #{text}"
               elsif stdout.include?("status SEARCH_LIMIT")
                 "search-limit"
               else
                 raise "#{label}: unexpected solver failure: #{stdout}" if stdout.include?("status ")

                 "complete"
               end
      row = FIELDS.first(4).zip([family, profile, label, seed]).to_h
      row.merge!("input_sha256" => Digest::SHA256.file(path).hexdigest,
                 "status" => status, "wall_seconds" => format("%.6f", elapsed))
      match = text.match(/exact search nodes=(\d+) branches=(\d+) max-depth=(\d+) limit-hits=(\d+) pruned=(\d+)/)
      if match
        nodes, branches, depth, _, pruned = match.captures.map { |value| Integer(value, 10) }
        row.merge!("nodes" => nodes, "branches" => branches, "depth" => depth, "pruned" => pruned,
                   "dfs_open_bound" => 1 + depth * (domains.max - 1))
      elsif !heuristic && status == "complete"
        raise "#{label}: missing search counters"
      end
      forks, epochs, epoch, has_epoch = [], [], 0, false
      File.foreach(trace) do |line|
        begin
          event = JSON.parse(line)
        rescue JSON::ParserError
          raise unless status == "timeout"

          next # A timeout can truncate the final JSONL record.
        end
        forks << event.fetch("branch_domain") if event.fetch("type") == "BRANCH_SELECT"
        if event["type"] == "RN_SELECT" && has_epoch
          epochs << epoch
          epoch, has_epoch = 0, false
        end
        work = ELEMENTS.sum { |field| event.fetch(field, 0) }
        if work.positive? || STRUCTURAL.include?(event["type"])
          epoch += work
          has_epoch = true
        end
      end
      epochs << epoch if has_epoch
      row["model_c_median_elements"] = median(epochs) if heuristic
      row.merge!("forks" => forks.length, "branch_domain_min" => forks.min || 0,
                 "branch_domain_max" => forks.max || 0)
      Zlib::GzipWriter.open("#{trace}.gz") do |zipped|
        zipped.mtime = 0
        File.open(trace, "rb") { |raw| IO.copy_stream(raw, zipped) }
      end
      File.unlink(trace)
      row["optimum"] = stdout[/^optimum (-?\d+)$/, 1] || ""
      summary << FIELDS.map { |field| row[field] }
      summary.flush
      puts [index, family, label, status].join(" ")
      $stdout.flush
    end
  end
end

if $PROGRAM_NAME == __FILE__
  options = {}
  parser = OptionParser.new do |opts|
    opts.banner = "Usage: ruby recheck_search_report.rb --runner PATH --generator PATH --output DIR [--heuristic-only]"
    opts.on("--runner PATH") { |value| options[:runner] = value }
    opts.on("--generator PATH") { |value| options[:generator] = value }
    opts.on("--output DIR") { |value| options[:output] = value }
    opts.on("--heuristic-only") { options[:heuristic_only] = true }
  end
  parser.parse!
  abort parser.to_s unless ARGV.empty? && %i[runner generator output].all? { |key| options.key?(key) }
  collect(options)
end
