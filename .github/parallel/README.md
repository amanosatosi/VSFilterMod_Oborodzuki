# Runtime worker validation

The feature-branch workflow compares the candidate with the renderer from
`b110ccac5d1b9b55410b1d50df46dfaf29468503` on the same runner. It reuses the
release DLL from the completed validation run and executes three alternating-order
synthetic trials. Each process reads `OBORODZUKI_WORKERS` before its first
visible render. A new process supplies an independent renderer instance per trial.

The full dispatch input or a `Parallel-KFX full` commit marker enables the
5,400-frame acceptance test. It alternates both renderer revisions and each
supported worker count through two full passes. An independent pass checks
candidate RGB hashes for configurations above the runner's logical processor
count. Every pass uses the original 1920×1080 fixture and checks the complete
sequence against the baseline's one-worker hashes.

The results artifact includes per-frame CSVs, hashes, captures, latency summaries,
CPU and memory measurements, and a full-timeline chart. A
`Parallel-KFX quick` commit marker selects a subset of synthetic workloads
for iteration. A `Parallel-KFX profile` marker runs the instrumented renderer
over frames 3300 through 3899 and the mixed fixture at one, two, and four workers.
Use batch wall times to measure elapsed parallel work. Individual stage times
accumulate across workers and can exceed that wall time.
