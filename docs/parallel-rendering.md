# Parallel event preparation

Set `OBORODZUKI_WORKERS` to an integer from 1 through 8 before initializing a
renderer. The setting counts the calling thread. The default is the smaller of
four and the detected logical processor count. Each renderer owns its pool.
Workers are created only when at least twice that many events need preparation.
Cached geometry and small scenes run on the caller. Set 1 for the sequential
reference using the same binary.

## Execution and ownership

The caller selects events, evaluates overrides, resolves layout and collisions,
and creates ordered composition closures. A dynamic queue hands independent
events to workers. Their expensive work includes glyph paths, transforms, scan
conversion, outline widening, rasterization and blur. Each word is prepared at
the exact position, transform origin and subpixel phase of its first paint call.

Each renderer has a caller GDI device context and text cache. Each worker has its
own persistent GDI context and bounded text cache. Scoped thread-local bindings
restore the previous context on nested calls. Font selection restoration remains
in the existing text path. The override-token cache belongs to the caller.
Rasterizer paths, outlines and masks belong to each event's words.

A generation counter and condition variable wake the persistent workers. An
atomic next-task index balances events as workers finish. The caller also drains
the queue. The completion barrier publishes prepared geometry before the caller
composites events in their original layer and read order. Workers never write the
destination image. Shutdown joins every worker before renderer resources are
destroyed. Exceptions join the batch and return a rendering error.

External Lua files and direct Lua calls from ASS retain sequential execution.
This preserves Lua state and callback order. Gradients, clipping, custom blend
modes and existing effects use the normal ordered compositor. CSRI render and
format changes are synchronized per instance. Callers must finish outstanding
calls before closing an instance, as required by the API's handle lifetime.

The production project files do not enable OpenMP. For separately configured
OpenMP builds, event workers constrain filter parallelism to one OpenMP thread
and restore the previous setting afterward. This conditional configuration needs
its own validation. Production CI validates the persistent-pool configuration.

## Cache invalidation correction

The old CSRI wrapper used its video rectangle as the renderer's output-bounds
argument. Animated subtitle bounds consequently became the next frame's video
rectangle, forcing layout initialization and clearing caches. The wrapper now
stores output bounds separately. Legacy scripts using video-relative coordinates
can expose this pre-existing defect. The KFX and regression comparisons check for
unexpected pixel changes.

Font keys now populate the same LOGFONT fields directly. The generic style
conversion queried screen DPI to compute a height that the renderer immediately
overwrote. Removing that query eliminates shared screen-DC operations in a hot
worker path.

## Historical designs inspected

The local VSFilterMod `CSubPicQueue::ThreadProc` in
`src/subpic/ISubPic.cpp` prepares future subtitle frames asynchronously.
The [xy-VSFilter queue](https://github.com/pinterf/xy-VSFilter/blob/master/src/subpic/SubPicQueueImpl.cpp)
also has a CAMThread producer, wait events and calls to RenderTo.
These queues address frame delivery and overlap with playback. The event pool
added here operates inside an individual frame.

The [archived Threaded VSFilter project description](https://storage.googleapis.com/google-code-archive/v2/code.google.com/threaded-vsfilter/project.json)
describes multithreaded rasterization, rendering and duplicate-work caches. It
also explicitly documents a changed `\\be` result. This implementation retains
the existing algorithms and validates pixel equality. The archived implementation
was not imported.

## Validation and measurements

The existing Windows workflow continues to produce x86 and x64 DLLs. The
existing iteration suite checks original-VSFilterMod and baseline frame hashes.
The parallel validation workflow compares 1, 2, 4, 6 and 8 workers on one runner
with warmups and three alternating-order trials. It covers static and animated
text, 4–64 overlaps, borders, blur, clipping, gradients and mixed scripts.

The full opt-in suite renders all 5,400 KFX frames at each worker count with a
persistent instance, preserves per-frame timings and hashes, and compares the
current single-worker output with the previous optimized renderer when its
validated artifact is available. A separate existing KFX run also compares the
original renderer and Mangetsu on one runner.

Instrumented worker runs record task counts, task execution time, dispatch time,
wake latency, condition-variable sleep, mutex acquisition and completion-barrier
wait. Ordinary timed builds omit those counters. Sleep includes time between
frames. Wake latency includes condition-variable mutex reacquisition. Neither
quantity alone measures avoidable lock contention.

Six and eight workers on a four-logical-processor hosted runner are
oversubscribed. Those results do not establish scaling on six or eight available
cores. GDI resource counts and concurrent instance tests provide targeted Windows
diagnostics. They do not constitute thread-sanitizer coverage.
