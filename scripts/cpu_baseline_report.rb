#!/usr/bin/env ruby
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 PCAA contributors
# Rebuilds CPU/cycle comparisons, sensitivity and held-out selective-offload tables.
require "csv"
require "digest"
require "fileutils"
require "json"
require "open3"
require "optparse"
require "pathname"
require "zlib"

module CpuBaselineReport
  ROOT = Pathname.new(__dir__).parent.expand_path
  VARIANTS = %w[current_original current scalar dense structured].freeze
  COMPONENTS = %w[other topology selection reduction reconstruction kernel metadata].freeze
  CLASSES = %w[matching forbidden_rows row_exceptions dense].freeze
  FREQUENCIES = [250, 500, 1000, 2000].freeze
  module_function

  def median(values)
    raise "empty samples" if values.empty?
    sorted = values.sort
    sorted[sorted.length / 2].to_f
  end

  def percentile(values, p)
    return 0.0 if values.empty?
    sorted = values.sort
    sorted[[(p * sorted.length).ceil - 1, 0].max].to_f
  end

  def read_gzip(path)
    Zlib::GzipReader.open(path) { |stream| stream.each_line.map { |line| JSON.parse(line) } }
  end

  def csv_text(rows)
    return "" if rows.empty?
    columns = rows.flat_map(&:keys).uniq
    CSV.generate do |csv|
      csv << columns
      rows.each { |row| csv << columns.map { |key| row[key] } }
    end
  end

  def table(headings, rows)
    ([headings, headings.map { "---" }] + rows).map { |row| "| #{row.join(' | ')} |" }.join("\n") + "\n"
  end

  def number(value, digits = 3)
    value.nil? || !value.finite? ? "—" : format("%.#{digits}f", value)
  end

  def cpu_data(data, metadata)
    result = {}
    read_gzip(data.join("native-runs.jsonl.gz")).each do |run|
      raise "failed CPU row #{run['input']}" unless run.fetch("status") == "ok"
      rows = run.fetch("stdout").lines.map { |line| JSON.parse(line) }
      samples = rows.select { |row| row["type"] == "sample" }
      raise "sample coverage" unless samples.length == metadata.fetch("samples")
      answer = rows.find { |row| row["type"] == "answer" }
      raise "missing answer" unless answer
      event_samples = rows.select { |row| row["type"] == "events" }
      raise "event coverage" unless event_samples.length == metadata.fetch("profile_samples")
      floor = rows.find { |row| row["type"] == "calibration" }.fetch("clock_pair_ns")
      events = event_samples.first.fetch("batches").each_with_index.map do |event, index|
        times = event_samples.map do |sample|
          actual = sample.fetch("batches").fetch(index)
          raise "unstable callback" unless actual.slice("kind", "count", "elements") == event.slice("kind", "count", "elements")
          actual.fetch("ns")
        end
        event.merge("raw_ns" => median(times), "ns" => [median(times) - floor, 0].max)
      end
      raw_profile = rows.select { |row| row["type"] == "profile" }
      profile = COMPONENTS.each_with_index.to_h do |component, index|
        [component, median(raw_profile.map { |row| row.fetch("components")[index] })]
      end
      structure = rows.select { |row| row["type"] == "structure" }
      classes = if structure.empty?
                  []
                else
                  CLASSES.each_index.map do |index|
                    structure.first.fetch("classes").fetch(index).to_h do |key, value|
                      values = structure.map { |row| row.fetch("classes").fetch(index).fetch(key) }
                      raise "unstable structure #{key}" unless key.end_with?("ns") || values.uniq.length == 1
                      [key, key.end_with?("ns") ? median(values) : value]
                    end
                  end
                end
      total = median(samples.map { |row| row.fetch("ns") })
      kernel = events.sum { |row| row.fetch("ns") }
      result[[run.fetch("input"), run.fetch("variant")]] = {
        answer: answer, total: total, kernel: kernel, raw_kernel: events.sum { |row| row.fetch("raw_ns") },
        profile: profile, events: events, classes: classes, structure: structure,
        samples: samples.map { |row| row.fetch("ns") },
        p10: percentile(samples.map { |row| row.fetch("ns") }, 0.1),
        p90: percentile(samples.map { |row| row.fetch("ns") }, 0.9), floor: floor,
        reductions: samples.first.slice("r0", "r1", "r2", "rn")
      }
    end
    metadata.fetch("input_sha256").each_key do |input|
      answers = VARIANTS.map { |variant| result.fetch([input, variant]).fetch(:answer) }
      raise "CPU assignment mismatch #{input}" unless answers.uniq.length == 1
      reductions = VARIANTS.map { |variant| result.fetch([input, variant]).fetch(:reductions) }
      raise "CPU reduction mismatch #{input}" unless reductions.uniq.length == 1
    end
    result
  end

  def model_data(data, metadata, cpu)
    result = {}
    read_gzip(data.join("model-runs.jsonl.gz")).group_by { |run| [run.fetch("input"), run.fetch("model")] }.each do |key, runs|
      input, model = key
      expected_count = model == "l1" ? 1 : metadata.fetch("model_samples")
      raise "model coverage" unless runs.length == expected_count
      parsed = runs.map do |run|
        raise "failed model #{key}" unless run.fetch("status") == "ok"
        stdout = run.fetch("stdout")
        answer = cpu.fetch([input, "current_original"]).fetch(:answer)
        expected = ["optimum #{answer.fetch('objective')}", "assignment #{answer.fetch('assignment').join(' ')}",
                    "solution heuristic", "strategy HEURISTIC_RN"]
        actual = stdout.lines.map(&:chomp).select { |line| line.start_with?("optimum ", "assignment ", "solution ", "strategy ") }
        raise "model assignment mismatch #{key}" unless actual == expected
        counters = if model == "l2"
                     JSON.parse(stdout.lines.find { |line| line.start_with?("l2 ") }.delete_prefix("l2 "))
                   else
                     stdout.lines.find { |line| line.start_with?("timing cycles=") }.split.drop(1).to_h do |field|
                       name, value = field.split("=", 2)
                       [name, Integer(value)]
                     end
                   end
        events = run.fetch("events").lines.map { |line| JSON.parse(line) }
        raise "cycle partition" unless events.sum { |row| row.fetch("cycles") } == counters.fetch("cycles")
        if model == "l2"
          phases = counters.select { |name, _| name.end_with?("_cycles") && !name.start_with?("opcode") }
          raise "phase partition" unless phases.values.sum == counters.fetch("cycles")
          raise "submission partition" unless events.length == counters.fetch("submissions")
        end
        [counters, events]
      end
      raise "unstable modeled cycles" unless parsed.map(&:first).uniq.length == 1
      counters = parsed.first.first
      raise "native/model callback count mismatch #{key}" unless
        parsed.first.last.length == cpu.fetch([input, "structured"]).fetch(:events).length
      events = parsed.first.last.each_with_index.map do |event, index|
        native = cpu.fetch([input, "structured"]).fetch(:events).fetch(index)
        raise "callback stream mismatch #{key} #{index}" unless native.slice("kind", "count") == event.slice("kind", "count")
        raise "stream shape changed" unless parsed.all? { |_, stream| stream.length == parsed.first.last.length }
        event.to_h do |name, value|
          values = parsed.map { |_, stream| stream[index].fetch(name) }
          raise "unstable event cycles" if name == "cycles" && values.uniq.length != 1
          [name, name.end_with?("ns") ? median(values) : value]
        end
      end
      result[key] = { counters: counters, events: events }
    end
    result
  end

  # Fit only on every third graph in filename order. The other graphs remain
  # held out. Nonnegative affine estimates use logical batch elements; decisions
  # never read the held-out batch's measured CPU cost.
  def fit(points)
    return [0.0, 0.0] if points.empty?
    n = points.length.to_f
    sx = points.sum(&:first); sy = points.sum(&:last)
    sxx = points.sum { |x, _| x * x }; sxy = points.sum { |x, y| x * y }
    divisor = n * sxx - sx * sx
    slope = divisor.positive? ? [(n * sxy - sx * sy) / divisor, 0.0].max : 0.0
    intercept = [(sy - slope * sx) / n, 0.0].max
    [intercept, slope]
  end

  def predict(coefficients, elements)
    coefficients[0] + coefficients[1] * elements
  end

  def generate(data)
    metadata = JSON.parse(data.join("metadata.json").read)
    raise "failed/incomplete evidence" unless metadata.fetch("failures").empty? && metadata.key?("finished_utc")
    inputs = metadata.fetch("input_sha256").keys.sort
    raise "corpus coverage" unless metadata.fetch("smoke") || inputs.length == 491
    cpu = cpu_data(data, metadata)
    models = model_data(data, metadata, cpu)
    # A single implementation is selected by aggregate solve time; no per-graph
    # oracle picks the fastest variant. Both candidate results remain reported.
    strong = %w[dense structured].min_by { |v| inputs.sum { |input| cpu.fetch([input, v]).fetch(:total) } }
    graph_rows = []; batch_rows = []; shape_rows = []; profile_rows = []
    inputs.each do |input|
      answer = cpu.fetch([input, strong]).fetch(:answer)
      row = { "input" => input, "nodes" => answer.fetch("nodes"), "edges" => answer.fetch("edges"),
              "objective" => answer.fetch("objective"), "strong_variant" => strong }
      VARIANTS.each do |variant|
        values = cpu.fetch([input, variant])
        row["#{variant}_ns"] = values.fetch(:total)
        row["#{variant}_kernel_ns"] = values.fetch(:kernel)
        row["#{variant}_raw_kernel_ns"] = values.fetch(:raw_kernel)
        row["#{variant}_p10_ns"] = values.fetch(:p10)
        row["#{variant}_p90_ns"] = values.fetch(:p90)
        values.fetch(:profile).each { |k, v| row["#{variant}_profile_#{k}_ns"] = v }
        profile_rows << { "input" => input, "variant" => variant }.merge(values.fetch(:profile).transform_keys { |k| "#{k}_ns" })
      end
      regular = cpu.fetch([input, strong]).fetch(:kernel)
      %w[l1 l2].each do |model|
        cycles = models.fetch([input, model]).fetch(:counters).fetch("cycles")
        row["#{model}_cycles"] = cycles
        row["#{model}_break_even_mhz"] = regular.positive? ? cycles * 1000.0 / regular : nil
      end
      row["strong_ns"] = cpu.fetch([input, strong]).fetch(:total)
      row["strong_kernel_ns"] = regular
      row["strong_control_residual_ns"] = [row["strong_ns"] - regular, 0.0].max
      l2 = models.fetch([input, "l2"])
      row["host_prepare_ns"] = l2.fetch(:events).sum { |e| e.fetch("host_ns") }
      row["staging_ns"] = l2.fetch(:events).sum { |e| e.fetch("staging_ns") }
      row["submission_build_ns"] = l2.fetch(:events).sum { |e| e.fetch("submission_build_ns") }
      row["readback_ns"] = l2.fetch(:events).sum { |e| e.fetch("readback_ns") }
      row["submissions"] = l2.fetch(:events).length
      row["primitives"] = l2.fetch(:counters).fetch("children")
      classes = cpu.fetch([input, "structured"]).fetch(:classes)
      classes.each_with_index do |stats, index|
        stats.each { |k, v| row["projection_#{CLASSES[index]}_#{k}"] = v }
      end
      VARIANTS.each do |variant|
        raise "overwritten native timing #{input}/#{variant}" unless
          row.fetch("#{variant}_ns") == cpu.fetch([input, variant]).fetch(:total)
      end
      row["projection_calls"] = classes.sum { |s| s.fetch("calls") }
      row["projection_elements"] = classes.sum { |s| s.fetch("elements") }
      row["projection_cost_additions"] = classes.sum { |s| s.fetch("evaluated") }
      row["preadd_elements"] = classes.sum { |s| s.fetch("preadd_elements") }
      row["predicate_elements"] = classes.sum { |s| s.fetch("predicate_elements") }
      row["arithmetic_removed_share"] = row["projection_elements"].positive? ? 1.0 - row["projection_cost_additions"].fdiv(row["projection_elements"]) : 0.0
      graph_rows << row
      l2.fetch(:events).each_with_index do |event, index|
        batch_rows << { "input" => input, "index" => index, "split" => inputs.index(input) % 3 == 0 ? "train" : "test" }
          .merge(cpu.fetch([input, strong]).fetch(:events)[index].transform_keys { |k| "cpu_#{k}" })
          .merge(event.transform_keys { |k| "l2_#{k}" })
      end
      structure = cpu.fetch([input, "structured"]).fetch(:structure)
      if structure.any?
        shape_groups = structure.first.fetch("projections").group_by { |p| p.slice("m", "n", "column_stride", "class", "changed", "add3") }
        shape_groups.each do |shape, calls|
          times = structure.map do |sample|
            sample.fetch("projections").select { |p| p.slice(*shape.keys) == shape }.sum { |p| p.fetch("ns") }
          end
          shape_rows << { "input" => input }.merge(shape).merge("calls" => calls.length, "ns" => median(times))
        end
      end
    end
    kernel_run = read_gzip(data.join("kernel-run.jsonl.gz")).fetch(0)
    raise "failed microbench" unless kernel_run.fetch("status") == "ok"
    micro_rows = kernel_run.fetch("stdout").lines.map { |line| JSON.parse(line) }
    kernel_rows = micro_rows.group_by { |r| r.slice("m", "n", "layout", "form", "operation", "level") }.map do |key, samples|
      raise "microbench coverage" unless samples.length == metadata.fetch("kernel_samples")
      times = samples.map { |s| s.fetch("ns").fdiv(s.fetch("iterations")) }
      ns = median(times)
      key.merge("ns" => ns, "p10_ns" => percentile(times, 0.1), "p90_ns" => percentile(times, 0.9),
                "effective_elements_per_second" => key.fetch("m") * key.fetch("n") * 1e9 / ns,
                "logical_bytes_per_second" => (key.fetch("m") * key.fetch("n") + key.fetch("n") + key.fetch("m")) * 4e9 / ns)
    end
    totals = %w[current_original scalar dense structured].to_h do |variant|
      values = inputs.map { |input| cpu.fetch([input, variant]) }
      [variant, { "ns" => values.sum { |v| v.fetch(:total) }, "kernel_ns" => values.sum { |v| v.fetch(:kernel) },
                  "median_ns" => median(values.map { |v| v.fetch(:total) }),
                  "p90_ns" => percentile(values.map { |v| v.fetch(:total) }, 0.9),
                  "p99_ns" => percentile(values.map { |v| v.fetch(:total) }, 0.99),
                  "max_ns" => values.map { |v| v.fetch(:total) }.max,
                  "median_p90_p10_ratio" => median(values.map { |v| v.fetch(:p90) / v.fetch(:p10) }) }]
    end
    sum = ->(name) { graph_rows.sum { |r| r.fetch(name) } }
    sensitivity = FREQUENCIES.map do |mhz|
      allowance = sum.call("strong_kernel_ns") - sum.call("l2_cycles") * 1000.0 / mhz
      { "mhz" => mhz, "ideal_allowance_ns" => allowance,
        "ideal_allowance_per_solve_ns" => allowance / inputs.length,
        "ideal_allowance_per_submission_ns" => allowance / sum.call("submissions"),
        "host_prepare_allowance_ns" => allowance - sum.call("host_prepare_ns") }
    end
    required = [0, 50, 100, 500, 1000].product([0, 100, 1000]).map do |submission, visibility|
      available = sum.call("strong_kernel_ns") - submission * sum.call("submissions") - visibility * inputs.length
      measured = available - sum.call("host_prepare_ns")
      { "submission_ns" => submission, "visibility_per_solve_ns" => visibility,
        "ideal_mhz" => available.positive? ? sum.call("l2_cycles") * 1000.0 / available : nil,
        "measured_host_mhz" => measured.positive? ? sum.call("l2_cycles") * 1000.0 / measured : nil }
    end
    estimates = batch_rows.group_by { |r| r.fetch("cpu_kind") }.to_h do |kind, rows|
      train = rows.select { |r| r.fetch("split") == "train" }
      [kind, %w[cpu_ns l2_cycles l2_host_ns].to_h { |field| [field, fit(train.map { |r| [r.fetch("cpu_elements").to_f, r.fetch(field).to_f] })] }]
    end
    selective = []
    [2000, 8000].product([0, 100], [false, true], %w[all test]).each do |mhz, overhead, preparation, split|
      selected = batch_rows.select { |r| split == "all" || r.fetch("split") == "test" }
      %w[cpu all_offload estimated_threshold oracle_upper_bound].each do |policy|
        cpu_ns = cycles = submissions = host_ns = predicted_wins = false_positives = 0.0
        selected.each do |row|
          estimate = estimates.fetch(row.fetch("cpu_kind"))
          elements = row.fetch("cpu_elements")
          predicted_cost = predict(estimate.fetch("l2_cycles"), elements) * 1000.0 / mhz + overhead
          predicted_cost += predict(estimate.fetch("l2_host_ns"), elements) if preparation
          actual_host = preparation ? row.fetch("l2_host_ns") : 0
          actual_device = row.fetch("l2_cycles") * 1000.0 / mhz + overhead + actual_host
          offload = case policy
                    when "cpu" then false
                    when "all_offload" then true
                    when "estimated_threshold" then predict(estimate.fetch("cpu_ns"), elements) > predicted_cost
                    else row.fetch("cpu_ns") > actual_device
                    end
          if offload
            cycles += row.fetch("l2_cycles"); submissions += 1; host_ns += actual_host
            false_positives += 1 if row.fetch("cpu_ns") <= actual_device
          else
            cpu_ns += row.fetch("cpu_ns")
          end
        end
        identities = selected.map { |r| r.fetch("input") }.uniq
        control = graph_rows.select { |r| identities.include?(r.fetch("input")) }.sum { |r| r.fetch("strong_control_residual_ns") }
        selective << { "policy" => policy, "split" => split, "mhz" => mhz, "submission_ns" => overhead,
                       "measured_preparation" => preparation, "remaining_cpu_kernel_ns" => cpu_ns,
                       "l2_cycles" => cycles, "submissions" => submissions, "host_prepare_ns" => host_ns,
                       "false_positives" => false_positives,
                       "modeled_kernel_ns" => cpu_ns + host_ns + submissions * overhead + cycles * 1000.0 / mhz,
                       "modeled_total_ns" => control + cpu_ns + host_ns + submissions * overhead + cycles * 1000.0 / mhz }
      end
    end
    summary = { "graphs" => inputs.length, "strong_variant" => strong, "totals" => totals,
                "l1_cycles" => sum.call("l1_cycles"), "l2_cycles" => sum.call("l2_cycles"),
                "host_prepare_ns" => sum.call("host_prepare_ns"), "submissions" => sum.call("submissions"),
                "primitives" => sum.call("primitives"), "kernel_ns" => sum.call("strong_kernel_ns"),
                "l2_break_even_mhz" => sum.call("l2_cycles") * 1000.0 / sum.call("strong_kernel_ns"),
                "l1_break_even_mhz" => sum.call("l1_cycles") * 1000.0 / sum.call("strong_kernel_ns"),
                "projection_elements" => sum.call("projection_elements"),
                "projection_cost_additions" => sum.call("projection_cost_additions"),
                "preadd_elements" => sum.call("preadd_elements"), "predicate_elements" => sum.call("predicate_elements"),
                "negative_control_residual_graphs" => graph_rows.count { |r| r.fetch("strong_kernel_ns") > r.fetch("strong_ns") },
                "clock_floor_median_ns" => median(cpu.values.map { |v| v.fetch(:floor) }),
                "threshold_estimates" => estimates }
    comparison = [["corpus", graph_rows]]
    ordered = graph_rows.sort_by { |r| [r.fetch("strong_kernel_ns"), r.fetch("input")] }
    [0.5, 0.9, 0.99].each { |p| comparison << ["CPU-kernel p#{(100*p).to_i} rank", [ordered[(p * ordered.length).ceil - 1]]] }
    comparison << ["projection-heavy top 10% by elements", graph_rows.sort_by { |r| -r.fetch("projection_elements") }.first((inputs.length * 0.1).ceil)]
    comparison << ["CPU most expensive ten", graph_rows.sort_by { |r| -r.fetch("strong_ns") }.first(10)]
    compare_rows = comparison.map do |name, rows|
      kernel = rows.sum { |r| r.fetch("strong_kernel_ns") }
      l1 = rows.sum { |r| r.fetch("l1_cycles") }; l2 = rows.sum { |r| r.fetch("l2_cycles") }
      { "workload" => name, "graphs" => rows.length, "cpu_kernel_ns" => kernel,
        "cpu_total_ns" => rows.sum { |r| r.fetch("strong_ns") }, "l1_cycles" => l1, "l2_cycles" => l2,
        "l2_l1" => l1.positive? ? l2.fdiv(l1) : nil,
        "l1_mhz" => kernel.positive? ? l1 * 1000.0 / kernel : nil,
        "l2_mhz" => kernel.positive? ? l2 * 1000.0 / kernel : nil }
    end
    # Exact model discrepancy partition, using the same current stream.
    l1 = models.select { |(_, m), _| m == "l1" }.values.map { |v| v.fetch(:counters) }
    l2 = models.select { |(_, m), _| m == "l2" }.values.map { |v| v.fetch(:counters) }
    phase = ->(name) { l2.sum { |r| r.fetch(name) } }
    l1sum = ->(name) { l1.sum { |r| r.fetch(name) } }
    differences = {
      "descriptor service" => 3 * phase.call("descriptor_requests") - l1sum.call("descriptor"),
      "operand requests/reuse" => 3 * phase.call("operand_requests") - l1sum.call("operands"),
      "ADD/tree/merge" => %w[add1_cycles add2_cycles tree_cycles merge_cycles].sum { |p| phase.call(p) } - l1sum.call("compute"),
      "result acknowledgements" => 3 * phase.call("result_requests") - l1sum.call("result"),
      "decode/protection/control" => %w[decode_cycles protection_cycles drain_cycles control_cycles].sum { |p| phase.call(p) },
      "removed L1 read/compute overlap" => l1sum.call("descriptor") + l1sum.call("operands") + l1sum.call("compute") + l1sum.call("result") - l1sum.call("cycles")
    }
    raise "model discrepancy partition" unless differences.values.sum == sum.call("l2_cycles") - sum.call("l1_cycles")
    summary["l2_l1_difference"] = differences
    summary["phase_cycles"] = l2.first.keys.grep(/_cycles$/).reject { |k| k.start_with?("opcode") }.to_h { |k| [k, phase.call(k)] }
    outputs = {
      "graphs.csv" => csv_text(graph_rows), "batches.csv" => csv_text(batch_rows),
      "projection-shapes.csv" => csv_text(shape_rows), "profiles.csv" => csv_text(profile_rows),
      "kernels.csv" => csv_text(kernel_rows), "sensitivity.csv" => csv_text(sensitivity),
      "required-frequency.csv" => csv_text(required), "selective-offload.csv" => csv_text(selective),
      "comparison.csv" => csv_text(compare_rows), "summary.json" => JSON.pretty_generate(summary) + "\n"
    }
    tables = "# Generated CPU baseline tables\n\n"
    tables << "## Native corpus timing\n\n" + table(["Variant", "Aggregate ms", "Kernel ms", "Median µs", "p90 µs", "p99 µs", "Max µs", "Median p90/p10"], totals.map do |name, values|
      [name, number(values.fetch("ns") / 1e6), number(values.fetch("kernel_ns") / 1e6),
       *%w[median_ns p90_ns p99_ns max_ns].map { |key| number(values.fetch(key) / 1e3) }, number(values.fetch("median_p90_p10_ratio"))]
    end)
    tables << "\n## Exclusive diagnostic profiles (instrumented, not end-to-end measurements)\n\n"
    %w[current structured].each do |variant|
      components = COMPONENTS.to_h { |c| [c, inputs.sum { |input| cpu.fetch([input, variant]).fetch(:profile).fetch(c) }] }
      tables << "### #{variant}\n\n" + table(["Component", "Profile time ms", "Share"], components.map { |c, ns| [c, number(ns / 1e6), number(100 * ns / components.values.sum) + "%"] }) + "\n"
    end
    tables << "## Dynamic projection structure\n\n" + table(["Class", "Calls", "Logical m*n", "Projection timer ms", "Time share", "Logical bytes", "After-R2 calls"], CLASSES.each_with_index.map do |name, index|
      stats = inputs.map { |input| cpu.fetch([input, "structured"]).fetch(:classes)[index] }
      ns = stats.sum { |r| r.fetch("ns") }; total_ns = graph_rows.sum { |r| CLASSES.sum { |c| r.fetch("projection_#{c}_ns") } }
      [name, stats.sum { |r| r.fetch("calls") }, stats.sum { |r| r.fetch("elements") }, number(ns / 1e6), number(100 * ns / total_ns) + "%", stats.sum { |r| r.fetch("bytes") }, stats.sum { |r| r.fetch("changed_calls") }]
    end)
    dynamic = { "initial forbidden matching" => [0, false], "structured after reductions" => [:structured, true], "generic dense" => [3, :all] }
    tables << "\n" + table(["Origin/class", "Calls", "Elements", "Projection ms"], dynamic.map do |name, (kind, changed)|
      selected = shape_rows.select do |r|
        (kind == :structured ? r.fetch("class") != 3 : r.fetch("class") == kind) &&
          (changed == :all || r.fetch("changed") == changed)
      end
      [name, selected.sum { |r| r.fetch("calls") }, selected.sum { |r| r.fetch("calls") * r.fetch("m") * r.fetch("n") }, number(selected.sum { |r| r.fetch("ns") } / 1e6)]
    end)
    tables << "\n## Warmed kernel comparison (ns/call)\n\n"
    selected_keys = kernel_rows.select { |r| r.fetch("layout") == 0 && r.fetch("form") < 2 && r.fetch("level") == 0 && [7,16,17,49].include?(r.fetch("m")) }.map { |r| r.slice("m", "n", "layout", "form", "operation") }
    tables << table(["Kernel/shape/form", "Scalar", "Dense", "Structured", "Dense/structured", "Structured effective elements/s"], selected_keys.map do |key|
      timings = (0..2).map { |level| kernel_rows.find { |r| r.slice(*key.keys) == key && r.fetch("level") == level } }
      ["#{key.fetch('operation') == 0 ? 'PROJECT' : 'MAP3'} #{key.fetch('m')}×#{key.fetch('n')} #{key.fetch('form') == 0 ? '0/INF' : 'row exceptions'}", *timings.map { |r| number(r.fetch("ns"), 1) }, number(timings[1].fetch("ns") / timings[2].fetch("ns")), number(timings[2].fetch("effective_elements_per_second"), 0)]
    end)
    tables << "\n## CPU versus modeled device work\n\n" + table(["Workload", "CPU kernel µs", "CPU total µs", "L1 cycles", "L2 cycles", "L2/L1", "L1 MHz", "L2 MHz"], compare_rows.map do |r|
      [r.fetch("workload"), number(r.fetch("cpu_kernel_ns") / 1e3), number(r.fetch("cpu_total_ns") / 1e3), r.fetch("l1_cycles"), r.fetch("l2_cycles"), number(r.fetch("l2_l1")), number(r.fetch("l1_mhz")), number(r.fetch("l2_mhz"))]
    end)
    tables << "\n## End-to-end sensitivity (negative allowance means no break-even)\n\n" + table(["Device MHz", "Ideal max additional ms/corpus", "ns/submission", "ns/solve", "With measured preparation ms/corpus"], sensitivity.map do |r|
      [r.fetch("mhz"), number(r.fetch("ideal_allowance_ns") / 1e6), number(r.fetch("ideal_allowance_per_submission_ns")), number(r.fetch("ideal_allowance_per_solve_ns")), number(r.fetch("host_prepare_allowance_ns") / 1e6)]
    end)
    tables << "\n" + table(["Submission ns", "Visibility ns/solve", "Ideal required MHz", "With measured preparation MHz"], required.map { |r| [r.fetch("submission_ns"), r.fetch("visibility_per_solve_ns"), number(r.fetch("ideal_mhz")), number(r.fetch("measured_host_mhz"))] })
    tables << "\n## Selective offload on held-out graphs\n\n" + table(["Policy", "MHz", "Fixed ns", "Preparation", "CPU kernel ms", "L2 cycles", "Submissions", "Modeled solve ms", "False offloads"], selective.select { |r| r.fetch("split") == "test" && r.fetch("submission_ns") == 0 }.map do |r|
      [r.fetch("policy"), r.fetch("mhz"), r.fetch("submission_ns"), r.fetch("measured_preparation") ? "measured" : "ideal", number(r.fetch("remaining_cpu_kernel_ns") / 1e6), r.fetch("l2_cycles").to_i, r.fetch("submissions").to_i, number(r.fetch("modeled_total_ns") / 1e6), r.fetch("false_positives").to_i]
    end)
    tables << "\n## L2 phase cycles\n\n" + table(["Phase", "Cycles", "Share"], summary.fetch("phase_cycles").map { |k,v| [k,v,number(100 * v.fdiv(summary.fetch("l2_cycles"))) + "%"] })
    tables << "\n## Current L2 minus L1\n\n" + table(["Difference", "Cycles"], differences.to_a)
    top = graph_rows.sort_by { |r| -r.fetch("strong_ns") }.first(10)
    tables << "\n## Most expensive native graphs\n\n" + table(["Input", "Nodes", "Edges", "CPU µs", "Kernel µs", "L2 cycles"], top.map { |r| [File.basename(r.fetch("input")),r.fetch("nodes"),r.fetch("edges"),number(r.fetch("strong_ns") / 1e3),number(r.fetch("strong_kernel_ns") / 1e3),r.fetch("l2_cycles")] })
    summary["top_ten_cpu_share"] = top.sum { |r| r.fetch("strong_ns") } / totals.fetch(strong).fetch("ns")
    outputs["summary.json"] = JSON.pretty_generate(summary) + "\n"
    outputs["tables.md"] = tables
    [outputs, metadata]
  end

  def verify_archive(data, metadata)
    sums = data.join("SHA256SUMS").read.lines
    sums.each do |line|
      expected, file = line.chomp.split("  ", 2)
      raise "checksum #{file}" unless Digest::SHA256.file(data.join(file)).hexdigest == expected
    end
    # Verify the pinned source overlay and corpus, rather than demanding that
    # a future working checkout still be the measured revision.
    %w[implementation inputs].each do |archive|
      hashes = metadata.fetch(archive == "implementation" ? "implementation_sha256" : "input_sha256")
      hashes.each do |file, expected|
        bytes, error, status = Open3.capture3("tar", "-xOzf", data.join("#{archive}.tar.gz").to_s, file)
        raise "archive member #{file}: #{error}" unless status.success? && Digest::SHA256.hexdigest(bytes) == expected
      end
    end
  end
end

if $PROGRAM_NAME == __FILE__
  options = { data: "doc/reports/data/cpu-baseline", verify: false }
  OptionParser.new do |p|
    p.on("--data PATH") { |v| options[:data] = v }
    p.on("--verify") { options[:verify] = true }
  end.parse!
  abort "unexpected arguments" unless ARGV.empty?
  data = CpuBaselineReport::ROOT.join(options[:data])
  outputs, metadata = CpuBaselineReport.generate(data)
  if options[:verify]
    CpuBaselineReport.verify_archive(data, metadata)
    outputs.each { |name, content| raise "generated drift #{name}" unless data.join(name).read == content }
    puts "verified #{metadata.fetch('input_sha256').length} inputs, samples, answers, streams, source archives, checksums and tables"
  else
    outputs.each { |name, content| data.join(name).write(content) }
    names = data.children.select(&:file?).map { |p| p.basename.to_s }.reject { |n| n == "SHA256SUMS" }.sort
    data.join("SHA256SUMS").write(names.map { |n| "#{Digest::SHA256.file(data.join(n)).hexdigest}  #{n}\n" }.join)
    puts "regenerated #{outputs.length} outputs"
  end
end
