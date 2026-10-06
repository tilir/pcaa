#!/usr/bin/env ruby
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 PCAA contributors
# Collects bounded exact CPU/model experiments with complete inputs and failure evidence.
require "digest"
require "fileutils"
require "json"
require "optparse"
require "rbconfig"
require "time"
require "zlib"
require_relative "l2_characterize"

options = { build: "build-cpu-release", output: "doc/reports/data/exact-pbqp", cpu: 0,
            limit: 128, samples: 7, cpu_timeout: 8, model_timeout: 20, smoke: false }
invocation = ARGV.dup
OptionParser.new do |p|
  p.on("--build PATH") { |v| options[:build] = v }
  p.on("--output PATH") { |v| options[:output] = v }
  p.on("--limit N", Integer) { |v| options[:limit] = v }
  p.on("--samples N", Integer) { |v| options[:samples] = v }
  p.on("--cpu N", Integer) { |v| options[:cpu] = v }
  p.on("--smoke") { options[:smoke] = true }
end.parse!
abort "invalid arguments" unless ARGV.empty? && options[:limit].positive? && options[:samples] >= 3
$stdout.sync = true
build = ROOT.join(options[:build]); out = ROOT.join(options[:output])
abort "archive exists; use a fresh directory" if out.join("metadata.json").exist?
abort "Release required" unless build.join("CMakeCache.txt").read.include?("CMAKE_BUILD_TYPE:STRING=Release")
FileUtils.mkdir_p(out.join("inputs"))
graphs = []
corpus = ROOT.glob("examples/regalloc/*.pbqp").sort
abort "corpus coverage" unless corpus.length == 491
corpus = corpus.first(3) if options[:smoke]
corpus.each { |p| graphs << { "id" => p.basename.to_s, "class" => "llvm", "source" => p.to_s } }
%w[degree-3 degree-4 mixed-degree].product([12,20],[2,4],[1001,1002]).each do |family,n,d,seed|
  next if options[:smoke] && (n != 12 || d != 2 || seed != 1001)
  id = "#{family}-n#{n}-d#{d}-s#{seed}.pbqp"
  command = [build.join("pbqp_graph_generate"),"--family",family,"--profile","small",
             "--nodes",n,"--domain-size",d,"--seed",seed]
  generated = invoke(command)
  raise generated.inspect unless generated.fetch("status") == "ok"
  out.join("inputs",id).write(generated.fetch("stdout"))
  graphs << { "id" => id, "class" => "synthetic", "family" => family, "n" => n, "d" => d,
              "seed" => seed, "generation" => generated }
end
[4,6,8].product([2,3,4],%w[matching random]).each do |n,d,form|
  next if options[:smoke] && (n != 4 || d != 3)
  seed = 7000+n*100+d; rng = Random.new(seed)
  id = "stress-k#{n}-d#{d}-#{form}-s#{seed}.pbqp"
  text = "nodes #{n}\n"
  n.times { text << "node #{d} #{Array.new(d) { form == 'matching' ? 0 : rng.rand(-5..5) }.join(' ')}\n" }
  n.times do |a|
    ((a+1)...n).each do |b|
      costs = Array.new(d) { |i| Array.new(d) { |j| form == "matching" ? (i == j ? "INF" : 0) : rng.rand(-9..9) } }
      text << "edge #{a} #{b} #{costs.flatten.join(' ')}\n"
    end
  end
  out.join("inputs",id).write(text)
  graphs << { "id" => id, "class" => "stress", "family" => "complete-#{form}", "n" => n, "d" => d, "seed" => seed }
end
graphs.each do |g|
  FileUtils.cp(g.fetch("source"),out.join("inputs",g.fetch("id"))) if g.key?("source")
  lines = out.join("inputs",g.fetch("id")).read.lines
  domains = lines.filter_map { |l| l.start_with?("node ") ? Integer(l.split[1]) : nil }
  edges = lines.select { |l| l.start_with?("edge ") }.map { |l| l.split.drop(1).map(&:to_i) }
  g.merge!("nodes" => domains.length, "edges" => edges.length, "domain" => domains.max,
           "unary_elements" => domains.sum, "matrix_elements" => edges.sum { |a,b| domains[a]*domains[b] })
end
sources = ROOT.glob("{accelerator,pcaalib,software,tools,cmake}/**/*").select(&:file?) +
          %w[CMakeLists.txt scripts/exact_pbqp_measure.rb scripts/exact_pbqp_report.rb scripts/exact_pbqp_stack.rb scripts/l2_characterize.rb].map { |p| ROOT.join(p) }
