#!/usr/bin/env ruby
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 PCAA contributors
# Bounds the complete freestanding verification call chain conservatively from GCC stack reports.
require 'fileutils'
require 'json'
require 'open3'
require 'pathname'
root = Pathname.new(__dir__).parent
out = Pathname.new(ARGV.fetch(0, '/tmp/exact-stack-final')).expand_path
FileUtils.mkdir_p(out)
common = %w[-march=rv64imac -mabi=lp64 -mcmodel=medany -ffreestanding -fno-builtin -nostdlib -nostartfiles -O2 -fstack-usage]
common += %w[accelerator/include software software/pbqp/freestanding pcaalib/include].map { |p| "-I#{root.join(p)}" }
sources = %w[software/pbqp/pbqp.cpp software/pbqp/pbqp_storage.cpp accelerator/src/cost_math.cpp software/accel_driver.c software/pbqp/pbqp_accelerator.c software/tests/runtime.c software/tests/basic.c software/tests/randomized.c software/tests/pbqp_basic.c software/tests/pbqp_randomized.c software/tests/pbqp_rn.c pcaalib/src/pcaa.c pcaalib/src/codec.c pcaalib/src/submission.c pcaalib/src/device.c pcaalib/src/baremetal_device.c]
commands = sources.map do |source|
  cpp = source.end_with?('.cpp')
  command = [cpp ? 'riscv64-unknown-elf-g++' : 'riscv64-unknown-elf-gcc', *common, cpp ? '-std=c++17' : '-std=c11']
  command += %w[-fno-exceptions -fno-rtti -fno-threadsafe-statics] if cpp
  command += ['-c', root.join(source).to_s, '-o', out.join(File.basename(source).sub(/\.[^.]+$/, '.o')).to_s]
  stdout, stderr, status = Open3.capture3(*command)
  raise stderr unless status.success?
  { source: source, command: command, stdout: stdout, stderr: stderr }
end
entries = out.glob('*.su').flat_map do |file|
  file.readlines.map do |line|
    name, size, kind = line.chomp.split("\t")
    { name: name, size: Integer(size), kind: kind, file: file.basename.to_s }
  end
end
raise 'unbounded dynamic frame' unless entries.all? { |e| %w[static dynamic,bounded].include?(e[:kind]) }
# Sum every emitted frame from every translation unit, including mutually exclusive
# test programs and callbacks. Then add 65 copies of every known recursive frame.
# This deliberately overbounds a 64-node graph rather than relying on inlining.
recursive = entries.select { |e| e[:name].match?(/SolveBranchAndReduce|EnumerateActiveCore|::Enumerate\(|pbqp_reference_enumerate/) }
base = entries.sum { |e| e[:size] }
recursion = 65 * recursive.sum { |e| e[:size] }
reserve = 1 << 20
result = { commands: commands, entries: entries, recursive_frames: recursive,
           frame_sum_bytes: base, recursive_allowance_bytes: recursion,
           conservative_bound_bytes: base + recursion, startup_reserve_bytes: reserve,
           scope: 'All PBQP/driver/pcaalib/runtime and five verification programs; 64-node recursion. No asynchronous interrupts or user callbacks. ABI startup uses the documented reserve.' }
raise 'stack reserve exceeded' unless base + recursion < reserve
out.join('stack.json').write(JSON.pretty_generate(result) + "\n")
puts JSON.generate(result.reject { |k,_| %i[commands entries].include?(k) })
