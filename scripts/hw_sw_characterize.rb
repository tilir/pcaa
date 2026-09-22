#!/usr/bin/env ruby
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 PCAA contributors
# Builds hypothetical HW/SW epochs from public PBQP JSONL solver traces.

require "csv"
require "json"
require "open3"
require "tempfile"

RUNNER = ENV.fetch("PCAA_RUNNER", "build/pcaa_graph_run")
GENERATOR = ENV.fetch("PCAA_GENERATOR", "build/pbqp_graph_generate")
FAMILIES = [["degree-3", "binary"], ["degree-4", "binary"], ["mixed-degree", "small"]].freeze
POLICIES = %w[min-degree max-degree min-work].freeze
ELEMENT_FIELDS = %w[project project_accumulate slice map3 argmin].freeze
output_path = ARGV.shift
abort "usage: hw_sw_characterize.rb [OUTPUT.csv]" unless ARGV.empty?

def invoke(*command)
  output, diagnostics, status = Open3.capture3(*command)
  abort "#{command.join(' ')} failed: #{diagnostics}" unless status.success?
  output
end

def work?(event)
  ELEMENT_FIELDS.any? { |field| event.fetch(field).positive? }
end

# Graph decisions that bound Model B/C epochs even with zero accelerator
# arithmetic: R0 is a host-side graph action with no cost-algebra elements,
# but it is still one reduction the host (Model B) or the autonomous cascade
# (Model C) performs, so it must not vanish from the epoch count.
STRUCTURAL_TYPES = %w[R0 R1 R2 RN_SELECT RN_SCORE RN_COMMIT].freeze

def epoch_event?(event)
  work?(event) || STRUCTURAL_TYPES.include?(event["type"])
end

def merge(events, model)
  result = { "model" => model, "events" => events.length }
  (ELEMENT_FIELDS + %w[primitive_descriptors structural_operations operand_bytes result_bytes r0 r1 r2 rn]).each do |field|
    result[field] = events.sum { |event| event.fetch(field, 0) }
  end
  result
end

def epochs(events, model)
  work_events = events.select { |event| work?(event) }
  case model
  when "A-operation"
    work_events.map { |event| merge([event], model) }
  when "B-reduction"
    groups = []
    current = []
    events.each do |event|
      if event["type"] == "RN_SELECT"
        groups << current unless current.empty?
        current = []
      end
      current << event if epoch_event?(event)
      if %w[R0 R1 R2 RN_COMMIT].include?(event["type"])
        groups << current unless current.empty?
        current = []
      end
    end
    groups << current unless current.empty?
    groups.map { |group| merge(group, model) }
  when "C-rn-cascade"
    # One epoch per RN pick plus its following exact cascade, and one more for
    # any exact reductions before the first RN (or for a whole RN-free solve).
    # scaling_characterize.rb's model_c_epochs column applies the same rule.
    groups = [[]]
    events.each do |event|
      if event["type"] == "RN_SELECT" && !groups.last.empty?
        groups << []
      end
      groups.last << event if epoch_event?(event)
    end
    groups.reject(&:empty?).map { |group| merge(group, model) }
  when "D-heuristic"
    solve_events = events.select { |event| epoch_event?(event) }
    solve_events.empty? ? [] : [merge(solve_events, model)]
  when "LS-A-node"
    events.select { |event| event["type"] == "LOCAL_SCORE" }.map { |event| merge([event], model) }
  when "LS-B-sweep"
    sweeps = [[]]
    events.select { |event| event["type"] == "LOCAL_SCORE" }.each do |event|
      sweeps << [] if event["node"].zero? && !sweeps.last.empty?
      sweeps.last << event
    end
    sweeps.reject(&:empty?).map { |group| merge(group, model) }
  else
    []
  end
end

header = %w[family seed solver policy model events project project_accumulate slice map3 argmin
            primitive_descriptors structural_operations operand_bytes result_bytes r0 r1 r2 rn]
rows = []
FAMILIES.each do |family, profile|
  (1001..1010).each do |seed|
    Tempfile.create(["pcaa-hwsw-", ".pbqp"]) do |graph|
      graph.write(invoke(GENERATOR, "--family", family, "--profile", profile, "--nodes", "20",
                         "--seed", seed.to_s))
      graph.flush
      ([["local-search", "min-degree"]] + POLICIES.map { |policy| ["heuristic-rn", policy] } +
       POLICIES.map { |policy| ["heuristic-rn-local-search", policy] }).each do |solver, policy|
        Tempfile.create(["pcaa-events-", ".jsonl"]) do |trace|
          invoke(RUNNER, "--solver", "local", "--strategy", solver, "--rn-policy", policy,
                 "--trace", trace.path, graph.path)
          events = File.readlines(trace.path, chomp: true).reject(&:empty?).map { |line| JSON.parse(line) }
          models = solver == "local-search" ? %w[LS-A-node LS-B-sweep] :
                   solver == "heuristic-rn-local-search" ? %w[A-operation B-reduction C-rn-cascade D-heuristic LS-A-node LS-B-sweep] :
                   %w[A-operation B-reduction C-rn-cascade D-heuristic]
          models.each do |model|
            epochs(events, model).each do |epoch|
              rows << [family, seed, solver, policy, epoch.fetch("model"), *header.drop(5).map { |key| epoch.fetch(key) }]
            end
          end
        end
      end
    end
  end
end
output = output_path ? File.open(output_path, "w") : $stdout
CSV(output) { |csv| csv << header; rows.each { |row| csv << row } }
output.close if output_path
