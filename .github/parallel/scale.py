"""Compare runtime worker settings on one runner using the existing hosts."""
import csv
import json
import math
import os
from pathlib import Path
import statistics
import subprocess
import sys

WORKERS = [1, 2, 4, 6, 8]
ROOT = Path("parallel-results")
ROOT.mkdir(exist_ok=True)
DLL = str(Path("renderer/renderer.dll").resolve(strict=True))
HOST = str(Path("hosts/render-bench.exe").resolve(strict=True))
PLAYBACK = str(Path("hosts/kfx-playback.exe").resolve(strict=True))
fixture = Path(".github/ci/shared.ass").read_text(encoding="utf-8")
header = fixture[:fixture.index("Dialogue:")]
cpu = os.cpu_count() or 1
scenarios = {"idle": (1, ""), "static": (1, ""), "animated": (1, ""), "banner": (8, ""), "scroll": (8, ""),
             "blend": (16, r"\blendadd\alpha&H60&\clip(100,100,1800,1000)"),
             "distortion": (16, r"\distort(1,0.1,0.9,1,0.1,0.9)\frx20"),
             **{f"overlap-{n}": (n, "") for n in [4, 8, 16, 32, 64]},
             "border": (16, r"\bord8"), "blur": (16, r"\bord6\blur6"),
             "clip": (16, r"\clip(100,100,1800,1000)\frx15\fry20"),
             "gradient": (16, r"\1vc(&H00FF00&,&H0000FF&,&HFF0000&,&HFFFFFF&)\alpha&H40&"),
             "mixed": (16, r"\1vc(&H00FF00&,&H0000FF&,&HFF0000&,&HFFFFFF&)\alpha&H40&"
                       r"\iclip(m 800 300 l 1200 300 1200 750 800 750)\fry20"),
             "random": (16, r"\rnd0.2\rnds42\jitter(1,1,1,1,50,7)"),
             "lua": (16, r"\eventstyle")}
lua = ROOT / "callbacks.lua"
lua.write_text("counter = 0\nfunction eventstyle(line)\n counter = counter + 1\n"
               " return {frz = counter % 7 * 2, fscx = 100 + line.id % 3 * 3}\nend\n", encoding="ascii")
for name, (count, effects) in scenarios.items():
    text = header
    if name == "lua":
        text = text.replace("[Script Info]", "[Script Info]\nLua: " + lua.resolve().as_posix())
    for index in range(count):
        motion = "" if name in ("idle", "static") else r"\t(0,4000,\frz30\fscx120\fscy85)"
        event_effects = effects
        if name == "blend":
            mode = ["over", "add", "sub", "mult", "scr", "diff", "rsub", "isub"][index % 8]
            event_effects = effects.replace(r"\blendadd", r"\blend" + mode)
        effect = "Banner;10;0;40" if name == "banner" else "Scroll up;80;900;20;40" if name == "scroll" else ""
        text += (f"Dialogue: {index},0:00:01.00,0:00:05.00,Default,,0,0,0,{effect},"
                 rf"{{\an5\pos({350 + index % 8 * 160},{180 + index % 6 * 120}){motion}{event_effects}}}"
                 f"Worker overlap event {index}"
                 + (" 日本語 မြန်မာ" if name == "mixed" else "") + "\n")
    (ROOT / f"{name}.ass").write_text(text, encoding="utf-8")
if sys.argv[1:] == ["--fixtures-only"]:
    raise SystemExit()
print("Checking concurrent instances and repeated teardown", flush=True)
subprocess.run(["hosts/concurrent.exe", DLL, str((ROOT / "overlap-32.ass").resolve())], check=True, timeout=180)

baseline = str(Path("threading-baseline/renderer/renderer.dll").resolve(strict=True))
dlls = {"baseline": baseline, "candidate": DLL}
comparable = [workers for workers in WORKERS if workers <= cpu]
quick = os.environ.get("PARALLEL_QUICK") == "true" and os.environ.get("PARALLEL_FULL") != "true"
fixture_names = ("idle", "static", "animated", "overlap-16", "blend", "mixed") if quick else tuple(scenarios)
settings = [(revision, workers) for workers in (comparable if quick else WORKERS)
            for revision in dlls if revision == "candidate" or workers in comparable]


def trial_order(settings, trial):
    return settings if trial % 2 == 0 else settings[::-1]


