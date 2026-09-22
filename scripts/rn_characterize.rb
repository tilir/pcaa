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
            seeds: DEFAULT_SEEDS.to_a, output: nil, summary: nil, exact_limit: 100_000 }
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
  parser.on("--summary PATH", "Write Markdown aggregate summary to PATH") do |value|
    options[:summary] = value
  end
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

def cascade_metrics(text)
  match = text.match(
    /rn_cascades rn_episodes=(\d+) rn_cascade_r0_total=(\d+) rn_cascade_r1_total=(\d+) rn_cascade_r2_total=(\d+) rn_cascade_exact_total=(\d+) rn_cascade_mean=([0-9.]+) rn_cascade_max=(\d+)/)
  abort "missing RN cascade statistics in runner diagnostics" unless match
  match.captures.map.with_index { |value, index| index == 5 ? value.to_f : value.to_i }
end

def cascade_histogram(text)
  match = text.match(/^pcaa: rn_cascade_histogram (.+)$/)
  abort "missing RN cascade histogram in runner diagnostics" unless match
  values = Array.new(65, 0)
  match[1].split.each do |field|
    name, value = field.split("=", 2)
    next unless name.start_with?("cascade_len_")

    values[Integer(name.delete_prefix("cascade_len_"), 10)] = Integer(value, 10)
  end
  values
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
  result[:cascades] = cascade_metrics(diagnostics)
  result[:cascade_histogram] = cascade_histogram(diagnostics)
  result[:condition] = metrics(diagnostics,
    /condition traffic operations=(\d+) elements=(\d+) matrix-read=(\d+) unary-read=(\d+) unary-write=(\d+)/,
    "conditioning traffic")
  result[:local] = metrics(diagnostics,
    /local search evaluations=(\d+) sweeps=(\d+) accepted-moves=(\d+) slices=(\d+) slice-elements=(\d+) argmin=(\d+)/,
    "local-search statistics")
  result[:operation_mix] = metrics(diagnostics,
    /operation mix project-elements=(\d+) project-accumulate-elements=(\d+) slice-elements=(\d+) map3-elements=(\d+) argmin-elements=(\d+) descriptors=(\d+) batches=(\d+) operand-bytes=(\d+) result-bytes=(\d+) bytes=(\d+)/,
    "operation mix")
  result[:views] = metrics(diagnostics,
    /views contiguous=(\d+) strided=(\d+)/,
    "view statistics")
  result[:search] = metrics(diagnostics,
    /exact search nodes=(\d+) branches=(\d+) max-depth=(\d+) limit-hits=(\d+)/,
    "search statistics")
  result
end

header = %w[family profile nodes seed strategy policy status objective exact_objective gap
            r0 r1 r2 rn projections projection_primitives rn_commit_elements
            rn_episodes rn_cascade_r0_total rn_cascade_r1_total rn_cascade_r2_total
            rn_cascade_exact_total rn_cascade_mean rn_cascade_max
            condition_operations condition_elements condition_matrix_read condition_unary_read condition_unary_write
            local_evaluations local_sweeps local_moves local_slices local_slice_elements local_argmin
            project_elements project_accumulate_elements slice_elements map3_elements argmin_elements descriptors
            batches operand_bytes result_bytes bytes contiguous_views strided_views
            search_nodes search_branches search_depth search_limit_hits]
header.concat((0..64).map { |length| "cascade_len_#{length}" })
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
          cascades = result[:cascades] || Array.new(7)
          cascade_histogram = result[:cascade_histogram] || Array.new(65)
          condition = result[:condition] || Array.new(5)
          local = result[:local] || Array.new(6)
          operation_mix = result[:operation_mix] || Array.new(10)
          views = result[:views] || Array.new(2)
          search = result[:search] || Array.new(4)
          exact_objective = exact_result && exact_result[:status] == "OK" ? exact_result[:objective] : nil
          gap = result[:objective] && exact_objective ? result[:objective] - exact_objective : nil
          rows << [family, profile, nodes, seed, strategy, policy, result[:status], result[:objective],
                   exact_objective, gap, *reductions, *cascades, *condition, *local, *operation_mix,
                   *views,
                   *search, *cascade_histogram]
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

def mean(rows, field)
  rows.sum { |row| row.fetch(field).to_f } / rows.length
end

