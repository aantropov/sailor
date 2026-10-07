# Engine measurements

Benchmarks are optional command-line programs, not editor commands or CTest
correctness gates. Build explicitly:

```sh
cmake -S . -B build -DSAILOR_BUILD_BENCHMARKS=ON
cmake --build build --config Release --target SailorMemoryBenchmark
cmake --build build --config Release --target SailorContainerBenchmark
```

Use the repository's existing platform/toolchain options when configuring a new
build. On macOS, select the built runtime if it is not on the loader search path:

```sh
DYLD_LIBRARY_PATH="$PWD/build/Lib/Release" \
  Binaries/Release/SailorMemoryBenchmark --count 4096 --min-size 1 --max-size 4096 --iterations 5
```

## Allocators

SailorMemoryBenchmark prints raw CSV samples for HeapAllocator, LockFreeHeapAllocator and
MallocAllocator. Every allocator receives the same seeded size sequence and
16-byte alignment. Simple frees in order; shuffle frees in a seeded permutation;
interleaved frees alternate blocks from the first half before allocating the rest.
Generation of sizes/order and checksum validation are outside the timed regions.
Allocation timing includes touching the first and last payload bytes. Free timing
includes the release loop. Initialization and allocator destruction are not timed.
The shared heap can retain thread-local pools between samples; this is not a
fresh-process or cold-cache comparison. No synthetic score ranks the allocators.

`payload_bytes` is the sum of requested bytes, not resident or peak memory.
Windows reports absolute process-private bytes before allocation and at the live
sampling point. That metric includes the rest of the process and is not allocator
overhead. Other platforms report `unavailable`, not zero. The live sample is not
a measured peak. Successful rows say `measured`; checksum/allocation failure exits
nonzero. A successful measurement does not prove allocator correctness.

The default workload is modest. Larger distributions and counts are explicit CLI
choices; use separate invocations for small, medium, large or mixed allocations.
Keep correctness checks in `SmartPointerTests`: alignment, byte preservation,
fragmented reuse, growth and cross-thread release. Timing has no pass threshold.
The old runtime HTML/JavaScript report and its derived scores are intentionally
replaced by raw CSV, with no browser or network dependency.

## Containers

```sh
DYLD_LIBRARY_PATH="$PWD/build/Lib/Release" \
  Binaries/Release/SailorContainerBenchmark --count 4096 --iterations 3
```

The container program measures vector append, swap removal, ordered erase,
first-value removal and insertion; list push/pop at alternating ends and removal
of all matching values; set hit/miss lookups and removal; map insertion, value
updates, mixed lookups and removal. Each pair uses the same seeded inputs and
operation order. Payloads contain 512 inline bytes or an independently owned
512-byte allocation. Both list implementations remove from the same end; value
removal includes duplicates and preserves survivor order. No variant reserves
capacity in advance. Counts below 256 leave some fractional erase/insert phases
empty; use at least 512 to exercise every operation.

Rows contain raw elapsed microseconds, operation counts, thread counts and an
observed checksum, not a synthetic score. Sequence order, every map value and
exact membership are checked outside timing. Lookup results are consumed and
checked, not discarded. The octree is resolved while populated; ray results are
compared with an independent axis-aligned segment/bounds oracle after mutations.
The plain octree has no STL counterpart and is not presented as a paired result.

TConcurrentMap uses four workers with disjoint keys and real distinct writes;
its timed regions include thread creation, the start barrier and joining.
The STL map and TMap rows are single-thread measurements, not equivalent parallel
implementations or a scheduler benchmark. TConcurrentSet rows use one thread and
measure the synchronized API's single-thread cost. No result claims engine FPS.
Data generation, reference replay, validation, output and final container
destruction are outside timing; allocation and payload work inside each operation
are included. Fixed ordering and reused process allocator state mean these are
not cold-cache or statistically unbiased rankings.

Mismatch exits nonzero. A `measured` row only validates that particular workload;
small `ContainerContractTests` and `ConcurrentContainerTests` remain the correctness
gates, with no elapsed-time threshold. Benchmark programs are not registered with
CTest. The old runtime console commands, DLL benchmark exports and reflected
BenchmarkTestCaseComponent are replaced by this standalone CLI. There are no
project-owned scenes referencing the removed component.
