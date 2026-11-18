"""Turn the serial per-frame KFX captures into summaries, charts and regressions."""
import csv
import json
import math
import os
from pathlib import Path
import statistics

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from PIL import Image, ImageChops

from fixture import FPS, FRAMES


RAW = Path("kfx-raw")
DEST = Path("kfx-report")
DEST.mkdir(exist_ok=True)
LABELS = {
    "original": "Original VSFilterMod",
    "baseline": "Oborodzuki baseline",
    "candidate": "Current Oborodzuki",
    "mangetsu-legacy": "Mangetsu legacy",
    "mangetsu-rgba": "Mangetsu RGBA",
}
COLORS = dict(zip(LABELS, ["#737d8b", "#bb5e10", "#1769aa", "#27865b", "#874bb7"], strict=True))
BUDGETS = (16.67, 33.33)
BINS = (0, 4, 8, *BUDGETS, 50, 100, math.inf)
BIN_LABELS = ("0–4", "4–8", "8–16.67", "16.67–33.33", "33.33–50", "50–100", ">100")


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def at(frame):
    ms = frame * 1000 // FPS
    return f"{ms // 60000:02}:{ms // 1000 % 60:02}.{ms % 1000:03}"


def interval(start, stop):
    return {"first_frame": start, "last_frame": stop - 1, "frames": stop - start,
            "start": at(start), "end_exclusive": at(stop)}


def slow_runs(values, budget):
    starts = []
    start = None
    for frame, ms in enumerate([*values, 0]):
        if ms > budget and start is None:
            start = frame
        elif ms <= budget and start is not None:
            run = interval(start, frame)
            run["peak_ms"] = max(values[start:frame])
            starts.append(run)
            start = None
    return sorted(starts, key=lambda run: (run["frames"], run["peak_ms"]), reverse=True)[:5]


def worst_windows(values, active):
    ranked = []
    for i in range(FRAMES - FPS + 1):
        samples = values[i:i + FPS]
        ranked.append((sum(sample > BUDGETS[0] for sample in samples), statistics.mean(samples), i))
    worst = []
    for slow, mean, i in sorted(ranked, reverse=True):
        if any(i < window["last_frame"] + 1 and i + FPS > window["first_frame"] for window in worst):
            continue
        window = interval(i, i + FPS)
        window.update(slow_frames=slow, mean_ms=mean, peak_ms=max(values[i:i + FPS]),
                      mean_active_events=statistics.mean(active[i:i + FPS]))
        worst.append(window)
        if len(worst) == 5:
            break
    return worst


def analyze(values, active, metadata):
    ranked = sorted(values)
    total = sum(values)
    return {
        **metadata,
        "average_ms": total / FRAMES,
        "median_ms": statistics.median(values),
        "p95_ms": ranked[math.ceil(0.95 * FRAMES) - 1],
        "p99_ms": ranked[math.ceil(0.99 * FRAMES) - 1],
        "maximum_ms": max(values),
        "total_render_ms": total,
        "render_fps": FRAMES * 1000 / total,
        "cpu_percent_of_one_core": 100 * metadata["render_cpu_ms"] / total,
        "over_60fps_budget": sum(v > BUDGETS[0] for v in values),
        "over_30fps_budget": sum(v > BUDGETS[1] for v in values),
        "slow_runs_60fps": slow_runs(values, BUDGETS[0]),
        "slow_runs_30fps": slow_runs(values, BUDGETS[1]),
        "worst_one_second_windows": worst_windows(values, active),
        "slowest_20_frames": [
            {"frame": i, "timestamp": at(i), "render_ms": values[i], "active_events": active[i]}
            for i in sorted(range(FRAMES), key=values.__getitem__, reverse=True)[:20]
        ],
        "latency_distribution": dict(zip(BIN_LABELS, [
            sum(lo < v <= hi or lo == 0 == v for v in values) for lo, hi in zip(BINS, BINS[1:])
        ], strict=True)),
    }


