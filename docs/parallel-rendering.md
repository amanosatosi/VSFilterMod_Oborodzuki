# Parallel event preparation

Set `OBORODZUKI_WORKERS` to an integer from 1 through 8 before initializing a
renderer. The setting counts the calling thread. The default is the smaller of
four and the detected logical processor count. Each renderer owns its pool.
Preparation uses up to one worker per two tasks, bounded by the configured
limit. Frames with fewer than four preparation tasks use the caller. For eight
or more events on a 32-bit surface with independent rows, the pool can also
prepare complete draws and blend across groups of rows. The blend worker count
depends on the clipped pixel area and the number of occupied row groups. Set 1
for the sequential reference using the same binary.

## Execution and ownership

The caller selects events, evaluates overrides, and resolves layout and
collisions. A dynamic queue hands independent events to workers. Their work
includes glyph paths, transforms, scan conversion, outline widening,
rasterization and blur. The preparation path uses the exact position, transform
origin and subpixel phase of the first paint call.

Each renderer has a caller GDI device context and text cache. Each worker has its
own persistent GDI context and bounded text cache. Scoped thread-local bindings
restore the previous context on nested calls. Font selection restoration remains
in the existing text path. The override-token cache belongs to the caller.
Rasterizer paths, outlines and masks belong to each event's words.

A generation counter and per-worker condition variables wake the selected
workers. An atomic next-task index balances tasks as workers finish. The
caller also drains the queue. Shutdown joins every worker before renderer
resources are destroyed. Exceptions join the batch and return a rendering
error after resetting the cached layout.

On eligible frames, each event records its shadow, outline and fill draws in
paint order. Each draw borrows its mask and snapshots its clipped bounds and
blend parameters. An event retains any mask replaced by a subpixel phase change
through the end of blending. Each row group visits the intersecting draws in
original ASS layer, read and paint order and writes directly to its own
destination rows. The same fixed-point mixers handle serial and parallel
painting, including destination-dependent blend modes. The original clipped
row coordinates preserve vector-mask, gradient and PNG sampling.

External Lua files and direct Lua calls from ASS retain sequential execution.
This preserves Lua state and callback order. Gradients, clipping, custom blend
modes and existing effects use the normal ordered compositor. CSRI render and
format changes are synchronized per instance. Callers must finish outstanding
calls before closing an instance, as required by the API's handle lifetime.

The production project files do not enable OpenMP. For separately configured
OpenMP builds, event workers constrain filter parallelism to one OpenMP thread
and restore the previous setting afterward. This conditional configuration needs
its own validation. Production CI validates the persistent-pool configuration.

Identical vector clips share immutable alpha masks in the caller's geometry cache.
That cache retains at most eight masks and 16 MiB. Banner and scroll fades detach
a writable copy before changing pixels. Normal masked compositing skips empty
glyph coverage and fully clipped pixels before evaluating gradients. Custom blend
modes keep their existing path.

## Cache invalidation correction

The old CSRI wrapper used its video rectangle as the renderer's output-bounds
argument. Animated subtitle bounds consequently became the next frame's video
rectangle, forcing layout initialization and clearing caches. The wrapper now
stores output bounds separately. Legacy scripts using video-relative coordinates
can expose this pre-existing defect. The KFX and regression comparisons check for
unexpected pixel changes.

The scrolling-effect regression also exposed an existing switch fallthrough.
Scroll processing entered vector-clip handling and read uninitialized timing
fields from its Effect object. A break now ends the scroll case. This removes
nondeterministic clipping independently of the event scheduler.

Font keys now populate the same LOGFONT fields directly. The generic style
conversion queried screen DPI to compute a height that the renderer immediately
overwrote. Removing that query eliminates shared screen-DC operations in a hot
worker path.

Before user Lua code runs, the tag parser snapshots the actual standard-library
global function names. Ordinary tags then avoid repeated Lua VM lookups. A direct
Lua call or loaded user script restores dynamic lookup and sequential callback
ordering.

## Historical designs inspected

The local VSFilterMod `CSubPicQueue::ThreadProc` in
`src/subpic/ISubPic.cpp` prepares future subtitle frames asynchronously.
The [xy-VSFilter queue](https://github.com/pinterf/xy-VSFilter/blob/master/src/subpic/SubPicQueueImpl.cpp)
also has a CAMThread producer, wait events and calls to RenderTo.
These queues address frame delivery and overlap with playback. The event pool
added here operates inside an individual frame.

The XySubFilter provider wrapper handles a consumer's frame request, reuses a
valid cached frame, then locks the provider around RenderEx when it needs fresh
output. This separates consumer delivery from the renderer's internal event work.
Its wrapper supplies no same-frame event scheduler. [Provider source](https://github.com/Cyberbeing/xy-VSFilter/blob/xy_sub_filter_rc5/src/subpic/XySubRenderProviderWrapper.cpp)

The [archived Threaded VSFilter project description](https://storage.googleapis.com/google-code-archive/v2/code.google.com/threaded-vsfilter/project.json)
describes multithreaded rasterization, rendering and duplicate-work caches. It
also explicitly documents a changed `\\be` result. This implementation retains
the existing algorithms and validates pixel equality. The archived implementation
was not imported.

## Validation and measurements

The existing Windows workflow continues to produce x86 and x64 DLLs. The
existing iteration suite checks original-VSFilterMod and baseline frame hashes.
The parallel validation workflow compares the previous and current renderer on
one runner with warmups and three alternating-order trials. It covers empty
frames, static and animated text, 4–64 overlaps, borders, blur, clipping,
gradients and mixed scripts.

The full opt-in suite renders all 5,400 KFX frames at each worker count with a
persistent instance, preserves per-frame timings and hashes, and repeats the
same-runner comparison for the two parallel renderer revisions. Each candidate
pass must match the original parallel renderer's one-worker RGB hashes. A
separate existing KFX workflow compares the original renderer and Mangetsu.

Instrumented worker runs record task counts, task execution time, dispatch time,
wake latency, condition-variable sleep, mutex acquisition and completion-barrier
wait. Ordinary timed builds omit those counters. Sleep includes time between
frames. Wake latency includes condition-variable mutex reacquisition. Neither
quantity alone measures avoidable lock contention.

Six and eight workers on a four-logical-processor hosted runner are
oversubscribed. Those results do not establish scaling on six or eight available
cores. GDI resource counts and concurrent instance tests provide targeted Windows
diagnostics. They do not constitute thread-sanitizer coverage.
