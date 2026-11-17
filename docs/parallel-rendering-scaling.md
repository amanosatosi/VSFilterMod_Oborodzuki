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
speedup at four equally fast workers for that serial fraction. The observed
uninstrumented 1.64× implies an effective 48.2% serial fraction at four workers,
which absorbs scheduling and the higher cost of concurrent work.

Preparation took 22.398 seconds at two workers and 18.279 seconds at four.
Aggregate task time rose from 43.015 to 67.466 seconds. Widening alone grew from
36.034 to 57.445 aggregate seconds. At four workers the task-time totals ranged
from 16.800 to 17.023 seconds per worker, with approximately 92% of available
batch wall time spent in tasks. The runner's SMT topology and increased
aggregate CPU time constrain scaling. The collected counters do not isolate
cache misses, memory bandwidth, or execution-unit contention.

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
metric misses. These passes ran in sequence and do not isolate the cause of
the cold difference. [Short worker profile](https://github.com/amanosatosi/VSFilterMod_Oborodzuki/actions/runs/44196421088/artifacts/14379886024)
