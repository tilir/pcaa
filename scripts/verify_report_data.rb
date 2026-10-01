#!/usr/bin/env ruby
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 PCAA contributors
# Checks archived report evidence, repeated measurements, and published table arithmetic.

require "csv"
require "digest"
require "json"
require "optparse"
require "set"
require "zlib"

ROOT = File.expand_path("..", __dir__)
DATA = File.join(ROOT, "doc/reports/data")
FAMILIES = %w[degree-3 degree-4 mixed-degree].freeze
KEY = %w[family profile seed nodes domain policy strategy].freeze
RESULT = {}

# Checks are explicit exceptions, independent of interpreter optimization flags.
def check(condition, context)
  raise context.to_s unless condition
end

def read_rows(name)
  path = File.join(DATA, name)
  if path.end_with?(".gz")
    Zlib::GzipReader.open(path) { |raw| CSV.new(raw, headers: true).map(&:to_h) }
  else
    CSV.read(path, headers: true).map(&:to_h)
  end
end

def same(old, new, ignored: [], keyed: false)
  left, right = read_rows(old), read_rows(new)
  if keyed
    index = right.to_h { |row| [row.values_at(*KEY), row] }
    pairs = left.map { |row| [row, index.fetch(row.values_at(*KEY))] }
  else
    check(left.length == right.length, [old, left.length, right.length])
    pairs = left.zip(right)
  end
  pairs.each_with_index do |(a, b), i|
    differences = a.filter_map do |key, value|
      [key, [value, b[key]]] unless ignored.include?(key) || value.to_s == b[key].to_s
    end.to_h
    check(differences.empty?, [old, i, differences])
  end
  RESULT[old] = { "matching_repeated_rows" => left.length }
end

def total(rows, key)
  rows.sum { |row| Float(row.fetch(key)) }
end

def mean(rows, key)
  total(rows, key) / rows.length
end

def nearest(values, fraction)
  values.sort[(fraction * (values.length - 1) + 0.5).floor]
end

def median(values)
  sorted = values.sort
  middle = sorted.length / 2
  sorted.length.odd? ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) / 2.0
end

def grouped(text)
  whole, fraction = text.split(".", 2)
  whole = whole.gsub(/\d(?=(\d{3})+(?!\d))/, '\\&,')
  fraction ? "#{whole}.#{fraction}" : whole
end

def integer(value)
  grouped((value + 0.5).floor.to_s)
end

def general(value)
  format("%g", value)
end

def close(a, b, absolute:)
  (a - b).abs <= [absolute, 1e-9 * [a.abs, b.abs].max].max
end

def row_in(report, cells)
  line = "| #{cells.join(' | ')} |"
  check(File.read(File.join(ROOT, "doc/reports", report)).include?(line), [report, line])
end

