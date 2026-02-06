#!/usr/bin/env ruby
# frozen_string_literal: true

#
# Benchmark: gem extraction with a native C tar library vs. pure-Ruby
#
# Compares four strategies for extracting .gem files:
#
#   1. Serial Ruby    — current RubyGems behavior (Gem::Package#extract_files)
#   2. Parallel Ruby  — 4 threads, but limited by GVL during I/O
#   3. Serial C ext   — GemTar.extract + GemTar.extract_gz
#   4. Parallel C ext — 4 threads with GVL released (true parallelism)
#
# Build the extension first:
#
#   cd ext/gem_tar && ruby extconf.rb && make && cd ../..
#
# Then run:
#
#   ruby -Iext/gem_tar benchmark/gem_extract.rb [gem_name ...]
#
# Gems are downloaded to /tmp/gem_bench_cache on first run and reused after.
#

require "rubygems"
require "rubygems/package"
require "rubygems/remote_fetcher"
require "rubygems/specification"
require "fileutils"
require "tmpdir"
require "benchmark"

begin
  require "gem_tar"
  HAS_C_TAR = true
rescue LoadError
  abort <<~MSG
    Could not load the gem_tar C extension.

    Build it first:
      cd ext/gem_tar && ruby extconf.rb && make && cd ../..

    Then re-run with:
      ruby -Iext/gem_tar benchmark/gem_extract.rb
  MSG
end

CACHE_DIR  = "/tmp/gem_bench_cache"
ITERATIONS = 5  # report best-of to reduce noise

# -- Gem fetching --------------------------------------------------------

def fetch_gem(name)
  cached = Dir.glob(File.join(CACHE_DIR, "#{name}-*.gem")).first
  return cached if cached

  dep = Gem::Dependency.new(name, Gem::Requirement.default)
  specs, = Gem::SpecFetcher.fetcher.spec_for_dependency(dep)
  return nil if specs.empty?

  spec, source = specs.max_by { |s, _| s.version }
  path = source.download(spec, CACHE_DIR)
  dest = File.join(CACHE_DIR, File.basename(path))
  FileUtils.mv(path, dest) unless path == dest
  dest
end

# -- Extraction strategies -----------------------------------------------

def extract_ruby(gem_path, dest)
  Gem::Package.new(gem_path).extract_files(dest)
end

def extract_c(gem_path, dest)
  Dir.mktmpdir("gem_outer") do |tmpdir|
    GemTar.extract(gem_path, tmpdir)
    GemTar.extract_gz(File.join(tmpdir, "data.tar.gz"), dest)
  end
end

# -- Helpers --------------------------------------------------------------

def format_bytes(bytes)
  if bytes >= 1_000_000 then format("%.1f MB", bytes / 1_000_000.0)
  elsif bytes >= 1_000  then format("%.1f KB", bytes / 1_000.0)
  else "#{bytes} B"
  end
end

def file_tree(dir)
  Dir.glob(File.join(dir, "**", "*"), File::FNM_DOTMATCH)
     .map { |f| f.delete_prefix(dir) }.sort
end

# -- Per-gem benchmark ----------------------------------------------------

def benchmark_gem(gem_path)
  name = File.basename(gem_path, ".gem")
  ruby_times = []
  c_times    = []
  match      = nil

  ITERATIONS.times do |i|
    rd = Dir.mktmpdir("ruby")
    cd = Dir.mktmpdir("cext")
    begin
      ruby_times << Benchmark.realtime { extract_ruby(gem_path, rd) }
      c_times    << Benchmark.realtime { extract_c(gem_path, cd) }
      match = (file_tree(rd) == file_tree(cd)) if i == 0
    ensure
      FileUtils.rm_rf(rd)
      FileUtils.rm_rf(cd)
    end
  end

  { name: name, size: File.size(gem_path),
    ruby: ruby_times.min, c: c_times.min,
    speedup: ruby_times.min / c_times.min, match: match }
end

# -- Batch benchmark (serial vs. parallel, Ruby vs. C) --------------------

def run_serial(gem_paths, method)
  dest = Dir.mktmpdir("batch")
  Benchmark.realtime do
    gem_paths.each do |gp|
      d = File.join(dest, File.basename(gp, ".gem"))
      FileUtils.mkdir_p(d)
      send(method, gp, d)
    end
  end
ensure
  FileUtils.rm_rf(dest)
end

def run_parallel(gem_paths, method, threads: 4)
  dest = Dir.mktmpdir("batch")
  Benchmark.realtime do
    q = Thread::Queue.new
    gem_paths.each { |gp| q << gp }
    threads.times  { q << :stop }
    Array.new(threads) {
      Thread.new do
        while (gp = q.pop)
          break if gp == :stop
          d = File.join(dest, File.basename(gp, ".gem"))
          FileUtils.mkdir_p(d)
          send(method, gp, d)
        end
      end
    }.each(&:value)
  end
ensure
  FileUtils.rm_rf(dest)
end

def benchmark_batch(gem_paths)
  labels = {
    ruby:     -> { run_serial(gem_paths, :extract_ruby) },
    par_ruby: -> { run_parallel(gem_paths, :extract_ruby) },
    c:        -> { run_serial(gem_paths, :extract_c) },
    par_c:    -> { run_parallel(gem_paths, :extract_c) },
  }
  labels.transform_values do |block|
    ITERATIONS.times.map { block.call }.min
  end
end

# -- Main ------------------------------------------------------------------

FileUtils.mkdir_p(CACHE_DIR)

default_gems = %w[
  nokogiri rbs aws-sdk-s3 rack activesupport
  actionpack activerecord railties sprockets
]
gem_names = ARGV.empty? ? default_gems : ARGV

$stderr.puts "Fetching gems..."
gem_paths = gem_names.filter_map do |name|
  $stderr.print "  #{name}... "
  path = fetch_gem(name)
  if path
    $stderr.puts format_bytes(File.size(path))
    path
  else
    $stderr.puts "not found"
    nil
  end
end

puts
puts "=" * 72
puts "PER-GEM EXTRACTION  (best of #{ITERATIONS})"
puts "=" * 72
puts format("  %-35s %8s %10s %10s %8s %5s",
            "Gem", "Size", "Ruby", "C ext", "Speedup", "Match")
puts "  " + "-" * 78

results = gem_paths.map do |path|
  r = benchmark_gem(path)
  puts format("  %-35s %8s %10s %10s %7.2fx  %s",
              r[:name], format_bytes(r[:size]),
              format("%.4fs", r[:ruby]), format("%.4fs", r[:c]),
              r[:speedup], r[:match] ? "Y" : "N")
  r
end

puts
puts "=" * 72
total = gem_paths.sum { |p| File.size(p) }
puts "BATCH EXTRACTION  #{gem_paths.size} gems, #{format_bytes(total)} total  (best of #{ITERATIONS})"
puts "=" * 72

b = benchmark_batch(gem_paths)
baseline = b[:ruby]
puts format("  %-30s %10s %10s", "", "Time", "vs baseline")
puts "  " + "-" * 52
[
  ["Serial Ruby",        b[:ruby]],
  ["Parallel Ruby (4T)", b[:par_ruby]],
  ["Serial C ext",       b[:c]],
  ["Parallel C ext (4T)",b[:par_c]],
].each do |label, time|
  puts format("  %-30s %10s %10s", label,
              format("%.3fs", time), format("%.2fx", baseline / time))
end
