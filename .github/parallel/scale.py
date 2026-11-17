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
    from importlib.machinery import SourceFileLoader
    fixture_module = SourceFileLoader("kfx_fixture", ".github/kfx/fixture.py").load_module()
    plan = fixture_module.inspect_fixture()
    (ROOT / "input.json").write_text(json.dumps(plan, indent=2))
    full = {}
    for workers in WORKERS:
        os.environ["OBORODZUKI_WORKERS"] = str(workers)
        dest = ROOT / f"workers-{workers}"
        subprocess.run([PLAYBACK, "csri", DLL, str(Path(plan["fixture"]).resolve()), str(dest.resolve()),
                        str(plan["dialogue_events"]), str(plan["size_bytes"]),
                        *map(str, plan["capture_frames"])], check=True, timeout=900)
        with (dest / "frames.csv").open(newline="") as file:
            rows = list(csv.DictReader(file))
        if len(rows) != 5400 or [int(r["frame"]) for r in rows] != list(range(5400)):
            raise RuntimeError("Incomplete KFX coverage")
        if workers > 1 and [r["rgb_hash"] for r in rows] != full[1]["hashes"]:
            raise RuntimeError(f"KFX pixel mismatch at workers={workers}")
        values = sorted(float(r["render_ms"]) for r in rows)
        metadata = json.loads((dest / "metrics.json").read_text())
        longest, current = 0, 0
        for row in rows:
            current = current + 1 if float(row["render_ms"]) > 16.67 else 0
            longest = max(longest, current)
        full[workers] = dict(metadata, hashes=[r["rgb_hash"] for r in rows], average_ms=statistics.mean(values),
                             median_ms=statistics.median(values), p95_ms=values[5129], p99_ms=values[5345],
                             maximum_ms=max(values), over_16=sum(v > 16.67 for v in values),
                             over_33=sum(v > 33.33 for v in values), longest_slow_run=longest,
                             total_render_ms=sum(values), cpu_percent_of_one_core=100 * metadata["render_cpu_ms"] / sum(values),
                             render_fps=5400 * 1000 / sum(values))
    previous_dll = Path("previous-renderer/renderer.dll")
    if previous_dll.exists():
        os.environ["OBORODZUKI_WORKERS"] = "1"
        dest = ROOT / "previous-single"
        subprocess.run([PLAYBACK, "csri", str(previous_dll.resolve()), str(Path(plan["fixture"]).resolve()),
                        str(dest.resolve()), str(plan["dialogue_events"]), str(plan["size_bytes"])], check=True, timeout=900)
        with (dest / "frames.csv").open(newline="") as file:
            rows = list(csv.DictReader(file))
        if len(rows) != 5400 or [row["rgb_hash"] for row in rows] != full[1]["hashes"]:
            raise RuntimeError("Current one-worker output differs from the pre-threading renderer")
        previous_mean = statistics.mean(float(row["render_ms"]) for row in rows)
        lines += ["", f"Previous optimized one-worker mean on this runner: {previous_mean:.3f} ms. "
                  f"Current one-worker mean: {full[1]['average_ms']:.3f} ms. All 5,400 hashes match."]
    (ROOT / "full.json").write_text(json.dumps(full, indent=2))
    lines += ["", "## Full KFX, 5,400 consecutive frames per setting", "",
              "| Workers | Avg ms | Median ms | p95 | p99 | Max | FPS | Speedup | Efficiency | >16.67 | >33.33 | Longest slow run | Peak MiB |",
              "|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for workers, m in full.items():
        speed = full[1]["average_ms"] / m["average_ms"]
        lines.append(f'| {workers} | {m["average_ms"]:.3f} | {m["median_ms"]:.3f} | {m["p95_ms"]:.3f} | '
                     f'{m["p99_ms"]:.3f} | {m["maximum_ms"]:.3f} | {m["render_fps"]:.1f} | {speed:.2f}× | '
                     f'{speed / workers:.1%} | {m["over_16"]} | {m["over_33"]} | {m["longest_slow_run"]} | '
                     f'{m["peak_working_set_bytes"] / 2**20:.1f} |')
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    figure, ax = plt.subplots(figsize=(16, 6), layout="constrained")
    for workers in WORKERS:
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