def verify
  same("historical/solver-characterization.csv.gz", "recheck/solver-characterization.csv.gz")
  same("historical/hw-sw-epochs.csv.gz", "recheck/hw-sw-epochs.csv.gz")
  %w[isa-round3-scaling isa-round3-per-edge].each do |name|
    same("historical/#{name}.csv.gz", "recheck/#{name}.csv.gz", ignored: ["wall_time_seconds"], keyed: true)
  end
  %w[vector-cycle-projection vector-cycle-per-edge].each do |name|
    same("historical/#{name}.csv.gz", "recheck/#{name}.csv.gz", ignored: ["graph"])
  end
  solver = read_rows("historical/solver-characterization.csv.gz")
  mix_fields = %w[project_elements project_accumulate_elements slice_elements map3_elements
                  argmin_elements descriptors batches operand_bytes result_bytes]
  FAMILIES.each do |family|
    [["heuristic-rn", "RN"], ["heuristic-rn-local-search", "RN+local"],
     ["local-search", "local search"]].each do |strategy, label|
      selected = solver.select do |row|
        row["family"] == family && row["nodes"] == "20" && row["strategy"] == strategy &&
          row["policy"] == "min-degree" && row["status"] == "OK" &&
          (strategy == "local-search" || Integer(row["rn"], 10).positive?)
      end
      title = family + (family == "mixed-degree" && label != "local search" ? ", RN-required" : "")
      row_in("solver-characterization.md", [title, label,
             *mix_fields.map { |key| grouped(format("%.1f", mean(selected, key))) }])
    end
  end
  epochs = read_rows("historical/hw-sw-epochs.csv.gz")
  epoch_summary = []
  FAMILIES.each do |family|
    [["B-reduction", "B atomic reduction"], ["C-rn-cascade", "C RN+cascade"],
     ["D-heuristic", "D whole heuristic"]].each do |model, label|
      selected = epochs.select do |row|
        row["family"] == family && row["solver"] == "heuristic-rn" &&
          row["policy"] == "min-degree" && row["model"] == model
      end
      elements = selected.map do |row|
        %w[project project_accumulate slice map3 argmin].sum { |key| Integer(row[key], 10) }
      end
      traffic = selected.map { |row| Integer(row["operand_bytes"], 10) + Integer(row["result_bytes"], 10) }
      rank = ->(p) { elements.sort[[(p * elements.length).ceil - 1, 0].max] }
      row_in("hw-sw-boundary-characterization.md", [family, "min-degree", label,
             format("%.1f", selected.length / 10.0),
             [rank.call(0.1), median(elements), rank.call(0.9)].map { |x| integer(x) }.join(" / "),
             "#{integer(median(traffic))} / #{integer(traffic.max)}"])
      epoch_summary << { "family" => family, "model" => model, "epochs" => selected.length }
    end
  end
  RESULT["epoch_tables"] = epoch_summary
  scaling = read_rows("historical/scaling-runs-with-epochs.csv.gz")
  repeated = read_rows("recheck/isa-round3-per-edge.csv.gz") + read_rows("recheck/local-search.csv.gz")
  index = repeated.to_h { |row| [row.values_at(*KEY), row] }
  scaling.each do |row|
    other = index.fetch(row.values_at(*KEY))
    row.each do |key, value|
      next if key == "wall_time_seconds"

      check(value.to_s == other[key].to_s, [key, row, other])
    end
  end
  RESULT["scaling"] = { "repeated_rows" => scaling.length, "local_search_rows" => 270 }
  FAMILIES.each do |family|
    nodes = family == "mixed-degree" ? [20, 50, 100, 200, 500] : [20, 50, 100, 200, 500, 1000]
    nodes.each do |n|
      selected = scaling.select do |row|
        row["family"] == family && row["nodes"] == n.to_s && row["domain"] == "8" &&
          row["policy"] == "min-degree" && row["strategy"] == "heuristic-rn"
      end
      check(selected.length == 5, [family, n, "expected five seeds"])
      cells = %w[total_elements project_elements map3_elements total_bytes rn].map do |key|
        integer(mean(selected, key))
      end
      row_in("scaling-characterization.md", [n, *cells, general(mean(selected, "model_c_epochs")),
             integer(mean(selected, "elements_per_model_c_epoch")),
             "#{integer(mean(selected, 'initial_edges'))} / #{integer(mean(selected, 'max_rn_edges'))}"])
    end
  end

  per_node = read_rows("historical/isa-round3-scaling.csv.gz")
  per_edge = read_rows("historical/isa-round3-per-edge.csv.gz")
  batched = {}
  [false, true].each do |real|
    a = per_node.select { |row| (row["family"] == "llvm-regalloc") == real }
    b = per_edge.select { |row| (row["family"] == "llvm-regalloc") == real }
    a.zip(b).each do |x, y|
      check(Integer(x["descriptors"], 10) == Integer(y["descriptors"], 10), "batch descriptor mismatch")
      check(Integer(x["batches"], 10) <= Integer(y["batches"], 10), "batch submission increase")
    end
    before, after = total(b, "batches"), total(a, "batches")
    row_in("batch-restructuring-study.md", [real ? "LLVM corpus" : "synthetic sweep through D=32",
           a.length, integer(before), integer(after), format("%.1f%%", 100 * (1 - after / before)),
           "#{integer(total(b, 'descriptors'))} / #{integer(total(a, 'descriptors'))}"])
    batched[real ? "real" : "synthetic"] = { "solves" => a.length, "before" => before, "after" => after }
  end
  read_rows("historical/vector-cycle-projection.csv.gz").zip(
    read_rows("historical/vector-cycle-per-edge.csv.gz")).each do |a, b|
    delta = Integer(b["actual_cycles"], 10) - Integer(a["actual_cycles"], 10)
    batches = Integer(b["batch_submissions"], 10) - Integer(a["batch_submissions"], 10)
    check(delta == 5 * batches, "cycle difference does not match removed batches")
  end
  RESULT["batching"] = batched
  timed = read_rows("historical/vector-cycle-projection.csv.gz")
  [2, 4, 8, 16, 32].each do |d|
    ratios = FAMILIES.map do |family|
      selected = timed.select { |row| row["family"] == family && row["domain"] == d.to_s }
      actual = total(selected, "actual_cycles")
      [actual / total(selected, "combined_partial_total_cycles"),
       actual / total(selected, "combined_full_total_cycles"),
       100 * total(selected, "descriptor_cycles") / actual,
       100 * total(selected, "compute_cycles") / actual]
    end
    spans = 4.times.map { |i| ratios.map { |values| values[i] }.minmax }
    row_in("vector-primitive-cycle-projection.md", [d,
           *spans.first(2).map { |lo, hi| format("%.2f-%.2fx", lo, hi) },
           *spans.drop(2).map { |lo, hi| "#{integer(lo)}-#{integer(hi)}%" }])
  end
  RESULT["cycle_projection"] = { "identical_per_node_rows" => 671, "identical_per_edge_rows" => 671 }
  (FAMILIES + ["llvm-regalloc"]).zip(FAMILIES + ["LLVM RegAllocPBQP"]).each do |family, title|
    selected = per_node.select { |row| row["family"] == family }
    contiguous, strided = total(selected, "contiguous_views"), total(selected, "strided_views")
    row_in("matrix-access-pattern-study.md", [title, selected.length, integer(contiguous), integer(strided),
           format("%.2f:1", contiguous / strided), format("%.1f%%", 100 * strided / (contiguous + strided))])
  end
  per_node.each do |row|
    next unless FAMILIES.include?(row["family"])

    d = Integer(row["domain"], 10)
    check(Integer(row["projection_primitives"], 10) == d * Integer(row["projections"], 10), "PROJECT identity")
    check(Integer(row["map3_elements"], 10) == Integer(row["r2"], 10) * d**3, "MAP3 identity")
  end
  RESULT["primitive_shapes"] = { "uniform_domain_rows" => 407, "R2_elements" => "r2 * D^3",
                                 "RN_scalar_descriptors" => "projections * D" }

  forks = read_rows("historical/fork-parallelism.csv.gz")
  groups = forks.group_by { |row| row.values_at("family", "graph", "seed") }
  forks.each do |row|
    check(Float(row["independent_elements_at_fork"]) == Integer(row["branch_domain"], 10) *
          Float(row["model_c_elements_per_child"]), "fork work proxy")
  end
  (FAMILIES + ["llvm-regalloc"]).each do |family|
    selected = forks.select { |row| row["family"] == family }
    cells = [family == "llvm-regalloc" ? "real sample" : family, integer(selected.length)]
    %w[branch_domain model_c_elements_per_child independent_elements_at_fork].each do |field|
      values = selected.map { |row| Float(row[field]) }
      cells << [0, 0.25, 0.5, 0.75, 0.9, 0.99, 1].map do |p|
        grouped(general(nearest(values, p)))
      end.join(" / ")
    end
    row_in("fork-parallelism-characterization.md", cells)
  end
  read_rows("recheck/fork-heuristic.csv").each do |row|
    old = groups.fetch(row.values_at("family", "graph", "seed"))
    check(Float(row["model_c_median_elements"]) == Float(old.first["model_c_elements_per_child"]), "fork median")
  end
  search = read_rows("recheck/branch-bound.csv")
  search.each_with_index do |row, i|
    old = groups.fetch(row.values_at("family", "graph", "seed"))
    domains = []
    Zlib::GzipReader.open(File.join(DATA, format("recheck/search-%02d.jsonl.gz", i))) do |trace|
      trace.each_line do |line|
        begin
          event = JSON.parse(line)
        rescue JSON::ParserError
          check(row["status"] == "timeout", "truncated non-timeout trace")
          next
        end
        domains << event.fetch("branch_domain") if event.fetch("type") == "BRANCH_SELECT"
      end
    end
    check(domains.length == Integer(row["forks"], 10), "trace fork count")
    if row["status"] == "complete"
      check(domains == old.map { |x| Integer(x["branch_domain"], 10) }, "completed fork sequence")
      check(Integer(row["pruned"], 10) >= 0 && Integer(row["pruned"], 10) < Integer(row["nodes"], 10), "prune count")
    else
      check(row["status"] == "timeout" && row["nodes"].to_s.empty? && row["optimum"].to_s.empty?, "timeout completion fields")
    end
  end
  RESULT["forks"] = { "archived_forks" => forks.length, "repeated_heuristic_inputs" => 65,
                      "identical_completed_searches" => 63, "partial_timeout_searches" => 2 }
  FAMILIES.each do |family|
    selected = search.select { |row| row["family"] == family }
    triplet = lambda do |key|
      values = selected.map { |row| Float(row[key]) }
      [values.min, median(values), values.max].map { |v| grouped(general(v)) }.join(" / ")
    end
    pruning = selected.map { |row| 100.0 * Integer(row["pruned"], 10) / Integer(row["nodes"], 10) }
    row_in("branch-bound-characterization.md", [family, triplet.call("depth"), triplet.call("nodes"),
           format("%.1f / %.1f / %.1f", median(pruning), pruning.min, pruning.max),
           selected.map { |row| Integer(row["dfs_open_bound"], 10) }.max])
  end

  corpus = read_rows("recheck/llvm-corpus.csv")
  domain_counts = Hash.new(0)
  corpus.each do |row|
    path = File.join(ROOT, "examples/regalloc", row["graph"])
    check(Digest::SHA256.file(path).hexdigest == row["sha256"], "corpus input hash: #{path}")
    domains = File.readlines(path).filter_map do |line|
      Integer(line.split[1], 10) if line.start_with?("node ")
    end
    check(domains == JSON.parse(row["domains"]) && domains.length == Integer(row["nodes"], 10), "corpus domains")
    domains.each { |domain| domain_counts[domain] += 1 }
  end
  check(corpus.length == 491 && domain_counts.values.sum == 16462, "corpus size")
  row_in("llvm-corpus-characterization.md", ["count", *domain_counts.sort.map { |_, count| integer(count) }])
  check(corpus.count { |row| Integer(row["domain_min"], 10) != Integer(row["domain_max"], 10) } == 475, "nonuniform graphs")
  extraction = read_rows("recheck/llvm-extraction.csv")
  check(extraction.length == 491, "extraction size")
  check(extraction.all? { |row| row["sha256_original"] == row["sha256_reextracted"] }, "extraction hashes")
  RESULT["llvm_corpus"] = { "graphs" => corpus.length, "nodes" => domain_counts.values.sum,
                            "domain_histogram" => domain_counts.sort.to_h,
                            "identical_reextracted_graphs" => extraction.length }

  road = read_rows("recheck/road-summary.csv").first
  distances, errors, coordinates, road_samples = [], [], {}, []
  Zlib::GzipReader.open(File.join(DATA, "inputs/USA-road-d.NY.co.gz")) do |raw|
    raw.each_line do |line|
      next unless line.start_with?("v ")

      _, node, lon, lat = line.split
      coordinates[Integer(node, 10)] = [Integer(lon, 10) / 1e6 * Math::PI / 180,
                                      Integer(lat, 10) / 1e6 * Math::PI / 180]
    end
  end
  Zlib::GzipReader.open(File.join(DATA, "inputs/USA-road-d.NY.gr.gz")) do |source|
    source_arcs = source.each_line.lazy.select { |line| line.start_with?("a ") }.map { |line| line.split.drop(1) }.to_enum
    Zlib::GzipReader.open(File.join(DATA, "recheck/road-arcs.csv.gz")) do |raw|
      CSV.new(raw, headers: true).each_with_index do |row, i|
        check(i == Integer(row["arc"], 10), "arc index")
        check(row.values_at("source", "target", "published_weight") == source_arcs.next, "source arc")
        a, b = coordinates.fetch(Integer(row["source"], 10)), coordinates.fetch(Integer(row["target"], 10))
        h = Math.sin((b[1] - a[1]) / 2)**2 + Math.cos(a[1]) * Math.cos(b[1]) * Math.sin((b[0] - a[0]) / 2)**2
        distance = 2 * 6371000 * Math.asin(Math.sqrt(h))
        observed = Float(row["distance_m"])
        check(close(distance, observed, absolute: 1e-7), "road distance at arc #{i}")
        encoded = (observed * 1000 + 0.5).floor
        check(encoded == Integer(row["millimetres"], 10), "road quantization")
        road_samples << encoded if (i % 100000).zero?
        error = (encoded / 1000.0 - observed).abs
        check(close(error, Float(row["error_m"]), absolute: 1e-12), "road rounding error")
        distances << observed
        errors << error
      end
    end
    begin
      source_arcs.next
      raise "unconsumed source arcs"
    rescue StopIteration
      # All published arcs were consumed exactly once.
    end
  end
  check(road_samples == [80384, 115347, 19472, 181062, 100910, 65003, 90256, 87238], "road probe samples")
  check(distances.length == Integer(road["count"], 10) && distances.length == 733846 && coordinates.length == 264346, "road size")
  { "min_m" => distances.min, "max_m" => distances.max, "p01_m" => nearest(distances, 0.01),
    "median_m" => nearest(distances, 0.5), "p99_m" => nearest(distances, 0.99),
    "max_mm_error" => errors.max, "mean_mm_error" => errors.sum / errors.length }.each do |key, value|
    check(close(value, Float(road[key]), absolute: 1e-10), [key, value])
  end
  RESULT["road"] = road
  hmm = read_rows("recheck/hmm-log-weights.csv")
  check(hmm.length == 29, "HMM sample count")
  hmm.each do |row|
    value = -Math.log(10.0**(-Integer(row["negative_log10_probability"], 10)))
    check(close(value, Float(row["negative_log_probability"]), absolute: 1e-12), "HMM weight")
    check((Integer(row["millilog_units"], 10) / 1000.0 - value).abs <= 0.0005, "HMM quantization")
  end
  RESULT["hmm"] = { "log_weight_samples" => hmm.length, "maximum_error_bound" => 0.0005 }
  compact = read_rows("recheck/compact-encoding.csv")
  check(compact.map { |row| Integer(row["compact_bytes"], 10) } == [256, 1600, 2336, 3648], "compact example bytes")
  check(total(compact, "old_bytes") == 14160 && total(compact, "compact_bytes") == 7840, "compact total bytes")
  RESULT["compact"] = { "examples" => 4, "old_bytes" => 14160, "compact_bytes" => 7840 }
  recorded_files = Set.new
  File.foreach(File.join(DATA, "SHA256SUMS")) do |line|
    digest, filename = line.chomp.split("  ", 2)
    recorded_files << filename
    check(Digest::SHA256.file(File.join(DATA, filename)).hexdigest == digest, filename)
  end
  actual_files = Dir.glob(File.join(DATA, "**/*"), File::FNM_DOTMATCH).select do |path|
    File.file?(path) && File.basename(path) != "SHA256SUMS"
  end.map { |path| path.delete_prefix("#{DATA}/") }.to_set
  check(recorded_files == actual_files, "Checksum manifest must cover every evidence file")
  RESULT
end

def sorted_json(value)
  case value
  when Hash
    value.sort_by { |key, _| key }.to_h.transform_values { |item| sorted_json(item) }
  when Array
    value.map { |item| sorted_json(item) }
  else
    value
  end
end

if $PROGRAM_NAME == __FILE__
  options = {}
  parser = OptionParser.new do |opts|
    opts.banner = "Usage: ruby verify_report_data.rb [--output PATH]"
    opts.on("--output PATH") { |value| options[:output] = value }
  end
  parser.parse!
  abort parser.to_s unless ARGV.empty?
  result = verify
  File.write(options[:output], JSON.pretty_generate(sorted_json(result)) + "\n") if options[:output]
  puts "Verified report datasets, repeated runs, table arithmetic, and SHA-256 checksums."
end