plan = read_json(RAW / "input.json")
environment = read_json(RAW / "environment.json")
active = plan["active_events"]
passes = read_json(RAW / "passes.json")
all_data = {}
all_metrics = {}
hashes = {}
capture_paths = {}
diagnostics = []
with (DEST / "all-frames.csv").open("w", newline="", encoding="utf-8") as merged:
    writer = csv.writer(merged)
    writer.writerow(("engine", "pass", "frame", "presentation_ns", "renderer_timestamp_ms",
                     "render_ms", "cpu_ticks_100ns", "rgb_hash", "active_ass_events"))
    for entry in passes:
        name, trial = entry["engine"], entry["pass"]
        directory = RAW / entry["directory"]
        with (directory / "frames.csv").open(newline="", encoding="utf-8") as source:
            rows = list(csv.DictReader(source))
        if len(rows) != FRAMES:
            raise RuntimeError(f"Incomplete playback for {name} pass {trial}")
        key = (name, trial)
        values = [float(row["render_ms"]) for row in rows]
        hashes[key] = [row["rgb_hash"] for row in rows]
        all_data[key] = values
        all_metrics[key] = analyze(values, active, read_json(directory / "metrics.json"))
        log = (directory / "renderer.log").read_text(encoding="utf-8", errors="replace")
        suspicious = [line for line in log.splitlines() if any(
            word in line.casefold() for word in ("malformed", "unsupported", "invalid", "error", "failed", "unknown")
        )]
        if suspicious:
            diagnostics.append({"engine": name, "pass": trial, "messages": list(dict.fromkeys(suspicious))})
        writer.writerows((name, trial, row["frame"], row["presentation_ns"], row["renderer_timestamp_ms"],
                          row["render_ms"], row["cpu_ticks_100ns"], row["rgb_hash"], active[int(row["frame"])])
                         for row in rows)
        if trial == 1:
            captures = DEST / "captures" / name
            captures.mkdir(parents=True, exist_ok=True)
            for frame in plan["capture_frames"]:
                with Image.open(directory / f"frame-{frame}.bmp") as image:
                    dest = captures / f"frame-{frame}.png"
                    image.convert("RGB").save(dest)
                    capture_paths[name, frame] = dest

trials = sorted({trial for _, trial in all_data})
regression = []
for trial in trials:
    candidate = hashes["candidate", trial]
    baseline = hashes["baseline", trial]
    bad_frames = [i for i, (c, b) in enumerate(zip(candidate, baseline, strict=True)) if c != b]
    regression.append({"pass": trial, "mismatched_frames": [
        {"frame": i, "timestamp": at(i), "baseline_hash": baseline[i], "candidate_hash": candidate[i]}
        for i in bad_frames
    ]})

selected = {}
for name in LABELS:
    differing = []
    for frame in plan["capture_frames"]:
        with Image.open(capture_paths["baseline", frame]) as baseline_image, Image.open(capture_paths[name, frame]) as actual:
            diff = ImageChops.difference(baseline_image, actual)
            if diff.getbbox():
                differing.append(frame)
                if name == "candidate":
                    diff.save(DEST / f"regression-diff-{frame}.png")
    selected[name] = differing

times = [i / FPS for i in range(FRAMES)]
# Every chart uses pass 1 so each trace represents a single uninterrupted playback.
fig, ax = plt.subplots(figsize=(16, 6), layout="constrained")
for name in LABELS:
    ax.plot(times, all_data[name, 1], linewidth=0.8, color=COLORS[name], alpha=0.85, label=LABELS[name])
for budget in BUDGETS:
    ax.axhline(budget, color="#333333", linestyle="--", linewidth=0.9, label=f"{budget:.2f} ms budget")
