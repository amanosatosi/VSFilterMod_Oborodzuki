# KFX Stress Test

The event-worker feature branch accepts the same explicit verification marker.
Set `OBORODZUKI_WORKERS` before constructing a renderer to select its worker count.
The separate Parallel renderer validation workflow compares all worker counts.
Its `full` dispatch input or explicit `Parallel-KFX full` verification marker
retains 5,400 frames per worker configuration and a one-worker reference pass.
The worker regression suite also exercises scrolling and banner masks, randomized
geometry, and Lua callback fallback before full playback comparisons.

In GitHub Actions, select **KFX Stress Test**, choose the development branch, and run
one pass per renderer by default. Set **repetitions** to 2–5 for additional passes.
The **profile** input adds a separate instrumented Oborodzuki playback. Its result
does not gate the raw measurements or the performance report.

GitHub exposes the **Run workflow** button after the workflow file is on the default
branch. While it lives only on the development branch, a push that changes this
workflow or its KFX support files can explicitly request a verification run by
including `KFX-Stress-Test verification` in its commit message. Ordinary pushes
never run the playback.

The workflow verifies the exact fixture hash, loads the supplied ASS file wholesale,
and submits frames 0–5399 in order to one live instance per pass at 1920×1080.
Each engine sees the same integer millisecond timestamps derived from 60 FPS.
Benchmarks exclude hashing, captures and frame clearing from render latency.

The **kfx-raw** artifact contains native frame CSVs, renderer diagnostics, build and
runner metadata, and selected frames. The **kfx-report** artifact contains the
combined frame CSV, comparison and stutter statistics, charts, and PNG captures.
The comparison checks every baseline–candidate RGB frame hash and selected pixels.
Mangetsu timings use the exact fixture but its pixel output is assessed separately.