sources = sources.select(&:exist?).uniq.sort
metadata = {
  "base_commit" => invoke(%w[git rev-parse HEAD]).fetch("stdout").strip,
  "dirty_state" => invoke(%w[git status --short]).fetch("stdout"), "started_utc" => Time.now.utc.iso8601,
  "command" => [RbConfig.ruby,$PROGRAM_NAME,*invocation], "options" => options, "graphs" => graphs,
  "policy" => "exact-branch-reduce/min-degree/per-node/local; vector conditioning; same order and bound",
  "workspace_live_limit_bytes" => 512*1024*1024, "l2" => BASE, "model_samples" => 3,
  "implementation_sha256" => sources.to_h { |p| [relative(p),Digest::SHA256.file(p).hexdigest] },
  "input_sha256" => graphs.to_h { |g| ["inputs/#{g.fetch('id')}",Digest::SHA256.file(out.join('inputs',g.fetch('id'))).hexdigest] },
  "binary_sha256" => %w[pcaa_exact_cpu pcaa_exact_cpu_portable pcaa_exact_l2].to_h { |n| [n,Digest::SHA256.file(build.join(n)).hexdigest] },
  "environment" => [%w[lscpu],%w[uname -a],%w[c++ --version],%w[ruby --version],%w[pkg-config --modversion systemc],
                    ["ldd",build.join("pcaa_exact_cpu")]].to_h { |c| [c.join(" "),invoke(c)] }
}
write_json(out.join("metadata.json"),metadata)
FileUtils.cp(build.join("compile_commands.json"),out.join("compile_commands.json"))
FileUtils.cp(build.join("CMakeCache.txt"),out.join("CMakeCache.txt"))
archived = invoke(["tar","-czf",out.join("implementation.tar.gz"),*sources.map { |p| relative(p) }])
raise archived.inspect unless archived.fetch("status") == "ok"
completed = {}
def decoded(result)
  result.fetch("stdout").lines.filter_map { |l| JSON.parse(l) rescue nil }
end
pin = ["taskset","-c",options[:cpu].to_s]
Zlib::GzipWriter.open(out.join("cpu-runs.jsonl.gz")) do |stream|
  graphs.each_with_index do |g,index|
    %w[none heuristic].each do |seed|
      [["avx2","pcaa_exact_cpu","dense"],["portable","pcaa_exact_cpu_portable","dense"],
       ["structured","pcaa_exact_cpu","structured"]].each do |name,exe,variant|
        r = invoke([*pin,build.join(exe),variant,out.join("inputs",g.fetch("id")),seed,options[:limit],options[:samples],"vector"],timeout: options[:cpu_timeout])
        record(stream,{"id"=>g.fetch("id"),"seed"=>seed,"variant"=>name,"strategy"=>"branch"}.merge(r))
        samples = decoded(r).select { |v| v["type"] == "sample" }
        completed[[g.fetch("id"),seed]] = true if name == "avx2" && r.fetch("status") == "ok" && samples.length == options[:samples] && samples.all? { |v| v["status"] == 0 }
      end
    end
    # A bounded original/degree-only control sample; failures remain visible.
    if index < 20 || g.fetch("class") != "llvm"
      %w[current degree].each do |variant|
        r=invoke([*pin,build.join("pcaa_exact_cpu"),variant,out.join("inputs",g.fetch("id")),"none",options[:limit],3,"scalar"],timeout:options[:cpu_timeout])
        record(stream,{"id"=>g.fetch("id"),"seed"=>"none","variant"=>variant,"strategy"=>"branch_scalar_conditioning"}.merge(r))
      end
    end
    # Matched native heuristic exposes how exact work changes the CPU budget.
    r=invoke([*pin,build.join("pcaa_exact_cpu"),"dense",out.join("inputs",g.fetch("id")),"none",options[:limit],3,"vector","heuristic"],timeout:options[:cpu_timeout])
    record(stream,{"id"=>g.fetch("id"),"seed"=>"none","variant"=>"avx2","strategy"=>"heuristic"}.merge(r))
    if g.fetch("nodes") <= 6 && g.fetch("domain") <= 4
      r=invoke([*pin,build.join("pcaa_exact_cpu"),"dense",out.join("inputs",g.fetch("id")),"none",1000000,3,"vector","enumeration"],timeout:options[:cpu_timeout])
      record(stream,{"id"=>g.fetch("id"),"seed"=>"none","variant"=>"oracle","strategy"=>"enumeration"}.merge(r))
    end
    puts "CPU #{index+1}/#{graphs.length} #{g.fetch('id')}" if index%10==0
  end
end
Zlib::GzipWriter.open(out.join("model-runs.jsonl.gz")) do |stream|
  graphs.each_with_index do |g,index|
    %w[none heuristic].each do |seed|
      unless completed[[g.fetch("id"),seed]]
        record(stream,{"id"=>g.fetch("id"),"seed"=>seed,"status"=>"not_attempted_cpu_incomplete"})
        next
      end
      3.times do |sample|
        r=invoke([*pin,build.join("pcaa_exact_l2"),"dense",out.join("inputs",g.fetch("id")),seed,options[:limit],3,"vector"],timeout:options[:model_timeout])
        record(stream,{"id"=>g.fetch("id"),"seed"=>seed,"sample"=>sample}.merge(r))
      end
    end
    puts "L2 #{index+1}/#{graphs.length}" if index%10==0
  end
end
metadata["finished_utc"] = Time.now.utc.iso8601
write_json(out.join("metadata.json"),metadata)
puts "complete; run scripts/exact_pbqp_report.rb"