ax.set(xlim=(0, 90), xlabel="Presentation time (seconds)", ylabel="Frame time (ms)", title="KFX playback frame times")
ax.legend(ncol=3, loc="upper right")
fig.savefig(DEST / "frame-time-timeline.png", dpi=160)
plt.close(fig)

fig, ax = plt.subplots(figsize=(16, 4), layout="constrained")
ax.fill_between(times, active, step="post", color="#236a94", alpha=0.78)
ax.set(xlim=(0, 90), ylim=(0, None), xlabel="Presentation time (seconds)",
       ylabel="Active dialogue events", title="ASS event count during playback")
fig.savefig(DEST / "active-event-count.png", dpi=160)
plt.close(fig)

fig, ax = plt.subplots(figsize=(12, 5), layout="constrained")
for ordinal, name in enumerate(LABELS):
    counts = list(all_metrics[name, 1]["latency_distribution"].values())
    ax.bar([i + (ordinal - 2) * 0.16 for i in range(len(counts))], counts, width=0.16,
           color=COLORS[name], label=LABELS[name])
ax.set(xticks=range(len(BIN_LABELS)), xticklabels=BIN_LABELS, xlabel="Frame time bin (ms)",
       ylabel="Frame count", title="Distribution over 5,400 frames")
ax.legend()
fig.savefig(DEST / "frame-time-distribution.png", dpi=160)
plt.close(fig)

candidate = all_metrics["candidate", 1]
fig, axes = plt.subplots(len(candidate["worst_one_second_windows"]), 1, figsize=(13, 11), layout="constrained")
for ax, window in zip(axes, candidate["worst_one_second_windows"], strict=True):
    start, stop = window["first_frame"], window["last_frame"] + 1
    for name in ("baseline", "candidate"):
        ax.plot(times[start:stop], all_data[name, 1][start:stop], color=COLORS[name], label=LABELS[name])
    for budget in BUDGETS:
        ax.axhline(budget, color="#333333", linestyle="--", linewidth=0.8)
    ax.set(ylabel="ms", title=f'{window["start"]} to {window["end_exclusive"]}, {window["slow_frames"]}/60 over 16.67 ms')
axes[0].legend(loc="upper right")
axes[-1].set_xlabel("Presentation time (seconds)")
fig.savefig(DEST / "worst-sections.png", dpi=160)
plt.close(fig)

registered = {font.casefold() for font in environment["registered_fonts"]}
missing = [font for font in plan["style_fonts"] if font.casefold() not in registered]
summary = [
    "# KFX Stress Test", "",
    f'Input **{Path(plan["fixture"]).name}** · SHA-256 {plan["sha256"]}',
    f'{plan["dialogue_events"]:,} dialogue events, {plan["comment_events"]:,} comments. '
    f'Peak activity {max(active)} dialogue events at {at(active.index(max(active)))}.',
    f'{plan["zero_duration_events"]:,} zero-duration dialogue events were supplied unchanged '
    'and have no active frames.',
    f"{len(trials)} pass(es) per renderer, 5,400 chronological timestamps per pass at 60 FPS, 1920×1080.",
    "Frames 0–5399 cover [00:00.000, 01:30.000). The engines receive the same floored integer millisecond timestamp.",
    "Timing includes rendering and composition. Buffer clearing, hashes and frame captures are excluded. No video was decoded.",
    "The CPU column is sampled process CPU time around render calls. Peak RSS includes the host.",
    "Mangetsu legacy uses the sampled benchmark's reference scalar BGR compositor. RGBA uses the native Mangetsu compositor.",
    "The fixture uses 1vc and 3vc gradient tags. Mangetsu performance is reported without assuming pixel equivalence.",
    "Missing fixture style fonts in the runner registry: " + (", ".join(missing) if missing else "none") + ".",
    "CSRI accepted the complete input byte stream. Its API does not expose a parsed event count. "
    "Both Mangetsu paths reported the expected event count.",
    "", "## Performance by pass", "",
    "| Renderer | Pass | Mean ms | Median ms | p95 ms | p99 ms | Max ms | Render s | FPS | "
    "CPU s | Peak MiB | >16.67 ms | >33.33 ms |",
    "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
]
for trial in trials:
    for name in LABELS:
        m = all_metrics[name, trial]
        summary.append(f'| {LABELS[name]} | {trial} | {m["average_ms"]:.3f} | {m["median_ms"]:.3f} | '
                       f'{m["p95_ms"]:.3f} | {m["p99_ms"]:.3f} | {m["maximum_ms"]:.3f} | '
                       f'{m["total_render_ms"] / 1000:.2f} | {m["render_fps"]:.1f} | '
                       f'{m["render_cpu_ms"] / 1000:.2f} | {m["peak_working_set_bytes"] / 2**20:.1f} | '
                       f'{m["over_60fps_budget"]:,} | {m["over_30fps_budget"]:,} |')
