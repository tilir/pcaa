#!/usr/bin/env ruby
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 PCAA contributors
# Regenerates exact-search completion, accounting and conditional CPU/L2 tables.
require "csv"
require "digest"
require "json"
require "open3"
require "optparse"
require "pathname"
require "zlib"
module ExactReport
  ROOT = Pathname.new(__dir__).parent
  PHASES = %w[descriptor decode protection operand add1 add2 tree merge writeback drain memory_wait control].freeze
  FREQUENCIES = [250,500,1000,2000].freeze
  CANDIDATES = %w[avx2 portable structured].freeze
  module_function
  def median(values)
    raise "empty samples" if values.empty?
    values.sort[values.length/2].to_f
  end
  def quantile(values,p)
    return nil if values.empty?
    values.sort[[(p*values.length).ceil-1,0].max].to_f
  end
  def gzip(path)
    Zlib::GzipReader.open(path) { |s| s.each_line.map { |l| JSON.parse(l) } }
  end
  def parse(run)
    run.fetch("stdout","").lines.filter_map { |l| JSON.parse(l) rescue nil }
  end
  def csv(rows)
    columns=rows.flat_map(&:keys).uniq
    CSV.generate { |c| c << columns;rows.each { |r| c << columns.map { |k| r[k].is_a?(Hash) || r[k].is_a?(Array) ? JSON.generate(r[k]) : r[k] } } }
  end
  def table(columns,rows)
    ([columns,columns.map { "---" }]+rows).map { |r| "| #{r.join(' | ')} |" }.join("\n")+"\n"
  end
  def number(v,digits=3)
    v.nil? || !v.finite? ? "—" : format("%.#{digits}f",v)
  end
  def classify(run)
    return "timeout" if run.fetch("status") == "timeout"
    return "capacity/error" unless run.fetch("status") == "ok"
    samples=parse(run).select { |v| v["type"] == "sample" }
    return "capacity/error" if samples.empty?
    statuses=samples.map { |v| v.fetch("status") }.uniq
    raise "unstable status" unless statuses.length==1
    return "capacity/error" if statuses.first==-5 && samples.any? { |v|v.fetch("workspace_failures",0)>0 }
    {0=>"solved-exact",-5=>"search-node-limit"}.fetch(statuses.first,"capacity/error")
  end
  def cpu(run,expected_samples)
    return nil unless classify(run)=="solved-exact"
    rows=parse(run);samples=rows.select { |r| r["type"]=="sample" }
    raise "sample coverage" unless samples.length==expected_samples
    raise "unstable answer" unless samples.map { |r| r.slice("objective","assignment") }.uniq.length==1
    profiles=rows.select { |r| r["type"]=="profile" };events=rows.select { |r| r["type"]=="events" }
    raise "diagnostic coverage" unless profiles.length==3 && events.length==3
    signature=->(sample) { sample.fetch("events").map { |r| r.slice("phase","kind","count","elements","operand_bytes","result_bytes") } }
    raise "unstable stream" unless events.map { |r| signature.call(r) }.uniq.length==1
    raise "unstable work" unless (profiles+events).map { |r| r.fetch("work") }.uniq.length==1
    floor=rows.find { |r| r["type"]=="calibration" }.fetch("ns")
    batches=events.first.fetch("events").each_with_index.map do |r,i|
      raw=median(events.map { |s| s.fetch("events").fetch(i).fetch("ns") })
      r.merge("raw_ns"=>raw,"ns"=>[raw-floor,0].max)
    end
    components=profiles.first.fetch("components").keys.to_h { |k| [k,median(profiles.map { |r| r.fetch("components").fetch(k) })] }
    total=median(samples.map { |r| r.fetch("ns") });kernel=batches.sum { |r| r.fetch("ns") }
    {total:total,kernel:kernel,batches:batches,work:events.first.fetch("work"),components:components,
     answer:samples.first.slice("objective","assignment"),p10:quantile(samples.map { |r|r.fetch("ns") },0.1),
     p90:quantile(samples.map { |r|r.fetch("ns") },0.9),seed_ns:median(samples.map { |r|r.fetch("seed_ns") }),
     first:median(events.map { |r|r.fetch("first_solution_ns") }),found:median(events.map { |r|r.fetch("optimum_found_ns") }),
     proof:median(events.map { |r|r.fetch("ns") }),profile_total:median(profiles.map { |r|r.fetch("ns") }),
     event_total:median(events.map { |r|r.fetch("ns") }),floor:floor,allocations:events.first.fetch("allocations"),
     allocated_bytes:events.first.fetch("allocated_bytes"),peak:events.first.fetch("peak_workspace")}
  end
  def model(runs,native)
    return nil if runs.length != 3 || runs.any? { |r|r.fetch("status")!="ok" }
    values=runs.map do |run|
      rows=parse(run);sample=rows.find { |r|r["type"]=="model" };base=rows.find { |r|r["type"]=="baseline" }.fetch("l2")
      raise "model status" unless sample.fetch("status")==0
      raise "model answer" unless sample.slice("objective","assignment")==native.fetch(:answer)
      raise "model tree" unless sample.fetch("work")==native.fetch(:work)
      expected=native.fetch(:batches).map { |r|r.slice("phase","kind","count","elements","operand_bytes","result_bytes") }
      actual=sample.fetch("events").map { |r|r.slice("phase","kind","count","elements","operand_bytes","result_bytes") }
      raise "model stream mismatch" unless actual==expected
      counters=sample.fetch("l2").to_h do |key,value|
        counter=key.end_with?("_cycles","_count","_bytes","_requests") ||
          %w[cycles submissions batches children tiles chunks lane_groups active_elements lane_slots tail_slots state_updates requests split_transfers unaligned_splits contiguous_transfers gather_elements shared_loads protection_elements child_barriers].include?(key)
        [key,counter ? value-base.fetch(key) : value]
      end
      raise "cycle sum" unless sample.fetch("events").sum { |r|r.fetch("cycles") }==counters.fetch("cycles")
      raise "phase sum" unless PHASES.sum { |p|counters.fetch("#{p}_cycles") }==counters.fetch("cycles")
      raise "request sum" unless %w[descriptor_requests operand_requests result_requests].sum { |k|counters.fetch(k) }==counters.fetch("requests")
      raise "byte sum" unless %w[descriptor_bytes operand_bytes result_bytes].sum { |k|counters.fetch(k) }==counters.fetch("transferred_bytes")
      raise "lane partition" unless counters.fetch("active_elements")+counters.fetch("tail_slots")==counters.fetch("lane_slots")
      sample.fetch("events").each { |r| raise "callback phase sum" unless r.fetch("phases").sum==r.fetch("cycles") }
      [sample,counters]
    end
    raise "nondeterministic L2" unless values.map(&:last).uniq.length==1
    events=values.first.first.fetch("events").each_with_index.map do |r,i|
      r.merge(%w[host_ns staging_ns build_ns readback_ns].to_h { |k| [k,median(values.map { |v|v.first.fetch("events").fetch(i).fetch(k) })] })
    end
    search=median(values.map { |v| v.first.fetch("ns")-v.first.fetch("events").sum { |e|e.fetch("ns") } })
    {counters:values.first.last,events:events,search:search}
  end
  def correlation(xs,ys)
    return nil if xs.length<3
    mx=xs.sum.fdiv(xs.length);my=ys.sum.fdiv(ys.length)
    dx=xs.map { |x|x-mx };dy=ys.map { |y|y-my }
    denominator=Math.sqrt(dx.sum { |x|x*x }*dy.sum { |y|y*y })
    denominator.positive? ? dx.zip(dy).sum { |x,y|x*y }/denominator : nil
  end
  def generate(data)
    metadata=JSON.parse(data.join("metadata.json").read)
    raise "unfinished archive" unless metadata.key?("finished_utc")
    raw=gzip(data.join("cpu-runs.jsonl.gz"));models=gzip(data.join("model-runs.jsonl.gz")).group_by { |r|r.values_at("id","seed") }
    indexed=raw.to_h { |r| [r.values_at("id","seed","variant","strategy"),r] }
    statuses=raw.map do |r|
      status=classify(r)
      status="solved-heuristic" if r.fetch("strategy")=="heuristic" && status=="solved-exact"
      {"id"=>r.fetch("id"),"seed"=>r.fetch("seed"),"variant"=>r.fetch("variant"),"strategy"=>r.fetch("strategy"),"status"=>status,"subprocess_status"=>r.fetch("status")}
    end
    inputs=metadata.fetch("graphs"); expected=metadata.fetch("options").fetch("samples")
    native={}
    raw.each do |r|
      count=r.fetch("strategy")=="branch" ? expected : 3
      native[r.values_at("id","seed","variant","strategy")]=cpu(r,count)
    end
    inputs.each do |g|
      %w[none heuristic].each do |seed|
        candidates=CANDIDATES.map { |v|native.fetch([g.fetch("id"),seed,v,"branch"]) }.compact
        raise "CPU optimum mismatch" if candidates.map { |v|v.fetch(:answer) }.uniq.length>1
        raise "CPU tree mismatch" if candidates.map { |v|v.fetch(:work) }.uniq.length>1
      end
      witnesses=native.select { |(id,_,_,strategy),v|id==g.fetch("id") && v && strategy!="heuristic" }.values
      raise "oracle/seed optimum mismatch" if witnesses.map { |v|v.fetch(:answer).fetch("objective") }.uniq.length>1
    end
    common=inputs.select { |g| CANDIDATES.all? { |v|native[[g.fetch("id"),"none",v,"branch"]] } }
    strong=CANDIDATES.min_by { |v| common.sum { |g|native.fetch([g.fetch("id"),"none",v,"branch"]).fetch(:total) } }
    rows=[];batches=[];profiles=[];scenarios=[];counterfactuals=[]
    inputs.each do |g|
      %w[none heuristic].each do |seed|
        id=g.fetch("id");n=native[[id,seed,strong,"branch"]]
        row=g.reject { |k,_|%w[source generation].include?(k) }.merge("seed"=>seed,"strong_variant"=>strong,
          "cpu_status"=>classify(indexed.fetch([id,seed,strong,"branch"])))
        model_runs=models.fetch([id,seed],[])
        row["model_status"]=model_runs.all? { |r|r["status"]=="ok" } && model_runs.length==3 ? "completed" : model_runs.map { |r|r["status"] }.uniq.join(";")
        if n
          row.merge!("cpu_ns"=>n.fetch(:total),"kernel_ns"=>n.fetch(:kernel),"control_ns"=>[n.fetch(:total)-n.fetch(:kernel),0].max,
            "kernel_exceeds_solve"=>n.fetch(:kernel)>n.fetch(:total),"p10_ns"=>n.fetch(:p10),"p90_ns"=>n.fetch(:p90),
            "seed_ns"=>n.fetch(:seed_ns),"first_solution_ns"=>n.fetch(:first),"optimum_found_ns"=>n.fetch(:found),
            "proof_observation_ns"=>n.fetch(:proof),"profile_perturbation"=>n.fetch(:profile_total)/n.fetch(:total),
            "events_perturbation"=>n.fetch(:event_total)/n.fetch(:total),"allocations"=>n.fetch(:allocations),
            "allocated_bytes"=>n.fetch(:allocated_bytes),"peak_workspace_bytes"=>n.fetch(:peak),"objective"=>n.fetch(:answer).fetch("objective"))
          n.fetch(:work).each { |k,v|row["work_#{k}"]=v }
          CANDIDATES.each { |v|row["#{v}_ns"]=native[[id,seed,v,"branch"]]&.fetch(:total) }
          n.fetch(:components).each { |k,v|profiles<<{"id"=>id,"class"=>g.fetch("class"),"seed"=>seed,"phase"=>k,"diagnostic_ns"=>v} }
          m=model(model_runs,n)
          if m
            cycles=m.fetch(:counters).fetch("cycles");host=m.fetch(:events).sum { |e|e.fetch("host_ns") }
            current_host=m.fetch(:search)+host
            row.merge!("l2_cycles"=>cycles,"host_prepare_ns"=>host,"model_host_search_ns"=>m.fetch(:search),
              "current_host_ns"=>current_host,"ideal_break_even_mhz"=>n.fetch(:kernel).positive? ? cycles*1000.0/n.fetch(:kernel) : nil,
              "current_break_even_mhz"=>n.fetch(:total)>current_host ? cycles*1000.0/(n.fetch(:total)-current_host) : nil,
              "ideal_amdahl"=>row.fetch("control_ns").positive? ? n.fetch(:total)/row.fetch("control_ns") : nil,
              "current_host_amdahl"=>current_host.positive? ? n.fetch(:total)/current_host : nil)
            m.fetch(:counters).each { |k,v|row["l2_#{k}"]=v }
            m.fetch(:events).each_with_index do |event,i|
              c=n.fetch(:batches)[i]
              batches<<{"id"=>id,"class"=>g.fetch("class"),"seed"=>seed,"index"=>i,"cpu_ns"=>c.fetch("ns"),
                "simulation_callback_wall_ns"=>event.fetch("ns")}.merge(event.reject { |k,_|k=="ns" })
            end
            FREQUENCIES.each do |f|
              service=cycles*1000.0/f
              scenarios<<{"id"=>id,"class"=>g.fetch("class"),"seed"=>seed,"mhz"=>f,"cpu_ns"=>n.fetch(:total),
                "ideal_total_ns"=>row.fetch("control_ns")+service,"current_total_ns"=>current_host+service,
                "ideal_speedup"=>n.fetch(:total)/(row.fetch("control_ns")+service),"current_speedup"=>n.fetch(:total)/(current_host+service)}
            end
            counters=m.fetch(:counters)
            removed={"none"=>0,"free_descriptor_frontend"=>counters.fetch("descriptor_cycles")+counters.fetch("decode_cycles")+counters.fetch("descriptor_requests"),
              "zero_memory_wait"=>counters.fetch("memory_wait_cycles"),"free_operand_service"=>counters.fetch("operand_cycles")+counters.fetch("operand_requests"),
              "free_arithmetic"=>counters.fetch("add1_cycles")+counters.fetch("add2_cycles"),
              "free_tree_merge"=>counters.fetch("tree_cycles")+counters.fetch("merge_cycles"),
              "free_conditioning_device"=>m.fetch(:events).select { |e|e.fetch("phase")=="conditioning" }.sum { |e|e.fetch("cycles") }}
            removed.each { |name,cut|counterfactuals<<{"id"=>id,"class"=>g.fetch("class"),"seed"=>seed,"counterfactual"=>name,"remaining_cycles"=>cycles-cut,
              "ideal_mhz"=>n.fetch(:kernel).positive? ? (cycles-cut)*1000.0/n.fetch(:kernel) : nil} }
          end
        end
        heuristic=native[[id,"none","avx2","heuristic"]]
        if heuristic
          row["heuristic_cpu_ns"]=heuristic.fetch(:total);row["heuristic_kernel_ns"]=heuristic.fetch(:kernel)
          row["heuristic_elements"]=heuristic.fetch(:batches).sum { |e|e.fetch("elements") }
        end
        rows<<row
      end
    end
    summary={"strong_variant"=>strong,"input_count"=>inputs.length,"llvm_inputs"=>inputs.count { |g|g.fetch("class")=="llvm" },
      "search_limit"=>metadata.fetch("options").fetch("limit"),"cohorts"=>[]}
    tables="# Generated exact PBQP tables\n\n## Completion (strong CPU)\n\n"
    cohort_rows=[]
    %w[llvm synthetic stress].product(%w[none heuristic]).each do |klass,seed|
      all=rows.select { |r|r.fetch("class")==klass && r.fetch("seed")==seed }
      completed=all.select { |r|r["l2_cycles"] }
      cpu_ns=completed.sum { |r|r.fetch("cpu_ns") };kernel=completed.sum { |r|r.fetch("kernel_ns") };cycles=completed.sum { |r|r.fetch("l2_cycles") }
      freqs=completed.filter_map { |r|r["ideal_break_even_mhz"] }.select(&:positive?)
      cfreq=completed.filter_map { |r|r["current_break_even_mhz"] }.select(&:positive?)
      status=all.group_by { |r|r.fetch("cpu_status") }.transform_values(&:length)
      cohort={"class"=>klass,"seed"=>seed,"inputs"=>all.length,"cpu_status"=>status,"matched_completed"=>completed.length,
        "cpu_ns"=>cpu_ns,"kernel_ns"=>kernel,"control_ns"=>completed.sum { |r|r.fetch("control_ns") },"l2_cycles"=>cycles,
        "current_host_ns"=>completed.sum { |r|r.fetch("current_host_ns") },"host_prepare_ns"=>completed.sum { |r|r.fetch("host_prepare_ns") },
        "kernel_share"=>cpu_ns.positive? ? kernel/cpu_ns : nil,"weighted_ideal_mhz"=>kernel.positive? ? cycles*1000.0/kernel : nil,
        "median_mhz"=>quantile(freqs,0.5),"p75_mhz"=>quantile(freqs,0.75),"p90_mhz"=>quantile(freqs,0.9),"max_mhz"=>freqs.max,
        "geomean_mhz"=>freqs.empty? ? nil : Math.exp(freqs.sum { |f|Math.log(f) }/freqs.length),
        "finite_current_break_even"=>cfreq.length,"ideal_amdahl"=>cpu_ns.positive? ? cpu_ns/cohort_control(completed) : nil,
        "median_profile_perturbation"=>completed.empty? ? nil : median(completed.map { |r|r.fetch("profile_perturbation") }),
        "median_events_perturbation"=>completed.empty? ? nil : median(completed.map { |r|r.fetch("events_perturbation") })}
      FREQUENCIES.each do |f|
        cohort["ideal_win_fraction_#{f}"]=completed.empty? ? nil : completed.count { |r| r.fetch("l2_cycles").positive? && r.fetch("l2_cycles")*1000.0/f<r.fetch("kernel_ns") }.fdiv(completed.length)
        cohort["current_win_fraction_#{f}"]=completed.empty? ? nil : completed.count { |r|r.fetch("l2_cycles").positive? && r.fetch("current_host_ns")+r.fetch("l2_cycles")*1000.0/f<r.fetch("cpu_ns") }.fdiv(completed.length)
      end
      summary.fetch("cohorts")<<cohort;cohort_rows<<cohort
    end
    tables<<table(["Class","Incumbent","Inputs","Exact CPU","Search limit","Timeout/error","Matched L2"],cohort_rows.map { |c|
      [c["class"],c["seed"],c["inputs"],c["cpu_status"].fetch("solved-exact",0),c["cpu_status"].fetch("search-node-limit",0),
       c["cpu_status"].fetch("timeout",0)+c["cpu_status"].fetch("capacity/error",0),c["matched_completed"]] })
    tables<<"\n## Matched end-to-end budgets\n\n"+table(["Class","Seed","CPU ms","Kernel ms","Kernel share","Control ms","L2 cycles","Ideal MHz","Amdahl max","Current host ms"],cohort_rows.map { |c|
      [c["class"],c["seed"],number(c["cpu_ns"]/1e6),number(c["kernel_ns"]/1e6),number((c["kernel_share"]||0)*100)+"%",number(c["control_ns"]/1e6),c["l2_cycles"],number(c["weighted_ideal_mhz"]),number(c["ideal_amdahl"]),number(c["current_host_ns"]/1e6)] })
    tables<<"\n## Break-even distributions (positive ideal frequencies only)\n\n"+table(["Class","Seed","Median MHz","Geomean MHz","p75 MHz","p90 MHz","Max MHz","Finite current break-even"],cohort_rows.map { |c|
      [c["class"],c["seed"],*%w[median_mhz geomean_mhz p75_mhz p90_mhz max_mhz].map { |k|number(c[k]) },c["finite_current_break_even"]] })
    tables<<"\n## Win fractions among matched completed inputs\n\n"+table(["Class","Seed","250 MHz ideal/current","500 MHz ideal/current","1 GHz ideal/current","2 GHz ideal/current"],cohort_rows.map { |c|
      [c["class"],c["seed"],*FREQUENCIES.map { |f| "#{number(100*(c["ideal_win_fraction_#{f}"]||0),1)}% / #{number(100*(c["current_win_fraction_#{f}"]||0),1)}%" }] })
    phase_rows=profiles.group_by { |r|r.values_at("class","seed","phase") }.map { |(c,s,p),values|{"class"=>c,"seed"=>s,"phase"=>p,"diagnostic_ns"=>values.sum { |r|r.fetch("diagnostic_ns") }} }
    tables<<"\n## Exclusive diagnostic CPU phases (intrusive; all completed native solves)\n\n"+table(["Class","Seed","Phase","Profile ms"],phase_rows.map { |r|[r["class"],r["seed"],r["phase"],number(r["diagnostic_ns"]/1e6)] })
    mix=batches.group_by { |r|r.values_at("class","seed","phase","kind") }.map do |(c,s,p,k),values|
      {"class"=>c,"seed"=>s,"phase"=>p,"kind"=>k,"batches"=>values.length,"batch_jobs"=>values.sum { |r|r.fetch("count") },
       "elements"=>values.sum { |r|r.fetch("elements") },"logical_operand_bytes"=>values.sum { |r|r.fetch("operand_bytes") },
       "logical_result_bytes"=>values.sum { |r|r.fetch("result_bytes") },"cpu_ns"=>values.sum { |r|r.fetch("cpu_ns") },"cycles"=>values.sum { |r|r.fetch("cycles") }}
    end
    tables<<"\n## Exact operation mix (matched solves)\n\n"+table(["Class","Seed","Phase","Callback","Batches","Elements","CPU ms","L2 cycles"],mix.map { |r|
      [r["class"],r["seed"],r["phase"],r["kind"],r["batches"],r["elements"],number(r["cpu_ns"]/1e6),r["cycles"]] })
    predictors=[]
    %w[llvm synthetic stress].each do |klass|
      subset=rows.select { |r|r["class"]==klass && r["seed"]=="none" && r["ideal_break_even_mhz"]&.positive? }
      %w[nodes domain matrix_elements work_visited work_depth work_pruned work_r1 work_r2 work_conditioning work_clone_bytes work_lower_matrix].each do |field|
        xs=subset.map { |r|Math.log(1+r.fetch(field).to_f) };ys=subset.map { |r|Math.log(r.fetch("ideal_break_even_mhz")) }
        predictors<<{"class"=>klass,"predictor"=>field,"completed_pairs"=>subset.length,"log_pearson_r"=>correlation(xs,ys)}
      end
    end
incumbent_rows=inputs.map do |g|
  id=g.fetch("id");a=native[[id,"none",strong,"branch"]];b=native[[id,"heuristic",strong,"branch"]]
  h=native[[id,"none","avx2","heuristic"]]
  r={"id"=>id,"class"=>g.fetch("class"),"none_status"=>classify(indexed.fetch([id,"none",strong,"branch"])),
     "seeded_status"=>classify(indexed.fetch([id,"heuristic",strong,"branch"])),"heuristic_ns"=>h&.fetch(:total)}
  {"none"=>a,"seeded"=>b}.each do |name,x|
    next unless x
    r["#{name}_ns"]=x.fetch(:total);r["#{name}_visited"]=x.fetch(:work).fetch("visited")
    r["#{name}_objective"]=x.fetch(:answer).fetch("objective")
  end
  if a && b
    r["algorithmic_speedup"]=a.fetch(:total)/b.fetch(:total)
    r["visited_reduction"]=a.fetch(:work).fetch("visited").fdiv(b.fetch(:work).fetch("visited"))
    r["seed_ns"]=b.fetch(:seed_ns)
  end
  r["exact_vs_heuristic_time_ratio"]=a.fetch(:total)/h.fetch(:total) if a && h
  r
end
controls=raw.select { |r|%w[current degree].include?(r.fetch("variant")) }.map do |r|
  id=r.fetch("id");v=r.fetch("variant");x=native[[id,"none",v,"branch_scalar_conditioning"]];strong_run=native[[id,"none",strong,"branch"]]
  if x && strong_run
    raise "control tree mismatch" unless x.fetch(:work)==strong_run.fetch(:work)
  end
  {"id"=>id,"variant"=>v,"status"=>classify(r),"cpu_ns"=>x&.fetch(:total),
   "strong_ns"=>strong_run&.fetch(:total),"speedup"=>x && strong_run ? x.fetch(:total)/strong_run.fetch(:total) : nil}
end
    outputs={"workloads.csv"=>csv(rows),"statuses.csv"=>csv(statuses),"batches.csv"=>csv(batches),"profiles.csv"=>csv(profiles),
      "phase-summary.csv"=>csv(phase_rows),"operation-mix.csv"=>csv(mix),"scenarios.csv"=>csv(scenarios),"counterfactuals.csv"=>csv(counterfactuals),
      "incumbents.csv"=>csv(incumbent_rows),"controls.csv"=>csv(controls),"predictors.csv"=>csv(predictors),"summary.json"=>JSON.pretty_generate(summary)+"\n","tables.md"=>tables}
    [outputs,metadata]
  end
  def cohort_control(completed)
    [completed.sum { |r|r.fetch("control_ns") },1.0].max
  end
  def verify(data,metadata)
    data.join("SHA256SUMS").read.lines.each do |l|
      sha,name=l.chomp.split("  ",2);raise "checksum #{name}" unless Digest::SHA256.file(data.join(name)).hexdigest==sha
    end
    metadata.fetch("input_sha256").each { |p,h|raise "input hash" unless Digest::SHA256.file(data.join(p)).hexdigest==h }
    metadata.fetch("implementation_sha256").each do |p,h|
      out,error,status=Open3.capture3("tar","-xOzf",data.join("implementation.tar.gz").to_s,p)
      raise "source member #{p}: #{error}" unless status.success? && Digest::SHA256.hexdigest(out)==h
    end
  end
end
if $PROGRAM_NAME==__FILE__
  options={data:"doc/reports/data/exact-pbqp",verify:false}
  OptionParser.new { |p|p.on("--data PATH") { |v|options[:data]=v };p.on("--verify") { options[:verify]=true } }.parse!
  abort "unexpected arguments" unless ARGV.empty?
  data=ExactReport::ROOT.join(options[:data]);outputs,metadata=ExactReport.generate(data)
  if options[:verify]
    ExactReport.verify(data,metadata)
    outputs.each { |p,text|raise "generated drift #{p}" unless data.join(p).read==text }
    puts "verified #{metadata.fetch('graphs').length} inputs, bounded statuses, CPU/oracle/model answers, trees, streams, cycle/byte/lane partitions, hashes and regenerated tables"
  else
    outputs.each { |p,text|data.join(p).write(text) }
    names=data.glob("**/*").select(&:file?).map { |p|p.relative_path_from(data).to_s }.reject { |p|p=="SHA256SUMS" }.sort
    data.join("SHA256SUMS").write(names.map { |p|"#{Digest::SHA256.file(data.join(p)).hexdigest}  #{p}\n" }.join)
    puts "regenerated #{outputs.length} outputs"
  end
end
