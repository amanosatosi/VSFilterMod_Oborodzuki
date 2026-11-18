# Measuring and improving parallel rendering

## Baseline

The reference is `b110ccac5d1b9b55410b1d50df46dfaf29468503`, built as
Release (MOD) x64. The original validation used a Xeon Platinum 8573C runner
with two physical cores and four logical processors. The comparison workflow
reuses that run's DLL and measures it alongside the candidate on one runner.
[Baseline validation](https://github.com/amanosatosi/VSFilterMod_Oborodzuki/actions/runs/44151605518)

At one worker, the instrumented full KFX pass attributed 68.54 seconds to the
following mutually exclusive stages. Selection, animation, collision, and
cache costs were grouped inside the earlier bookkeeping and subtitle stages.
[Baseline worker profile](https://github.com/amanosatosi/VSFilterMod_Oborodzuki/actions/runs/44151605518/artifacts/14360131853)

| Stage | Total s | Share |
|---|---:|---:|
| Frame bookkeeping and effects | 0.557 | 0.8% |
| Subtitle parsing and layout | 10.558 | 15.4% |
| Font metrics and text measurement | 2.962 | 4.3% |
| Glyph path | 0.185 | 0.3% |
| Geometry transform | 0.086 | 0.1% |
| Scan conversion | 3.759 | 5.5% |
| Outline widening | 35.778 | 52.2% |
| Rasterization | 2.669 | 3.9% |
| Blur | 0.004 | 0.01% |
| Ordered blending | 11.984 | 17.5% |

Bookkeeping, subtitle processing, text measurement, and blending account for
an estimated 38.0% of the one-worker time. Amdahl's law gives an ideal 1.87×
speedup at four equally fast workers and an asymptotic ceiling of 2.63×
for that serial fraction. The observed
uninstrumented 1.64× implies an effective 48.2% serial fraction at four workers,
which absorbs scheduling and the higher cost of concurrent work.

Preparation took 22.398 seconds at two workers and 18.279 seconds at four.
Aggregate task time rose from 43.015 to 67.466 seconds. Widening alone grew from
36.034 to 57.445 aggregate seconds. At four workers the task-time totals ranged
from 16.800 to 17.023 seconds per worker, with approximately 92% of available
batch wall time spent in tasks. The runner's SMT topology and increased
aggregate CPU time constrain scaling. Cache misses, memory bandwidth, and
execution-unit contention remain unquantified.

Four-worker dispatch, completion wait, and mutex acquisition were 187.2,
1,064.4, and 7.4 ms across the pass, or 0.035, 0.197, and 0.0014 ms per
submitted frame. Completion wait includes outstanding event work. Inter-frame
condition-variable sleep includes hashing and host work. Blending stayed
serial at 12.013 seconds and reached 58.7% of elapsed time in the four-worker
mixed workload. [Baseline worker profile](https://github.com/amanosatosi/VSFilterMod_Oborodzuki/actions/runs/44151605518/artifacts/14360131853)

## Changes

- Event jobs capture complete shadow, outline, and fill draws. A mask replaced
  by a subpixel-phase change stays alive until blending completes. Banded
  compositing shares the existing masks directly.
- Bands have disjoint destination rows. They retain the complete clipped draw's
  coordinate system and run the original mixers in layer, read, and paint
  order. The caller retains Lua callbacks, collision resolution, and layout.
- The pool wakes selected workers. Blend jobs skip GDI and text-cache setup.
  The profiler separates batch wall time from summed stage and worker times.
- The parser defers Lua name conversion until a callback is found and uses
  direct interpolation for unit acceleration.
- Outline widening merges runs of identical, single-span rows through the
  widest required interval when the accumulated row has at most one span.
  The general path retains the existing span ordering and arithmetic.
- Font selection is restored during exception unwinding. Scan-conversion
  scratch allocation preserves the original buffer on allocation failure.
  A failed batch completes before cache cleanup. The profiler uses naturally
  aligned, padded per-worker counters, resolving MSVC C4316.

The short same-runner comparison at `a1cc349` passed sampled RGB tests and
the concurrent lifetime test. Median four-worker latency improved from
10.778 to 8.798 ms in the KFX window and from 125.288 to 89.835 ms in the
mixed scene. Mixed speedup rose from 1.49× to 2.18× at four workers.
[Short comparison](https://github.com/amanosatosi/VSFilterMod_Oborodzuki/actions/runs/44196421088)

On the separately instrumented 600-frame KFX window, four-worker preparation
took 2.557 seconds and band blending took 0.777 seconds out of 4.851 seconds
of wall time. The remaining 1.517 seconds include serial layout, override
evaluation, and band planning. Four glyph-path hit rates ranged from 97.6%
to 97.7%. The actual GDI path stage took 0.189 aggregate seconds, compared
with 8.015 aggregate seconds for widening. First-use font metrics can still
spike. The first one-worker pass spent 1.565 seconds on 140 cache misses,
while the later four-worker pass spent 0.011 seconds on the same number of
metric misses. The passes ran in sequence, and the cause of the first-use
difference remains unquantified. [Short worker profile](https://github.com/amanosatosi/VSFilterMod_Oborodzuki/actions/runs/44196421088/artifacts/14379886024)

## Stage and worker measurements for the acceptance revision

The separate instrumented run at `20c6174` rendered 600 consecutive frames
from the heavily overlapping 55–65 second interval. The one-worker pass took
11.073 seconds. At two and four workers, stage durations accumulate across
threads. Use the batch wall times below to account for overlap.
[Acceptance profile](https://github.com/amanosatosi/VSFilterMod_Oborodzuki/actions/runs/44199501549/artifacts/14380558535)

| Exclusive stage | 1 worker ms | Share of 1-worker elapsed | 2 workers summed ms | 4 workers summed ms |
|---|---:|---:|---:|---:|
| Active-event selection and ordering | 4.120 | 0.0% | 3.092 | 3.208 |
| ASS override parsing and evaluation | 1437.966 | 13.0% | 1369.704 | 1404.280 |
| Animation and position effects | 50.452 | 0.5% | 48.935 | 48.980 |
| GDI font metrics and selection | 586.659 | 5.3% | 13.748 | 13.695 |
| Text layout and cached measurement | 49.100 | 0.4% | 40.991 | 42.237 |
| Collision processing | 0.031 | 0.000% | 0.030 | 0.026 |
| Glyph path construction and cache copies | 48.913 | 0.4% | 55.852 | 166.743 |
| Geometry transform | 14.302 | 0.1% | 14.353 | 22.428 |
| Scan conversion | 498.737 | 4.5% | 548.025 | 806.793 |
| Outline widening | 5653.148 | 51.1% | 5858.916 | 10011.199 |
| Rasterization | 423.416 | 3.8% | 436.027 | 608.947 |
| Blur | 1.104 | 0.010% | 1.100 | 0.876 |
| Clip masks | 1.210 | 0.011% | 0.957 | 0.912 |
| Blend | 2104.909 | 19.0% | 2138.838 | 3489.615 |
| Cache lookup and maintenance | 24.608 | 0.2% | 22.811 | 33.129 |
| Paint setup and band planning | 27.559 | 0.2% | 50.717 | 73.629 |
| Other frame and subtitle work | 145.429 | 1.3% | 250.371 | 362.526 |

| Workers | Render wall ms | Preparation wall ms | Blending wall ms | Wall outside both batches ms | Dispatch ms | Caller completion wait ms | Mutex acquisition ms |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 2 | 6304.434 | 3573.14 | 1094.79 | 1636.50 | 15.013 | 89.764 | 0.426 |
| 4 | 5688.998 | 3081.37 | 937.954 | 1669.67 | 38.948 | 184.458 | 0.812 |

Dispatch and completion wait are included in the batch wall times. From two
to four workers, preparation and blending each gained about 16% throughput.
At four workers, 29.3% of elapsed time was outside those batches. Override
evaluation accounted for 1.404 of those 1.670 seconds. The collision
allocator had little work on this explicitly positioned interval.

Four workers spent 15.197 summed seconds in tasks, about 94.5% of the available
batch time. The four worker totals were 3.795, 3.832, 3.794, and 3.776 seconds.
Glyph path hits ranged from 97.6% to 97.7% per worker. GDI path work used about
1.0% of summed task time. The first-use font-metric spike remained visible at
one worker. The four-worker wake delay was 129.2 ms summed over the background
workers. Their 19.81 seconds in condition-variable waits include time between
frames. The profiler build completed without C4316.

A focused regression at `984cc14` exercised a Lua counter that changes
style and custom pixel output. The previous and current renderer produced
identical hashes at one, two, and four workers. A control with Lua disabled
produced different pixels. The renderer source was identical to `20c6174`.
[Stateful Lua validation](https://github.com/amanosatosi/VSFilterMod_Oborodzuki/actions/runs/44200942101)

## Full 90-second KFX acceptance

GitHub Actions reused the pinned `b110cca` release DLL and compared it with
`20c6174` on one AMD EPYC 9V74 Windows 2022 runner with two physical cores,
four logical processors, and a 16 GiB memory allocation on image
`20261112.366.7`. Both renderers used
Release (MOD) x64 and the same font environment, CSRI RGB32 output, and unchanged
`.github/ci/op.fx.4testing.ass` at 1920×1080 and 60 FPS. The fixture's SHA-256
was `b814f38e854f9dddf08226e6d1f943e74d2d24b63e2e000f5a0a85f8ac0d7ec3`.
Each process kept one instance through all 5,400 timestamps. Trial order
reversed between passes.
[Acceptance run](https://github.com/amanosatosi/VSFilterMod_Oborodzuki/actions/runs/44199501549)
[Raw timings, hashes, environment, and plot](https://github.com/amanosatosi/VSFilterMod_Oborodzuki/actions/runs/44199501549/artifacts/14383852209)

The table averages frame times across two complete trials per cell. Speedup
uses each revision's one-worker mean. Efficiency divides speedup by the
configured worker count.

| Workers | Previous mean ms | Current mean ms | Previous FPS | Current FPS | Previous speedup / efficiency | Current speedup / efficiency |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 9.535 | 9.391 | 104.9 | 106.5 | 1.00× / 100.0% | 1.00× / 100.0% |
| 2 | 6.536 | 5.456 | 153.0 | 183.3 | 1.46× / 72.9% | 1.72× / 86.1% |
| 4 | 5.998 | 4.743 | 166.7 | 210.8 | 1.59× / 39.7% | 1.98× / 49.5% |

Four-worker mean latency fell 20.9%, and two-worker latency fell 16.5%.
The one-worker mean fell 1.5%. Moving from two to four workers increased
throughput by 15.0%, up from 9.0% on the same runner. The four-worker p99
averaged 12.412 ms, down from 15.544 ms. Both new four-worker passes stayed
below 16.67 ms for every frame.

The four-worker process used 75.46 CPU seconds per pass on average, up from
73.52, and delivered 26.5% more frames per rendering second. Total CPU time
divided by render wall time rose from 2.27 to 2.95 logical-core equivalents.
Peak working set rose from 38.14 to 39.07 MiB across the four-worker passes.
Six and eight workers exercised correctness on this four-logical-processor
runner.

Every pass matched the pinned baseline's one-worker RGB hashes, including
the pre-threading reference. The job rendered 81,000 frames in 15 passes and
reported zero mismatches. The order, clipping, timing, and reinitialization
checks also passed, including concurrent seeking and rendering with padded and
negative row strides. The concurrent test measured 21 GDI objects before and
after teardown. [Acceptance run](https://github.com/amanosatosi/VSFilterMod_Oborodzuki/actions/runs/44199501549)
[Windows x86 and x64 builds](https://github.com/amanosatosi/VSFilterMod_Oborodzuki/actions/runs/44199501721)
[Renderer regressions](https://github.com/amanosatosi/VSFilterMod_Oborodzuki/actions/runs/44199501683)

| Revision | Workers | Trial | Mean ms | p50 ms | p95 ms | p99 ms | Max ms | Render s | FPS | Frames >16.67 / >33.33 ms | Longest >16.67 / >33.33 ms | CPU s | Peak MiB | RGB mismatches |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Previous | 1 | 1 | 9.588 | 9.083 | 20.530 | 27.558 | 36.288 | 51.77 | 104.3 | 610 / 5 | 71 / 1 | 51.02 | 33.23 | 0 |
| Previous | 1 | 2 | 9.482 | 9.014 | 20.368 | 27.144 | 30.375 | 51.20 | 105.5 | 586 / 0 | 69 / 0 | 51.81 | 32.96 | 0 |
| Previous | 2 | 1 | 6.546 | 6.297 | 13.237 | 17.296 | 23.082 | 35.35 | 152.8 | 94 / 0 | 14 / 0 | 51.72 | 34.55 | 0 |
| Previous | 2 | 2 | 6.526 | 6.245 | 13.358 | 17.308 | 24.844 | 35.24 | 153.2 | 94 / 0 | 14 / 0 | 50.98 | 33.91 | 0 |
| Previous | 4 | 1 | 5.993 | 5.779 | 11.981 | 15.542 | 17.110 | 32.36 | 166.9 | 2 / 0 | 1 / 0 | 73.58 | 38.08 | 0 |
| Previous | 4 | 2 | 6.002 | 5.758 | 12.006 | 15.547 | 18.289 | 32.41 | 166.6 | 5 / 0 | 2 / 0 | 73.47 | 38.14 | 0 |
| Current | 1 | 1 | 9.389 | 8.963 | 20.338 | 27.245 | 31.692 | 50.70 | 106.5 | 572 / 0 | 44 / 0 | 51.36 | 32.97 | 0 |
| Current | 1 | 2 | 9.393 | 8.966 | 20.402 | 27.119 | 30.877 | 50.72 | 106.5 | 576 / 0 | 56 / 0 | 51.52 | 33.00 | 0 |
| Current | 2 | 1 | 5.455 | 5.126 | 11.238 | 14.770 | 23.043 | 29.46 | 183.3 | 3 / 0 | 1 / 0 | 50.92 | 35.18 | 0 |
| Current | 2 | 2 | 5.456 | 5.118 | 11.256 | 14.670 | 18.609 | 29.46 | 183.3 | 2 / 0 | 1 / 0 | 51.00 | 34.89 | 0 |
| Current | 4 | 1 | 4.747 | 4.476 | 9.520 | 12.457 | 14.859 | 25.64 | 210.6 | 0 / 0 | 0 / 0 | 77.38 | 39.07 | 0 |
| Current | 4 | 2 | 4.739 | 4.460 | 9.536 | 12.367 | 14.556 | 25.59 | 211.0 | 0 / 0 | 0 / 0 | 73.55 | 37.75 | 0 |
| Current | 6 | 1 | 4.767 | 4.477 | 9.463 | 12.393 | 14.115 | 25.74 | 209.8 | 0 / 0 | 0 / 0 | 78.97 | 40.50 | 0 |
| Current | 8 | 1 | 4.779 | 4.472 | 9.638 | 12.499 | 14.299 | 25.81 | 209.3 | 0 / 0 | 0 / 0 | 76.92 | 40.11 | 0 |
| Pre-threading | 1 | 1 | 18.679 | 20.701 | 37.177 | 44.978 | 51.868 | 100.87 | 53.5 | 3352 / 365 | 391 / 33 | 101.48 | 30.19 | 0 |

## Synthetic workloads and KFX window

Median frame times are the medians of three trials, in milliseconds. The
last two columns report speedup and efficiency at two and four workers,
in that order. The full artifact includes p95, per-trial CPU times,
working sets, and hashes. [Synthetic results and artifacts](https://github.com/amanosatosi/VSFilterMod_Oborodzuki/actions/runs/44199501549/artifacts/14383852209)

| Workload | Previous ms at 1 / 2 / 4 | Current ms at 1 / 2 / 4 | Previous speedup / efficiency at 2 · 4 | Current speedup / efficiency at 2 · 4 |
|---|---:|---:|---:|---:|
| idle | 0.0001 / 0.0001 / 0.0001 | 0.0001 / 0.0001 / 0.0001 | 1.00× / 50.0% · 1.00× / 25.0% | 1.00× / 50.0% · 1.00× / 25.0% |
| static | 0.0473 / 0.0471 / 0.0470 | 0.0472 / 0.0469 / 0.0469 | 1.00× / 50.2% · 1.01× / 25.2% | 1.01× / 50.3% · 1.01× / 25.2% |
| animated | 1.7733 / 1.7819 / 1.7891 | 1.8074 / 1.8103 / 1.8324 | 1.00× / 49.8% · 0.99× / 24.8% | 1.00× / 49.9% · 0.99× / 24.7% |
| banner | 17.4370 / 11.6070 / 10.0284 | 17.7517 / 11.0201 / 9.4479 | 1.50× / 75.1% · 1.74× / 43.5% | 1.61× / 80.5% · 1.88× / 47.0% |
| scroll | 19.4914 / 13.9279 / 12.2797 | 19.5451 / 12.0405 / 10.3094 | 1.40× / 70.0% · 1.59× / 39.7% | 1.62× / 81.2% · 1.90× / 47.4% |
| blend | 51.6498 / 41.2769 / 38.4921 | 51.5624 / 27.8085 / 24.4521 | 1.25× / 62.6% · 1.34× / 33.5% | 1.85× / 92.7% · 2.11× / 52.7% |
| distortion | 21.6345 / 12.9895 / 10.7210 | 21.8859 / 12.2916 / 9.7381 | 1.67× / 83.3% · 2.02× / 50.4% | 1.78× / 89.0% · 2.25× / 56.2% |
| overlap-4 | 7.1708 / 4.0019 / 4.0051 | 7.2462 / 4.0229 / 4.0290 | 1.79× / 89.6% · 1.79× / 44.8% | 1.80× / 90.1% · 1.80× / 45.0% |
| overlap-8 | 13.4652 / 7.8225 / 6.4539 | 13.6332 / 7.4164 / 5.8815 | 1.72× / 86.1% · 2.09× / 52.2% | 1.84× / 91.9% · 2.32× / 57.9% |
| overlap-16 | 26.5857 / 16.0367 / 13.0158 | 27.1360 / 14.9568 / 11.8596 | 1.66× / 82.9% · 2.04× / 51.1% | 1.81× / 90.7% · 2.29× / 57.2% |
| overlap-32 | 54.4999 / 31.3349 / 25.8968 | 54.7253 / 29.4541 / 23.4269 | 1.74× / 87.0% · 2.10× / 52.6% | 1.86× / 92.9% · 2.34× / 58.4% |
| overlap-64 | 110.1273 / 62.9924 / 51.1614 | 111.2775 / 58.2784 / 46.0975 | 1.75× / 87.4% · 2.15× / 53.8% | 1.91× / 95.5% · 2.41× / 60.3% |
| border | 37.7107 / 21.6191 / 18.1938 | 38.2271 / 20.6380 / 16.9203 | 1.74× / 87.2% · 2.07× / 51.8% | 1.85× / 92.6% · 2.26× / 56.5% |
| blur | 65.8608 / 36.7895 / 32.0537 | 66.0423 / 35.3741 / 30.2479 | 1.79× / 89.5% · 2.05× / 51.4% | 1.87× / 93.3% · 2.18× / 54.6% |
| clip | 27.4278 / 16.3968 / 13.2701 | 27.7626 / 15.3506 / 12.1326 | 1.67× / 83.6% · 2.07× / 51.7% | 1.81× / 90.4% · 2.29× / 57.2% |
| gradient | 27.9396 / 17.2737 / 14.1842 | 28.2941 / 15.5861 / 12.2653 | 1.62× / 80.9% · 1.97× / 49.2% | 1.82× / 90.8% · 2.31× / 57.7% |
| mixed | 157.6267 / 116.2714 / 105.4541 | 158.7692 / 85.6307 / 70.9660 | 1.36× / 67.8% · 1.49× / 37.4% | 1.85× / 92.7% · 2.24× / 55.9% |
| random | 27.1579 / 16.2649 / 13.2810 | 27.5155 / 15.2170 / 12.2004 | 1.67× / 83.5% · 2.04× / 51.1% | 1.81× / 90.4% · 2.26× / 56.4% |
| lua | 26.7971 / 26.9147 / 26.7351 | 26.9953 / 27.1013 / 27.0709 | 1.00× / 49.8% · 1.00× / 25.1% | 1.00× / 49.8% · 1.00× / 24.9% |
| kfx-sample | 13.4302 / 9.2496 / 8.4515 | 13.4243 / 7.8529 / 6.7621 | 1.45× / 72.6% · 1.59× / 39.7% | 1.71× / 85.5% · 1.99× / 49.6% |

At 64 overlaps, four workers gained 26.4% throughput over two and reached
2.41× speedup. Four-worker latency in the mixed clipping and gradient fixture
fell 32.7%, and the custom-blend fixture improved by 36.5%. Idle and static
one-worker medians stayed within 0.0001 ms of baseline. The single animated
fixture's one-worker median rose by 0.034 ms, or 1.9%. The four-overlap
fixture used two active preparation workers at both budgets.

## Remaining limits

The 2.8× four-worker target was above the 2.41× best synthetic result on this
runner. From the paired full pass means, the Amdahl effective serial fraction
at four workers fell from 50.5% to 34.0%, using
`(T4 / T1 - 1/4) / (3/4)`. The effective fraction includes resource-sharing
costs. The short profile attributes the largest remaining caller cost to
override evaluation. The largest parallel cost is outline widening, which
rose from 5.859 to 10.011 summed seconds across workers from two to four.
Task execution took longer with SMT siblings active. The contributions from
scheduling, cache and bandwidth pressure, and shared execution units remain
unquantified. The latency deltas measure the combined renderer changes.
Six- and eight-worker scaling remains unmeasured on a runner with the
required logical processor count.

## Revisions

- [Renderer changes from the reference to the acceptance revision](https://github.com/amanosatosi/VSFilterMod_Oborodzuki/compare/b110ccac5d1b9b55410b1d50df46dfaf29468503...20c6174b9cc15292f77ea99605f22df39faf539c)
- [Lua regression fixture](https://github.com/amanosatosi/VSFilterMod_Oborodzuki/commit/984cc14dc88f18c7dc77fd8b0c7df8242f8feb55)