samples = []
os.environ["OBORODZUKI_BENCH_ITERATION"] = "1"
for name in (*fixture_names, "kfx-sample"):
    if name == "kfx-sample":
        arguments = [str(Path(".github/ci/op.fx.4testing.ass").resolve()), "real", "55000", "65000"]
    else:
        arguments = [str((ROOT / f"{name}.ass").resolve()), name if name in ("idle", "static") else "animation"]
    reference = None
    for trial in range(3):
        for revision, workers in trial_order(settings, trial):
            label = f"{name}, {revision}, trial {trial + 1}, workers={workers}"
            print(f"Starting {label}", flush=True)
            os.environ["OBORODZUKI_WORKERS"] = str(workers)
            result = subprocess.run([HOST, "csri", dlls[revision], *arguments],
                                    text=True, capture_output=True, timeout=180)
            if result.returncode:
                raise RuntimeError(f"{label}: {result.stderr}")
            sample = dict(json.loads(result.stdout), workload=name, trial=trial + 1, workers=workers, revision=revision)
            samples.append(sample)
            (ROOT / "iteration.json").write_text(json.dumps(samples, indent=2), encoding="utf-8")
            if reference is None:
                reference = sample["hashes"]
            if sample["hashes"] != reference:
                raise RuntimeError(f"Pixel mismatch for {label}: {sample['hashes']} vs {reference}")
lines = [f"# Worker scaling on {cpu} logical processors", "",
         "Three alternating-order trials per setting. All synthetic RGB hashes match the previous renderer.",
         "Worker counts above the processor count are used for correctness checks.",
         "", "| Workload | Revision | Workers | Median ms | p95 ms | Speedup | Efficiency |",
         "|---|---|---:|---:|---:|---:|---:|"]
for name in (*fixture_names, "kfx-sample"):
    for revision in dlls:
        workload_samples = [s for s in samples if s["workload"] == name and s["revision"] == revision]
        one = statistics.median(s["median_ms"] for s in workload_samples if s["workers"] == 1)
        for workers in comparable:
            selected = [s for s in workload_samples if s["workers"] == workers]
            median = statistics.median(s["median_ms"] for s in selected)
            p95 = statistics.median(s["p95_ms"] for s in selected)
            speedup = one / median
            lines.append(f"| {name} | {revision} | {workers} | {median:.4f} | {p95:.4f} | {speedup:.2f}× | {speedup / workers:.1%} |")

