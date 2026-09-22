#!/usr/bin/env ruby
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 PCAA contributors
# Analyzes fixed-point encoding of DIMACS road-graph coordinate distances.

require "zlib"

abort "usage: cost_representation_analyze.rb GRAPH.co.gz GRAPH.gr.gz" unless ARGV.length == 2

coordinates = {}
Zlib::GzipReader.open(ARGV[0]) do |file|
  file.each_line do |line|
    next unless line.start_with?("v ")

    fields = line.split
    coordinates[Integer(fields[1], 10)] =
      [Integer(fields[2], 10) / 1_000_000.0, Integer(fields[3], 10) / 1_000_000.0]
  end
end

earth_radius_metres = 6_371_000.0
distances = []
Zlib::GzipReader.open(ARGV[1]) do |file|
  file.each_line do |line|
    next unless line.start_with?("a ")

    fields = line.split
    first = coordinates.fetch(Integer(fields[1], 10))
    second = coordinates.fetch(Integer(fields[2], 10))
    first_latitude = first[1] * Math::PI / 180
    second_latitude = second[1] * Math::PI / 180
    latitude_delta = (second[1] - first[1]) * Math::PI / 180
    longitude_delta = (second[0] - first[0]) * Math::PI / 180
    haversine = Math.sin(latitude_delta / 2)**2 +
                Math.cos(first_latitude) * Math.cos(second_latitude) *
                Math.sin(longitude_delta / 2)**2
    distances << 2 * earth_radius_metres * Math.asin(Math.sqrt(haversine))
  end
end

sorted = distances.sort
quantile = lambda do |fraction|
  sorted[(fraction * (sorted.length - 1)).round]
end
errors = distances.map { |distance| ((distance * 1_000).round / 1_000.0 - distance).abs }
puts "count,min_m,p01_m,p25_m,median_m,p75_m,p99_m,max_m,mean_m,max_mm_error,mean_mm_error"
puts [distances.length, sorted.first, quantile.call(0.01), quantile.call(0.25),
      quantile.call(0.5), quantile.call(0.75), quantile.call(0.99), sorted.last,
      distances.sum / distances.length, errors.max, errors.sum / errors.length].join(",")
