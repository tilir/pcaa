#!/usr/bin/env ruby
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 PCAA contributors
# Records deterministic PBQP scaling points (graph-size sweep, domain-size
# sweep, a joint N x D grid, and an RN-policy comparison) through the public
# graph generator and solver CLI tools. Every row comes from one real solver
# invocation; this script only drives the sweep and parses its diagnostics.

require "csv"
require "open3"
require "optparse"
require "set"
require "tempfile"

# Per-family default node lists for the graph-size sweep. mixed-degree is
# capped below 1000: a single N=1000/D=8 mixed-degree run already runs for
# several minutes because RN cascades over its higher average degree grow
# superlinearly, while degree-3/degree-4 solve N=1000 in about a second.
DEFAULT_FAMILY_PROFILES = { "degree-3" => "binary", "degree-4" => "binary",
                            "mixed-degree" => "small" }.freeze
DEFAULT_GRAPH_SIZE_NODES = { "degree-3" => [20, 50, 100, 200, 500, 1000],
                              "degree-4" => [20, 50, 100, 200, 500, 1000],
                              "mixed-degree" => [20, 50, 100, 200, 500] }.freeze
DEFAULT_GRAPH_SIZE_DOMAINS = [2, 4, 8].freeze

DEFAULT_DOMAIN_SIZE_NODES = { "degree-3" => [20, 100, 500], "degree-4" => [20, 100, 500],
                               "mixed-degree" => [20, 100] }.freeze
DEFAULT_DOMAIN_SIZE_DOMAINS = [2, 4, 8, 16].freeze

DEFAULT_GRID_FAMILIES = %w[degree-3 degree-4].freeze
DEFAULT_GRID_NODES = [20, 50, 100, 200].freeze
DEFAULT_GRID_DOMAINS = [2, 4, 8, 16].freeze
DEFAULT_GRID_SEEDS = (1001..1003).to_a.freeze

DEFAULT_POLICY_POINTS = [["degree-3", 100, 4], ["degree-3", 200, 8],
                          ["degree-4", 100, 4], ["degree-4", 200, 8],
                          ["mixed-degree", 100, 4], ["mixed-degree", 200, 8]].freeze
RN_POLICIES = %w[min-degree max-degree min-work].freeze

DEFAULT_SEEDS = (1001..1005).to_a.freeze
DEFAULT_TIMEOUT = 180

def parse_family_list(text, default_profiles)
  text.split(",").to_h do |entry|
    family, profile = entry.split(":", 2)
    [family, profile || default_profiles.fetch(family) { abort "unknown family #{family}, pass family:profile" }]
  end
end

def parse_int_list(text)
  text.split(",").map { |value| Integer(value, 10) }
end

def parse_nodes_for(values, default_profiles)
  values.to_h do |entry|
    family, list = entry.split("=", 2)
    abort "unknown family #{family}" unless default_profiles.key?(family)
    [family, parse_int_list(list)]
  end
end

options = { runner: "build/pcaa_graph_run", generator: "build/pbqp_graph_generate",
            seeds: DEFAULT_SEEDS, grid_seeds: DEFAULT_GRID_SEEDS, timeout: DEFAULT_TIMEOUT,
            sweeps: %w[graph-size domain-size grid policy], resume: false, quiet: false,
            families: DEFAULT_FAMILY_PROFILES.dup,
            graph_size_nodes: DEFAULT_GRAPH_SIZE_NODES.dup, graph_size_domains: DEFAULT_GRAPH_SIZE_DOMAINS,
            domain_size_nodes: DEFAULT_DOMAIN_SIZE_NODES.dup, domain_size_domains: DEFAULT_DOMAIN_SIZE_DOMAINS,
            policies: RN_POLICIES }
nodes_for_overrides = []
domain_nodes_for_overrides = []

