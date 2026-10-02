#!/usr/bin/env ruby
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 PCAA contributors
# Checks L2 evidence invariants and regenerates tables and flattened CSVs.

require "csv"
require "digest"
require "fileutils"
require "json"
require "optparse"
require "pathname"
require "rubygems/package"
require "set"
require "zlib"

ROOT = Pathname.new(__dir__).parent.expand_path
DATA = ROOT.join("doc/reports/data/l2")
PHASES = %w[descriptor decode protection operand add1 add2 tree merge writeback drain memory_wait control].freeze
PARAMS = %w[lanes mem_bytes tm tn memory_latency].freeze
BASE = [4, 16, 8, 16, 1].freeze
SHAPE_FIELDS = %w[opcode n m layout batch_count].freeze
NON_ADDITIVE = (PARAMS + %w[cycle_ns acceptance_delay outstanding_limit max_outstanding max_writebacks]).freeze
CORPUS_SIZE = 491
COST_BYTES = 4
PAIR_BYTES = 8


def check(condition, context)
  raise context unless condition
end

def read_rows(name)
  Zlib::GzipReader.open(DATA.join(name)) { |stream| stream.each_line.map { |line| JSON.parse(line) } }
end

def key(config)
  config.values_at(*PARAMS)
end

def totals(rows)
  result = Hash.new(0)
  rows.each do |row|
    row.fetch("l2").each do |field, value|
      result[field] += value if value.is_a?(Integer) && !NON_ADDITIVE.include?(field)
    end
    %w[max_outstanding max_writebacks].each do |field|
      result[field] = [result[field], row.fetch("l2").fetch(field)].max
    end
  end
  result
end

# Retain the historical fingerprint encoding: sorted flat hash, spaces after
# separators. This verifies the archived source identity without rewriting it.
def fingerprint(metadata)
  sources = metadata.fetch("implementation_sha256").sort.map do |name, digest|
    "#{JSON.generate(name)}: #{JSON.generate(digest)}"
  end.join(", ")
  Digest::SHA256.hexdigest(metadata.fetch("base_commit") + "{#{sources}}")
end

def grouped(value)
  value.to_s.reverse.scan(/.{1,3}/).join(",").reverse
end

def share(value, total)
  format("%.2f%%", 100 * value.fdiv(total))
end

def table(headers, rows)
  (["| #{headers.join(' | ')} |", "| #{(['---'] * headers.length).join(' | ')} |"] +
   rows.map { |row| "| #{row.join(' | ')} |" }).join("\n")
end

def flatten(path, rows)
  Zlib::GzipWriter.open(path) do |stream|
    csv = CSV.new(stream, row_sep: "\r\n")
    fields = nil
    rows.each do |row|
      flat = row.reject { |_, value| value.is_a?(Hash) || value.is_a?(Array) }.merge(row.fetch("l2"))
      row.fetch("l1", {}).each { |name, value| flat["l1_#{name}"] = value }
      unless fields
        fields = flat.keys
        csv << fields
      end
      csv << fields.map do |field|
        value = flat[field]
        # Match the existing CSV's booleans for byte-identical regeneration.
        value == true ? "True" : value == false ? "False" : value
      end
    end
  end
end

def corpus_rows(workloads)
  workloads.select { |row| key(row.fetch("config")) == BASE && row.fetch("input").start_with?("examples/regalloc/") }
end