def median(values)
  sorted = values.sort
  middle = sorted.length / 2
  sorted.length.odd? ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) / 2.0
end

def percent(part, whole)
  whole.zero? ? 0.0 : 100.0 * part / whole
end

def format_number(value)
  format("%.1f", value)
end

def write_summary(path, header, rows)
  records = rows.map { |row| header.zip(row).to_h }
  File.open(path, "w") do |file|
    file.puts "# Generated Solver Characterization Summary"
    file.puts
    file.puts "## RN Cascade Characterization"
    file.puts
    file.puts "Only successful runs with at least one RN episode contribute to this table."
    file.puts
    file.puts "| Family | Solver / policy | runs | RN decisions / solve | exact reductions / episode (mean / median / max) | zero / one / multiple episodes | R0 / R1 / R2 cascade composition |"
    file.puts "|---|---|---:|---:|---:|---:|---:|"
    %w[degree-3 degree-4 mixed-degree].each do |family|
      %w[heuristic-rn heuristic-rn-local-search].each do |strategy|
        RN_POLICIES.each do |policy|
          selected = records.select do |row|
            row["family"] == family && row["strategy"] == strategy && row["policy"] == policy &&
              row["nodes"].to_i == 20 && row["status"] == "OK" && row["rn_episodes"].to_i > 0
          end
          next if selected.empty?

          lengths = selected.flat_map do |row|
            (0..64).flat_map { |length| [length] * row.fetch("cascade_len_#{length}").to_i }
          end
          episodes = lengths.length
          total = lengths.sum
          r0 = selected.sum { |row| row["rn_cascade_r0_total"].to_i }
          r1 = selected.sum { |row| row["rn_cascade_r1_total"].to_i }
          r2 = selected.sum { |row| row["rn_cascade_r2_total"].to_i }
          composition = total.zero? ? "0 / 0 / 0" :
            "#{format_number(percent(r0, total))}% / #{format_number(percent(r1, total))}% / #{format_number(percent(r2, total))}%"
          distribution = "#{format_number(percent(lengths.count(0), episodes))}% / " \
                         "#{format_number(percent(lengths.count(1), episodes))}% / " \
                         "#{format_number(percent(lengths.count { |length| length > 1 }, episodes))}%"
          file.puts "| #{family} | #{strategy}, #{policy} | #{selected.length} | " \
                    "#{format_number(mean(selected, 'rn_episodes'))} | " \
                    "#{format_number(total.to_f / episodes)} / #{format_number(median(lengths))} / #{lengths.max} | " \
                    "#{distribution} | #{composition} |"
        end
      end
    end
    file.puts
    file.puts "## Operation Mix by Graph Family"
    file.puts
    file.puts "Values are means per successful 20-node run. Operation elements describe shapes of " \
              "work, not equal hardware cost; no cycle or area percentage is implied."
    file.puts
    file.puts "| Family | Solver / policy | PROJECT | PROJECT_ACC | SLICE | MAP3 | ARGMIN | descriptors | batches | operand bytes | result bytes |"
    file.puts "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|"
    selections = [["heuristic-rn", "min-degree"], ["heuristic-rn-local-search", "min-degree"],
                  ["local-search", "min-degree"]]
    %w[degree-3 degree-4 mixed-degree].each do |family|
      selections.each do |strategy, policy|
        selected = records.select do |row|
          row["family"] == family && row["strategy"] == strategy && row["policy"] == policy &&
            row["nodes"].to_i == 20 && row["status"] == "OK"
        end
        next if selected.empty?

        label = family
        if family == "mixed-degree" && strategy != "local-search"
          rn_selected = selected.select { |row| row["rn_episodes"].to_i > 0 }
          selected = rn_selected unless rn_selected.empty?
          label = "mixed-degree RN-required"
        end
        fields = %w[project_elements project_accumulate_elements slice_elements map3_elements
                    argmin_elements descriptors batches operand_bytes result_bytes]
        values = fields.map { |field| format_number(mean(selected, field)) }
        file.puts "| #{label} | #{strategy}, #{policy} | #{values.join(' | ')} |"
      end
    end
  end
end

write_summary(options[:summary], header, rows) if options[:summary]

irreducible = rows.count { |row| row[4] == "reduce-only" && row[6] == "IRREDUCIBLE" }
warn "rows=#{rows.size} reduce-only-irreducible=#{irreducible}"