OptionParser.new do |parser|
  parser.banner = "Usage: ruby scripts/scaling_characterize.rb [options] OUTPUT.csv"
  parser.on("--runner PATH", "pcaa_graph_run executable") { |v| options[:runner] = v }
  parser.on("--generator PATH", "pbqp_graph_generate executable") { |v| options[:generator] = v }
  parser.on("--sweeps LIST", "Comma-separated subset of graph-size,domain-size,grid,policy") do |v|
    options[:sweeps] = v.split(",")
  end
  parser.on("--families LIST", "Comma-separated family[:profile] list (default: degree-3,degree-4,mixed-degree)") do |v|
    options[:families] = parse_family_list(v, DEFAULT_FAMILY_PROFILES)
  end
  parser.on("--seeds LIST", "Comma-separated seeds for graph-size/domain-size/policy sweeps (default: 1001..1005)") do |v|
    options[:seeds] = parse_int_list(v)
  end
  parser.on("--grid-seeds LIST", "Comma-separated seeds for the 2D grid (default: 1001..1003)") do |v|
    options[:grid_seeds] = parse_int_list(v)
  end
  parser.on("--nodes-for FAMILY=LIST", "Override the graph-size sweep node list for one family; repeatable") do |v|
    nodes_for_overrides << v
  end
  parser.on("--domain-nodes-for FAMILY=LIST", "Override the domain-size sweep node list for one family; repeatable") do |v|
    domain_nodes_for_overrides << v
  end
  parser.on("--graph-size-domains LIST", "Fixed domain sizes for the graph-size sweep (default: 2,4,8)") do |v|
    options[:graph_size_domains] = parse_int_list(v)
  end
  parser.on("--domain-size-domains LIST", "Domain sizes for the domain-size sweep (default: 2,4,8,16)") do |v|
    options[:domain_size_domains] = parse_int_list(v)
  end
  parser.on("--policies LIST", "Comma-separated RN policies to compare (default: min-degree,max-degree,min-work)") do |v|
    options[:policies] = v.split(",")
  end
  parser.on("--timeout SECONDS", Integer, "Kill and record a run as time-limit past this wall clock (default: 180)") do |v|
    options[:timeout] = v
  end
  parser.on("--resume", "Skip (family,nodes,domain,policy,strategy,seed) tuples already present in OUTPUT.csv") do
    options[:resume] = true
  end
  parser.on("--quiet", "Do not print per-run progress to stderr") { options[:quiet] = true }
  parser.on("--dry-run", "Print the deduplicated job count and exit without running anything") do
    options[:dry_run] = true
  end
  parser.on("--help", "Show this help") { puts parser; exit 0 }
end.parse!

output_path = ARGV.shift
abort "usage: scaling_characterize.rb [options] OUTPUT.csv" unless ARGV.empty? && output_path

options[:graph_size_nodes] = DEFAULT_GRAPH_SIZE_NODES.merge(parse_nodes_for(nodes_for_overrides, options[:families]))
options[:domain_size_nodes] = DEFAULT_DOMAIN_SIZE_NODES.merge(parse_nodes_for(domain_nodes_for_overrides, options[:families]))

# Build the deduplicated job list: every (family,nodes,domain,policy,strategy,seed)
# tuple is solved at most once even when several sweeps ask for the same point,
# because mixed-degree runs are expensive enough that repeating them is wasteful.
jobs = {}

def add_job(jobs, tag, family, profile, nodes, domain, policy, strategy, seeds)
  seeds.each do |seed|
    key = [family, nodes, domain, policy, strategy, seed]
    (jobs[key] ||= { profile: profile, tags: Set.new }).tap { |job| job[:tags] << tag }
  end
end

if options[:sweeps].include?("graph-size")
  options[:families].each do |family, profile|
    next unless options[:graph_size_nodes].key?(family)

    options[:graph_size_nodes].fetch(family).each do |nodes|
      options[:graph_size_domains].each do |domain|
        add_job(jobs, "graph-size", family, profile, nodes, domain, "min-degree", "heuristic-rn", options[:seeds])
      end
    end
  end
end

if options[:sweeps].include?("domain-size")
  options[:families].each do |family, profile|
    next unless options[:domain_size_nodes].key?(family)

    options[:domain_size_nodes].fetch(family).each do |nodes|
      options[:domain_size_domains].each do |domain|
        add_job(jobs, "domain-size", family, profile, nodes, domain, "min-degree", "heuristic-rn", options[:seeds])
      end
    end
  end
end

if options[:sweeps].include?("grid")
  DEFAULT_GRID_FAMILIES.each do |family|
    profile = options[:families][family] || DEFAULT_FAMILY_PROFILES.fetch(family)
    DEFAULT_GRID_NODES.each do |nodes|
      DEFAULT_GRID_DOMAINS.each do |domain|
        add_job(jobs, "grid", family, profile, nodes, domain, "min-degree", "heuristic-rn", options[:grid_seeds])
      end
    end
  end