def verify(metadata, micro, workloads)
  check(metadata.fetch("failures").zero?, "failed measurements")
  hashes = metadata.fetch("implementation_sha256")
  seen = Set.new
  Zlib::GzipReader.open(DATA.join("implementation.tar.gz")) do |compressed|
    Gem::Package::TarReader.new(compressed) do |archive|
      archive.each do |entry|
        next unless hashes.key?(entry.full_name)
        check(Digest::SHA256.hexdigest(entry.read) == hashes.fetch(entry.full_name),
              "implementation snapshot #{entry.full_name}")
        seen << entry.full_name
      end
    end
  end
  check(seen == hashes.keys.to_set, "source snapshot coverage")
  metadata.fetch("input_sha256").each do |path, digest|
    check(Digest::SHA256.file(ROOT.join(path)).hexdigest == digest, "input #{path}")
  end
  configs = metadata.fetch("configurations").map { |config| key(config) }.to_set
  check(configs == micro.map { |row| key(row.fetch("l2")) }.to_set, "microbench configuration coverage")
  check(configs == workloads.map { |row| key(row.fetch("config")) }.to_set, "workload configuration coverage")
  (micro + workloads).each do |row|
    check(row.fetch("status", "ok") == "ok" && row.fetch("verified"), "unverified row")
    s = row.fetch("l2")
    check(s.fetch("cycles") == PHASES.sum { |phase| s.fetch("#{phase}_cycles") }, "cycle partition")
    check(s.fetch("requests") == s.values_at("descriptor_requests", "operand_requests", "result_requests").sum,
          "request partition")
    check(s.fetch("transferred_bytes") == s.values_at("descriptor_bytes", "operand_bytes", "result_bytes").sum,
          "byte partition")
    check(s.fetch("active_elements") + s.fetch("tail_slots") == s.fetch("lane_slots"), "lane partition")
    check(s.fetch("max_outstanding") <= 1 && s.fetch("max_writebacks") <= 1, "queue bounds")
    check(s.fetch("descriptor_cycles") == 2 * s.fetch("descriptor_requests"), "descriptor handshakes")
    check(s.fetch("operand_cycles") == 2 * s.fetch("operand_requests"), "operand handshakes")
    check(s.fetch("writeback_cycles") == 2 * s.fetch("result_requests"), "write handshakes")
    check(s.fetch("memory_wait_cycles") == s.fetch("requests") *
          (s.fetch("memory_latency") + s.fetch("acceptance_delay")), "response wait")
    check(s.fetch("header_requests") + s.fetch("body_requests") == s.fetch("descriptor_requests"), "header/body requests")
    check(s.fetch("decode_cycles") == 2 * (s.fetch("submissions") + s.fetch("children")), "header/body decode")
    check(s.fetch("child_barriers") == s.fetch("children"), "child drain")
  end
  micro.each do |row|
    s = row.fetch("l2")
    op, n, m, batch = row.values_at("opcode", "n", "m", "batch_count")
    count = batch.zero? ? 1 : batch
    tiles = op >= 7 ? (m + s.fetch("tm") - 1) / s.fetch("tm") : 1
    shared = op == 7 ? 1 : 2
    operand = (op >= 7 ? COST_BYTES * (m * n + shared * n * tiles) :
               COST_BYTES * n * ([2, 4].include?(op) ? 3 : 2)) * count
    result_width = case op
                   when 8 then m * PAIR_BYTES
                   when 7 then m * COST_BYTES
                   when 6 then n * COST_BYTES
                   when 3, 4 then PAIR_BYTES
                   else COST_BYTES
                   end
    descriptor_width = if op == 8
                         64
                       elsif [2, 4, 7].include?(op) || (op == 6 && row.fetch("layout") != 4)
                         48
                       else
                         32
                       end
    result = result_width * count + (batch.zero? ? 0 : PAIR_BYTES)
    descriptor = descriptor_width * count + (batch.zero? ? 0 : 32)
    check(s.fetch("operand_bytes") == operand, "MAS scheduled payload")
    check(s.fetch("result_bytes") == result && s.fetch("descriptor_bytes") == descriptor, "result/descriptor payload")
    check(s.fetch("shared_reread_bytes") == (op >= 7 ? COST_BYTES * shared * n * (tiles - 1) * count : 0), "shared rereads")
  end
  corpus = corpus_rows(workloads)
  check(corpus.length == CORPUS_SIZE, "full corpus baseline")
  check(corpus.map { |row| row.fetch("input") }.uniq.length == CORPUS_SIZE, "duplicate corpus input")
  shapes = micro.group_by { |row| key(row.fetch("l2")) }.transform_values do |rows|
    rows.map { |row| row.values_at(*SHAPE_FIELDS) }.to_set
  end
  configs.each { |config| check(shapes.fetch(config) == shapes.fetch(BASE), "changed shape set in sweep") }
  %w[runs.jsonl.gz workload-runs.jsonl.gz].each do |name|
    Zlib::GzipReader.open(DATA.join(name)) do |stream|
      stream.each_line { |line| check(JSON.parse(line).fetch("status") == "ok", "failed raw invocation") }
    end
  end
  { "microbench_rows" => micro.length, "workload_rows" => workloads.length,
    "configurations" => configs.length, "full_corpus_rows" => corpus.length, "failed_rows" => 0 }