if os.environ.get("PARALLEL_FULL") == "true":
    import runpy

    plan = runpy.run_path(".github/kfx/fixture.py")["inspect_fixture"]()
    (ROOT / "input.json").write_text(json.dumps(plan, indent=2), encoding="utf-8")
    full = []
    reference = None

    def play_full(revision, dll, workers, trial, captures=()):
        os.environ["OBORODZUKI_WORKERS"] = str(workers)
        prefix = "" if revision == "candidate" else revision + "-"
        suffix = "" if trial == 1 else f"-trial-{trial}"
        dest = ROOT / f"{prefix}workers-{workers}{suffix}"
        label = f"{revision}, workers={workers}, trial={trial}"
        print(f"Starting complete KFX playback for {label}", flush=True)
        subprocess.run([PLAYBACK, "csri", dll, str(Path(plan["fixture"]).resolve()), str(dest.resolve()),
                        str(plan["dialogue_events"]), str(plan["size_bytes"]), *map(str, captures)],
                       check=True, timeout=900)
        with (dest / "frames.csv").open(newline="") as file:
            rows = list(csv.DictReader(file))
        if [int(row["frame"]) for row in rows] != list(range(plan["frames"])):
            raise RuntimeError(f"Incomplete KFX coverage for {label}")
        chronological = [float(row["render_ms"]) for row in rows]
        values = sorted(chronological)

        def slow_frames(threshold):
            count, longest, consecutive = 0, 0, 0
            for value in chronological:
                slow = value > threshold
                count += slow
                consecutive = consecutive + 1 if slow else 0
                longest = max(longest, consecutive)
            return count, longest

        over_16, longest_16 = slow_frames(16.67)
        over_33, longest_33 = slow_frames(33.33)
        total = sum(values)
        metadata = json.loads((dest / "metrics.json").read_text(encoding="utf-8"))
        hashes = [row["rgb_hash"] for row in rows]
        mismatches = 0 if reference is None else sum(actual != expected for actual, expected in zip(hashes, reference))
        full.append(dict(metadata, revision=revision, workers=workers, trial=trial,
                         rgb_mismatches=mismatches, total_render_ms=total, average_ms=statistics.mean(values),
                         median_ms=statistics.median(values), p95_ms=values[math.ceil(len(values) * .95) - 1],
                         p99_ms=values[math.ceil(len(values) * .99) - 1], maximum_ms=max(values),
                         over_16=over_16, over_33=over_33, longest_16=longest_16, longest_33=longest_33,
                         cpu_percent_of_one_core=100 * metadata["render_cpu_ms"] / total,
                         render_fps=len(values) * 1000 / total))
        (ROOT / "full.json").write_text(json.dumps(full, indent=2), encoding="utf-8")
        if mismatches:
            first = next(i for i, (actual, expected) in enumerate(zip(hashes, reference)) if actual != expected)
            raise RuntimeError(f"KFX pixel mismatch for {label}: {mismatches} frames, first at frame {first}")
        return hashes

    full_settings = [(revision, workers) for workers in comparable for revision in dlls]
    full_settings += [("candidate", workers) for workers in WORKERS if workers not in comparable]
    for trial in range(2):
        for revision, workers in trial_order(full_settings, trial):
            if trial and workers not in comparable:
                continue
            hashes = play_full(revision, dlls[revision], workers, trial + 1,
                               plan["capture_frames"] if trial == 0 else ())
            if reference is None:
                reference = hashes

    previous_dll = Path("previous-renderer/renderer.dll")
    if previous_dll.exists():
        play_full("pre-threading", str(previous_dll.resolve()), 1, 1)
        lines += ["", f"Pre-threading optimized renderer averaged {full[-1]['average_ms']:.3f} ms. "
                  "All 5,400 hashes match."]

    one = {revision: statistics.mean(m["average_ms"] for m in full if m["revision"] == revision and m["workers"] == 1)
           for revision in dlls}
    lines += ["", "## Full KFX, 5,400 consecutive frames per pass", "",
              "Speedup and efficiency use each revision's two-trial one-worker mean. "
              "Counts above the runner's logical processor count verify pixels only.",
              "", "| Renderer | Workers | Trial | Mean ms | Median ms | p95 ms | p99 ms | Max ms | Render s | FPS | "
              ">16.67 | >33.33 | Longest >16.67 / >33.33 | CPU s | Peak MiB | Speedup | Efficiency | RGB mismatches |",
              "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for m in sorted(full, key=lambda row: (row["revision"], row["workers"], row["trial"])):
        revision, workers = m["revision"], m["workers"]
        speed = one[revision] / m["average_ms"] if revision in one and workers in comparable else None
        scaling = f"{speed:.2f}× | {speed / workers:.1%}" if speed else "— | —"
        lines.append(f'| {revision} | {workers} | {m["trial"]} | {m["average_ms"]:.3f} | {m["median_ms"]:.3f} | '
                     f'{m["p95_ms"]:.3f} | {m["p99_ms"]:.3f} | {m["maximum_ms"]:.3f} | '
                     f'{m["total_render_ms"] / 1000:.2f} | {m["render_fps"]:.1f} | {m["over_16"]} | {m["over_33"]} | '
                     f'{m["longest_16"]} / {m["longest_33"]} | {m["render_cpu_ms"] / 1000:.2f} | '
                     f'{m["peak_working_set_bytes"] / 2**20:.1f} | {scaling} | {m["rgb_mismatches"]} |')
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    figure, ax = plt.subplots(figsize=(16, 6), layout="constrained")
    for workers in comparable:
        with (ROOT / f"workers-{workers}/frames.csv").open(newline="") as f:
            rows = list(csv.DictReader(f))
        ax.plot([int(r["frame"]) / 60 for r in rows], [float(r["render_ms"]) for r in rows],
                label=f"{workers} workers", linewidth=0.6)
    ax.axhline(16.67, color="black", linestyle="--")
    ax.set(xlabel="Presentation seconds", ylabel="Frame latency ms", title="KFX worker scaling, full 90-second timeline")
    ax.legend()
    figure.savefig(ROOT / "full-scaling.png", dpi=150)

report = "\n".join(lines) + "\n"
(ROOT / "summary.md").write_text(report, encoding="utf-8")
print(report)
with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as output:
    output.write(report)