end

if options[:sweeps].include?("policy")
  DEFAULT_POLICY_POINTS.each do |family, nodes, domain|
    profile = options[:families][family] || DEFAULT_FAMILY_PROFILES.fetch(family)
    options[:policies].each do |policy|
      add_job(jobs, "policy", family, profile, nodes, domain, policy, "heuristic-rn", options[:seeds])
    end
  end
end

done_keys = Set.new
if options[:resume] && File.exist?(output_path)
  CSV.foreach(output_path, headers: true) do |row|
    done_keys << [row["family"], row["nodes"].to_i, row["domain"].to_i, row["policy"], row["strategy"], row["seed"].to_i]
  end
end

def invoke(*command)
  Open3.capture3(*command)
end

# Runs `command` and kills it (the launcher execs into the real model process,
# so its own pid is the one doing the work) once `timeout` seconds elapse.
def invoke_with_timeout(command, timeout)
  stdout_text = +""
  stderr_text = +""
  status = nil
  Open3.popen3(*command) do |stdin, stdout, stderr, wait_thr|
    stdin.close
    out_reader = Thread.new { stdout_text = stdout.read }
    err_reader = Thread.new { stderr_text = stderr.read }
    if wait_thr.join(timeout)
      status = wait_thr.value
    else
      begin
        Process.kill("TERM", wait_thr.pid)
      rescue Errno::ESRCH
        nil
      end
      wait_thr.join(2)
      begin
        Process.kill("KILL", wait_thr.pid)
      rescue Errno::ESRCH
        nil
      end
      status = :timeout
    end
    out_reader.join
    err_reader.join
  end
  [stdout_text, stderr_text, status]
end

# Computes initial graph-state metrics directly from the generator's text
# output: node/edge counts, degree, domain-size statistics, unary/pairwise
# element counts, and cost-table bytes (int32_t elements, matching pbqp.h).
def graph_stats(text)
  domains = []
  edges = []
  text.each_line do |line|
    fields = line.strip.split
    next if fields.empty? || fields.first == "#"

    case fields.first
    when "node"
      domains << Integer(fields[1], 10)
    when "edge"
      edges << [Integer(fields[1], 10), Integer(fields[2], 10)]
    end
  end
  degree = Hash.new(0)
  matrix_elements = 0
  edges.each do |i, j|
    degree[i] += 1
    degree[j] += 1
    matrix_elements += domains[i] * domains[j]
  end
  unary_elements = domains.sum
  { nodes: domains.length, edges: edges.length, max_degree: degree.values.max || 0,
    domain_min: domains.min, domain_mean: domains.sum.to_f / domains.length, domain_max: domains.max,
    unary_elements: unary_elements, matrix_elements: matrix_elements,
    cost_table_bytes: (unary_elements + matrix_elements) * 4 }
end

def metrics(text, pattern)
  match = text.match(pattern)
  match && match.captures.map(&:to_i)
end

REDUCTIONS_RE = /reductions R0=(\d+) R1=(\d+) R2=(\d+) RN=(\d+) projections=(\d+) projection_primitives=(\d+) commits=(\d+)/
RN_CORE_RE = /RN core first=(\d+) nodes\/(\d+) edges max=(\d+) nodes\/(\d+) edges episodes=(\d+) degree=(\d+)\.\.(\d+) after-RN R0=(\d+) R1=(\d+) R2=(\d+) cascade-total=(\d+) cascade-max=(\d+)/
CASCADES_RE = /rn_cascades rn_episodes=(\d+) rn_cascade_r0_total=(\d+) rn_cascade_r1_total=(\d+) rn_cascade_r2_total=(\d+) rn_cascade_exact_total=(\d+) rn_cascade_mean=([0-9.]+) rn_cascade_max=(\d+)/
OPERATION_MIX_RE = /operation mix project-elements=(\d+) project-accumulate-elements=(\d+) slice-elements=(\d+) map3-elements=(\d+) argmin-elements=(\d+) descriptors=(\d+) batches=(\d+) operand-bytes=(\d+) result-bytes=(\d+) bytes=(\d+)/

