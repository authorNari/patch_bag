# Make pseudo flagmentation in a ruby objects heap.
GC::Profiler.enable if $DEBUG

M = 1000000
B = 2 * 5 * (4**3) + 1
A = 100001

def make_fragmentation(h, seed)
  i = seed
  10000.times {|m|
    h << Object.new
  }
  10000.times {|m|
    i = ((A * i) + B) % M
    h[i % h.length] = nil
  }
end

heaps = []
100.times{|i| make_fragmentation(heaps, i) }

if $DEBUG
  GC::Profiler.report
  puts GC::Profiler.total_time
end
