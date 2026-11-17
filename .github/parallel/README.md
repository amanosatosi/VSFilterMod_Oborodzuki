# Runtime worker validation

The normal feature-branch run repeats warm synthetic comparisons three times in
alternating worker order. The full dispatch input or an explicit
`Parallel-KFX full` marker on a commit changing this directory also runs all
5,400 KFX frames at every configured worker count. All settings use the same DLL
and comparison runner. Each process reads `OBORODZUKI_WORKERS` before its first
visible render. A new process supplies an independent cold instance per trial.

The results artifact includes raw native CSVs, hashes, captures, latency summaries,
CPU and memory measurements, and a full-timeline chart. The separate profile job
provides worker task distribution and synchronization counters. Its times include
instrumentation overhead and are intended for diagnosis.

Counts above the recorded logical processor count are oversubscribed. Small
workloads may deliberately use the calling thread alone. Efficiency percentages
for those adaptive cases describe the configured budget, while the execution
strategy avoids allocating idle workers.