end

def verify_derived(metadata, micro, workloads, checked)
  summary = JSON.parse(DATA.join("summary.json").read)
  stats = totals(corpus_rows(workloads))
  check(summary.fetch("verification") == checked, "derived verification counts")
  check(summary.fetch("corpus_totals") == stats, "derived corpus totals")
  identity = fingerprint(metadata)
  check(summary.fetch("implementation_fingerprint") == identity, "derived source fingerprint")
  report = ROOT.join("doc/reports/l2-microarchitecture-characterization.md").read
  check(report.include?(identity), "report source fingerprint")
  PHASES.each do |phase|
    row = "| #{phase} | #{grouped(stats.fetch("#{phase}_cycles"))} | #{share(stats.fetch("#{phase}_cycles"), stats.fetch('cycles'))} |"
    check(report.include?(row), "report phase table #{phase}")
  end
  puts "Verified L2 source/input hashes, raw completion, cycle/request/byte partitions, MAS payloads and shape coverage: #{checked}"
end

def generate(metadata, micro, workloads, checked, output)
  FileUtils.mkdir_p(output)
  corpus = corpus_rows(workloads)
  s = totals(corpus)
  l1 = corpus.sum { |row| row.fetch("l1").fetch("cycles") }
  lines = ["# Generated L2 tables", "", table(%w[Evidence Count], checked.to_a), "",
           "## Full LLVM baseline phase partition", "",
           table(%w[Phase Cycles Share], PHASES.map do |phase|
             [phase, grouped(s.fetch("#{phase}_cycles")), share(s.fetch("#{phase}_cycles"), s.fetch("cycles"))]
           end + [["Total", grouped(s.fetch("cycles")), "100%"]]), ""]
  unattributed = s.fetch("cycles") - (1..8).sum { |op| s.fetch("opcode#{op}_cycles") }
  lines += ["## Opcode attribution", "", table(["Opcode", "Primitives", "Cycles", "Share of total"],
    [3, 6, 7, 8].map do |op|
      [op, grouped(s.fetch("opcode#{op}_count")), grouped(s.fetch("opcode#{op}_cycles")),
       share(s.fetch("opcode#{op}_cycles"), s.fetch("cycles"))]
    end + [["Batch parent/record", grouped(s.fetch("batches")), grouped(s.fetch("opcode5_cycles")),
            share(s.fetch("opcode5_cycles"), s.fetch("cycles"))],
           ["Unattributed header/control", "—", grouped(unattributed), share(unattributed, s.fetch("cycles"))]]), "",
    "## Memory composition", "", table(["Role", "Payload / transferred bytes", "Requests"],
    %w[descriptor operand result].map do |role|
      [role.capitalize, grouped(s.fetch("#{role}_bytes")), grouped(s.fetch("#{role}_requests"))]
    end + [["Total", grouped(s.fetch("transferred_bytes")), grouped(s.fetch("requests"))]]), "",
    table(%w[Activity Count], %w[header_requests body_requests lane_slots active_elements tail_slots shared_loads
      shared_reread_bytes contiguous_transfers gather_elements split_transfers protection_elements child_barriers].map do |field|
      [field, grouped(s.fetch(field))]
    end), "", "## Example and synthetic L1 comparison", "",
    table(["Input", "L1 cycles", "L2 cycles", "L2 / L1"], workloads.filter_map do |row|
      next unless key(row.fetch("config")) == BASE && !row.fetch("input").start_with?("examples/regalloc/")
      [File.basename(row.fetch("input")), row.fetch("l1").fetch("cycles"), row.fetch("l2").fetch("cycles"),
       format("%.3f", row.fetch("l2").fetch("cycles").fdiv(row.fetch("l1").fetch("cycles")))]
    end + [["Full LLVM corpus", grouped(l1), grouped(s.fetch("cycles")), format("%.3f", s.fetch("cycles").fdiv(l1))]]), ""]
  baseline = workloads.select { |row| key(row.fetch("config")) == BASE }.to_h { |row| [row.fetch("input"), row] }
  workload_groups = workloads.group_by { |row| key(row.fetch("config")) }
  sensitivity = metadata.fetch("configurations").map do |config|
    group = workload_groups.fetch(key(config))
    if key(config) == BASE
      sample = metadata.fetch("subset") + group.filter_map do |row|
        path = row.fetch("input")
        path if !path.start_with?("examples/regalloc/") &&
          (path.start_with?("examples/") || path.include?("-d4.") || path.include?("-d16."))
      end
      group = sample.map { |identity| baseline.fetch(identity) }
    end
    elapsed = group.sum { |row| row.fetch("l2").fetch("cycles") }
    original = group.sum { |row| baseline.fetch(row.fetch("input")).fetch("l2").fetch("cycles") }
    { "config" => config, "rows" => group.length, "cycles" => elapsed,
      "baseline_cycles" => original, "ratio" => elapsed.fdiv(original) }
  end
  lines += ["## Controlled workload sensitivity (same 18 inputs)", "",
    table(PARAMS + ["Cycles", "Ratio to baseline"], sensitivity.map do |row|
      key(row.fetch("config")) + [grouped(row.fetch("cycles")), format("%.4f", row.fetch("ratio"))]
    end), ""]
  shapes = micro.to_h { |row| [[key(row.fetch("l2")), row.values_at(*SHAPE_FIELDS)], row] }
  lines += ["## Direct projection parameter sensitivity (opcode 7, m=17, n=31)", "",
    table(PARAMS + ["Cycles", "Operand bytes", "Requests"], metadata.fetch("configurations").map do |config|
      row = shapes.fetch([key(config), [7, 31, 17, 0, 0]])
      key(config) + row.fetch("l2").values_at("cycles", "operand_bytes", "requests")
    end), "", "## Shape and layout samples", "",
    table(["Opcode", "n", "m", "Layout", "Batch", "L1", "L2", "Requests", "Operand bytes",
           "Shared reread bytes", "Lane utilization"], micro.filter_map do |row|
      next unless key(row.fetch("l2")) == BASE &&
        ((row.fetch("n") == 31 && [1, 8, 9, 17, 32].include?(row.fetch("m")) && [7, 8].include?(row.fetch("opcode"))) ||
         (row.fetch("n") == 17 && (row.fetch("batch_count").positive? || [3, 6].include?(row.fetch("opcode")) ||
                                  (row.fetch("opcode") >= 7 && row.fetch("m") == 9))))
      stats = row.fetch("l2")
      row.values_at(*SHAPE_FIELDS) + [row.fetch("l1_cycles")] +
        stats.values_at("cycles", "requests", "operand_bytes", "shared_reread_bytes") +
        [share(stats.fetch("active_elements"), stats.fetch("lane_slots"))]
    end), ""]
  summary = { "verification" => checked, "corpus_totals" => s, "corpus_l1_cycles" => l1,
              "sensitivity" => sensitivity, "implementation_fingerprint" => fingerprint(metadata) }
  output.join("summary.json").write(JSON.pretty_generate(summary) + "\n")
  output.join("tables.md").write(lines.join("\n"))
  flatten(output.join("microbench.csv.gz"), micro)
  flatten(output.join("workloads.csv.gz"), workloads)
  puts JSON.pretty_generate(summary)
end

if $PROGRAM_NAME == __FILE__
  options = { verify: false, output: DATA }
  OptionParser.new do |parser|
    parser.banner = "Usage: ruby scripts/l2_report.rb [--verify] [--output PATH]"
    parser.on("--verify") { options[:verify] = true }
    parser.on("--output PATH") { |value| options[:output] = ROOT.join(value).cleanpath }
  end.parse!
  abort "unexpected arguments: #{ARGV.join(' ')}" unless ARGV.empty?
  metadata = JSON.parse(DATA.join("metadata.json").read)
  micro = read_rows("microbench.jsonl.gz")
  workloads = read_rows("workloads.jsonl.gz")
  checked = verify(metadata, micro, workloads)
  if options[:verify]
    verify_derived(metadata, micro, workloads, checked)
  else
    generate(metadata, micro, workloads, checked, options[:output])
  end
end