HEADER = %w[sweeps family profile seed nodes domain policy strategy status wall_time_seconds
            initial_nodes initial_edges max_degree domain_min domain_mean domain_max
            unary_elements matrix_elements initial_bytes
            r0 r1 r2 rn projections projection_primitives rn_commit_elements
            first_rn_nodes first_rn_edges max_rn_nodes max_rn_edges rn_degree_min rn_degree_max
            r0_after_rn r1_after_rn r2_after_rn
            rn_episodes cascade_r0_total cascade_r1_total cascade_r2_total cascade_exact_total
            cascade_mean cascade_max
            project_elements project_accumulate_elements slice_elements map3_elements argmin_elements
            descriptors batches operand_bytes result_bytes total_bytes total_elements
            elements_per_rn_episode].freeze

def run_point(options, job, key)
  family, nodes, domain, policy, strategy, seed = key
  profile = job[:profile]
  graph, _diag, generated = invoke(options[:generator], "--family", family, "--profile", profile,
                                    "--nodes", nodes.to_s, "--seed", seed.to_s, "--domain-size", domain.to_s)
  unless generated.success?
    return [job[:tags].to_a.sort.join("|"), family, profile, seed, nodes, domain, policy, strategy,
            "generator-error", nil] + (Array.new(HEADER.length - 10))
  end

  stats = graph_stats(graph)
  row_prefix = [job[:tags].to_a.sort.join("|"), family, profile, seed, nodes, domain, policy, strategy]
  start_time = Time.now

  Tempfile.create(["pcaa-scaling-", ".pbqp"]) do |file|
    file.write(graph)
    file.flush
    command = [options[:runner], "--solver", "local", "--strategy", strategy, "--rn-policy", policy,
               "--verbose", file.path]
    stdout_text, stderr_text, status = invoke_with_timeout(command, options[:timeout])
    elapsed = Time.now - start_time

    state = [stats[:nodes], stats[:edges], stats[:max_degree], stats[:domain_min],
             format("%.3f", stats[:domain_mean]), stats[:domain_max], stats[:unary_elements],
             stats[:matrix_elements], stats[:cost_table_bytes]]

    filled = row_prefix.length + 2 + state.length
    if status == :timeout
      return row_prefix + ["time-limit", format("%.3f", elapsed)] + state + Array.new(HEADER.length - filled)
    end
    unless status.success?
      capacity_limited = stderr_text.include?("does not fit the shared PBQP solver") ||
                          stderr_text.include?("PCAA model could not solve the graph")
      run_status = capacity_limited ? "capacity-limit" : "runner-error"
      return row_prefix + [run_status, format("%.3f", elapsed)] + state + Array.new(HEADER.length - filled)
    end

    reductions = metrics(stderr_text, REDUCTIONS_RE)
    rn_core = metrics(stderr_text, RN_CORE_RE)
    cascades = metrics(stderr_text, CASCADES_RE)
    mix = metrics(stderr_text, OPERATION_MIX_RE)
    unless reductions && rn_core && cascades && mix
      return row_prefix + ["missing-diagnostics", format("%.3f", elapsed)] + state + Array.new(HEADER.length - filled)
    end

    total_elements = mix.first(5).sum
    rn_episodes = cascades[0]
    elements_per_episode = rn_episodes.positive? ? format("%.2f", total_elements.to_f / rn_episodes) : ""

    row_prefix + ["ok", format("%.3f", elapsed)] + state +
      reductions + [rn_core[0], rn_core[1], rn_core[2], rn_core[3], rn_core[5], rn_core[6],
                    rn_core[7], rn_core[8], rn_core[9]] +
      cascades + mix + [total_elements, elements_per_episode]
  end
end

pending = jobs.reject { |key, _| done_keys.include?(key) }
warn "scaling_characterize: #{jobs.length} points total, #{pending.length} to run " \
     "(#{done_keys.length} already present)" unless options[:quiet]

if options[:dry_run]
  by_tag = Hash.new(0)
  jobs.each_value { |job| job[:tags].each { |tag| by_tag[tag] += 1 } }
  by_tag.each { |tag, count| warn "  #{tag}: #{count} points" }
  exit 0
end

mode = options[:resume] && File.exist?(output_path) ? "a" : "w"
File.open(output_path, mode) do |file|
  CSV(file) do |csv|
    csv << HEADER if mode == "w"
    pending.each_with_index do |(key, job), index|
      unless options[:quiet]
        warn "[#{index + 1}/#{pending.length}] #{key.join(' ')} tags=#{job[:tags].to_a.sort.join(',')}"
      end
      row = run_point(options, job, key)
      csv << row
      file.flush
    end
  end
end