summary += ["", "## Worst one-second windows for current Oborodzuki, pass 1", "",
            "| Start | End (exclusive) | Frames over 16.67 ms | Mean ms | Peak ms | Mean active events |",
            "|---|---|---:|---:|---:|---:|"]
for row in candidate["worst_one_second_windows"]:
    summary.append(f'| {row["start"]} | {row["end_exclusive"]} | {row["slow_frames"]} | '
                   f'{row["mean_ms"]:.2f} | {row["peak_ms"]:.2f} | {row["mean_active_events"]:.1f} |')
summary += ["", "## Longest consecutive slow runs for current Oborodzuki, pass 1", ""]
for key, budget in (("slow_runs_60fps", 16.67), ("slow_runs_30fps", 33.33)):
    runs = candidate[key]
    if runs:
        longest = runs[0]
        summary.append(f'- Over {budget:.2f} ms: {longest["frames"]} consecutive frames, '
                       f'{longest["start"]} to {longest["end_exclusive"]}.')
    else:
        summary.append(f"- No frames exceeded {budget:.2f} ms.")
summary += ["", "## Twenty slowest current Oborodzuki frames, pass 1", "",
            "| Frame | Timestamp | Render ms | Active events |", "|---:|---|---:|---:|"]
for frame in candidate["slowest_20_frames"]:
    summary.append(f'| {frame["frame"]} | {frame["timestamp"]} | {frame["render_ms"]:.3f} | {frame["active_events"]} |')
summary += ["", "## Correctness", ""]
for r in regression:
    summary.append(f'- Pass {r["pass"]}: {len(r["mismatched_frames"])} / 5,400 baseline–candidate RGB hash mismatches.')
summary += ["", "| Renderer | Selected capture pixel differences vs baseline |", "|---|---:|"]
for name in LABELS:
    summary.append(f"| {LABELS[name]} | {len(selected[name])} / {len(plan['capture_frames'])} |")
if diagnostics:
    summary += ["", "Renderer diagnostics flagged for review are listed in analysis.json. Raw logs accompany the frame CSVs."]
summary += ["", "Charts show pass 1. Raw frame times, hashes, renderer logs, selected images and detailed statistics are downloadable."]
text = "\n".join(summary) + "\n"
(DEST / "summary.md").write_text(text, encoding="utf-8")
(DEST / "analysis.json").write_text(json.dumps({
    "input": plan, "environment": environment,
    "passes": [{"engine": name, "pass": trial, **m} for (name, trial), m in all_metrics.items()],
    "baseline_regression": regression, "capture_differences_vs_baseline": selected,
    "renderer_diagnostics": diagnostics,
}, indent=2), encoding="utf-8")
if "GITHUB_STEP_SUMMARY" in os.environ:
    Path(os.environ["GITHUB_STEP_SUMMARY"]).write_text(text, encoding="utf-8")
print(text)
if selected["candidate"] or any(row["mismatched_frames"] for row in regression):
    raise SystemExit("KFX candidate pixels differ from the pinned Oborodzuki baseline")
