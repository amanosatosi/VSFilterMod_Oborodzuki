"""Run deterministic public fixtures with each renderer in a fresh process."""
import hashlib
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import sys
import time


def run_engine(engine, fixture, workload, span=()):
    name, mode, dll = engine
    start = time.perf_counter()
    process = subprocess.run(
        [str(harness), mode, dll, str(fixture.resolve()), workload, *map(str, span)],
        text=True, capture_output=True
    )
    if process.returncode:
        raise RuntimeError(f"{name}: {process.returncode}\n{process.stdout}\n{process.stderr}")
    row = json.loads(process.stdout)
    row["process_ms"] = (time.perf_counter() - start) * 1000
    return row


profiling = sys.argv[1:] == ["--profile"]
suite = os.environ.get("BENCH_SUITE", "full")
iteration = suite == "iteration"
if iteration:
    os.environ["OBORODZUKI_BENCH_ITERATION"] = "1"
trial_count = 3 if iteration else 5
real_count, real_warmup = (60, 6) if iteration else (240, 12)
synthetic_count, synthetic_warmup = (120, 10) if iteration else (600, 30)
output = Path("profile-results" if profiling else sys.argv[1])
output.mkdir(exist_ok=True)
harness = Path("benchmark-harness/render-bench.exe").resolve(strict=True)
source = Path(__file__).with_name("shared.ass")
primary = Path(__file__).with_name("op.fx.4testing.ass")
primary_bytes = primary.read_bytes()
primary_sha256 = hashlib.sha256(primary_bytes).hexdigest()
if not profiling and primary_sha256 != os.environ["BENCH_INPUT_SHA256"]:
    raise RuntimeError("Primary benchmark SHA-256 differs from Prepare job")


def as_milliseconds(timestamp):
    hours, minutes, seconds, centiseconds = map(int, re.split(r"[:.]", timestamp))
    return ((hours * 60 + minutes) * 60 + seconds) * 1000 + centiseconds * 10


timestamp = r"([0-9]+:[0-9]{2}:[0-9]{2}\.[0-9]{2})"
events = re.findall(r"^Dialogue:\s*[^,\r\n]*,\s*" + timestamp + r",\s*" + timestamp,
                    primary_bytes.decode("utf-8-sig"), re.MULTILINE)
if not events:
    raise RuntimeError("No dialogue timestamps found in op.fx.4testing.ass")
real_span = (min(as_milliseconds(start) for start, _ in events),
             max(as_milliseconds(stop) for _, stop in events))

shared = source.read_text(encoding="utf-8")
animated = output / "animated.ass"
animated.write_text(shared + (
    r"Dialogue: 1,0:00:01.00,0:00:05.00,Default,,0,0,0,,"
    r"{\move(450,360,1440,680)\clip(200,200,1700,920)\t(\frz30\blur2)\fad(100,180)}"
    r"{\kf90}Moving {\kf140}lyrics {\kf170}and effects"
    "\n"
), encoding="utf-8")
heavy = output / "heavy.ass"
heavy.write_text(shared + "".join(
    f"Dialogue: {i + 1},0:00:01.00,0:00:05.00,Default,,0,0,0,,"
    rf"{{\move({180 + i * 65},{120 + i * 31},{340 + i * 65},{260 + i * 31})"
    rf"\bord4\blur3\alpha&H20&\t(\frz25)}}{{\kf100}}Heavy {{\kf300}}karaoke {i + 1}"
    "\n" for i in range(24)
), encoding="utf-8")
fixtures = {"real": primary, "idle": source, "static": source, "animation": animated,
            "seek": animated, "heavy": heavy}

if profiling:
    stages = ["Frame bookkeeping and effects", "Subtitle parsing and layout",
              "Font lookup and text measurement", "Glyph path construction", "Transform",
              "Scan conversion", "Outline widening", "Rasterization", "Blur", "Compositing"]
    engine = ("Oborodzuki profile", "csri", str(Path("bundle/renderer.dll").resolve(strict=True)))
    profiles = {}
    for workload, fixture in fixtures.items():
        sample = run_engine(engine, fixture, workload, real_span if workload == "real" else ())
        profile = sample.get("profile", [])
        if len(profile) != len(stages) or not profile[0]["calls"]:
            raise RuntimeError(f"Missing instrumented render stage data for {workload}")
        profiles[workload] = dict(zip(stages, profile))
    (output / "profile.json").write_text(json.dumps({
        "suite": suite, "primary_input": primary.name, "primary_sha256": primary_sha256,
        "timestamp_range_ms": real_span, "stages": profiles
    }, indent=2), encoding="utf-8")
    lines = ["### Exclusive rendering wall time", "",
             f"Instrumentation adds overhead. The real workload uses {real_count} frames and "
             f"synthetic workloads use {synthetic_count}.",
             "", "| Workload | Stage | Exclusive ms | Calls |", "|---|---|---:|---:|"]
    for workload, stages_result in profiles.items():
        for name, score in sorted(stages_result.items(), key=lambda item: item[1]["ns"], reverse=True):
            lines.append(f'| {workload} | {name} | {score["ns"] / 1e6:.3f} | {score["calls"]} |')
    profile_report = "\n".join(lines) + "\n"
    (output / "profile.md").write_text(profile_report, encoding="utf-8")
    print(profile_report)
    if "GITHUB_STEP_SUMMARY" in os.environ:
        Path(os.environ["GITHUB_STEP_SUMMARY"]).write_text(profile_report, encoding="utf-8")
    sys.exit()

_, original, baseline, candidate, mangetsu = sys.argv[1:]
engines = [
    ("Original VSFilterMod r5.2.7", "csri", original),
    ("Oborodzuki baseline", "csri", baseline),
    ("Mangetsu legacy", "legacy", mangetsu),
    ("Mangetsu RGBA", "rgba", mangetsu),
    ("Oborodzuki candidate", "csri", candidate),
]
samples = []

for workload, fixture in fixtures.items():
    digest = hashlib.sha256(fixture.read_bytes()).hexdigest()
    for trial in range(trial_count):
        order = engines if trial % 2 == 0 else engines[::-1]
        if trial == trial_count - 1:
            order = engines[2:] + engines[:2]
        for position, engine in enumerate(order, 1):
            name = engine[0]
            print(f"{workload}, trial {trial + 1}, position {position}: {name}", flush=True)
            row = run_engine(engine, fixture, workload, real_span if workload == "real" else ())
            samples.append(dict(row, suite=suite, engine=name, workload=workload, trial=trial + 1,
                                order=position, fixture_sha256=digest))
            (output / "trials.json").write_text(json.dumps(samples, indent=2), encoding="utf-8")

summary = [
    f"Suite: {suite}.",
    f"Primary real fixture: {primary.name}, SHA-256 {primary_sha256}.",
    f"All {len(events)} dialogue events were loaded unchanged. "
    f"Sampled {real_count} presentation times across {real_span[0]}–{real_span[1]} ms, "
    f"with {real_warmup} warmup frames.",
    f"Each result uses {trial_count} fresh processes at 1920×1080 BGRX/BGRA on black.",
    f"Supplementary synthetic workloads use {synthetic_warmup} warmup frames and {synthetic_count} measured frames.",
    "Wall timing includes compositing. Render FPS excludes frame clearing and process startup.",
    "The Mangetsu legacy path uses the harness's scalar BGR compositor.",
    "The real fixture contains VSFilterMod-style gradient overrides (1vc/3vc). "
    "Mangetsu timings on that fixture are reported separately from equivalent-workload speedup claims.",
    "The first render follows setup in a fresh process. The OS font and file caches may still be warm.",
    "Range is the minimum to maximum of the per-trial median frame times.",
    "",
    "| Workload | Engine | Mean ms | Median ms | Range ms | p95 ms | FPS | First frame ms | CPU ms | Peak MiB |",
    "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|",
]
mismatches = []
comparison = []
for workload in fixtures:
    baseline_hashes = [s["hashes"] for s in samples
                       if s["workload"] == workload and s["engine"] == engines[1][0]]
    median_by_engine = {}
    for name, _, _ in engines:
        rows = [s for s in samples if s["engine"] == name and s["workload"] == workload]
        def median(key):
            return statistics.median(r[key] for r in rows)
        medians = [r["median_ms"] for r in rows]
        median_by_engine[name] = median("median_ms")
        summary.append(
            f'| {workload} | {name} | {median("avg_ms"):.4f} | {median("median_ms"):.4f} | '
            f'{min(medians):.4f}–{max(medians):.4f} | {median("p95_ms"):.4f} | '
            f'{median("fps"):.1f} | {median("first_frame_ms"):.4f} | '
            f'{median("cpu_ms"):.1f} | {median("peak_working_set_bytes") / 2**20:.1f} |'
        )
        if name == engines[-1][0] and any(
            r["hashes"] != hashes for r, hashes in zip(rows, baseline_hashes)
        ):
            mismatches.append(workload)
    candidate_ms = median_by_engine[engines[-1][0]]
    for name, _, _ in engines[:-1]:
        if workload == "real" and name.startswith("Mangetsu"):
            continue
        reference_ms = median_by_engine[name]
        if candidate_ms > 0 and reference_ms > 0:
            comparison.append(f"| {workload} | {name} | {reference_ms / candidate_ms:.2f}× | "
                              f"{(reference_ms - candidate_ms) / reference_ms:+.1%} |")
summary += ["", "Candidate latency relative to each reference. Positive savings indicate lower latency.",
            "", "| Workload | Reference | Speedup | Latency savings |", "|---|---|---:|---:|", *comparison]
summary += ["", f'Candidate versus pinned Oborodzuki RGB frame hashes: '
                 f'{"mismatch in " + ", ".join(mismatches) if mismatches else "all sampled frames match"}.']

# Keep renderer-specific tags out of the shared performance comparison.
compatibility = output / "compatibility.ass"
compatibility.write_text(shared + (
    r"Dialogue: 1,0:00:01.00,0:00:05.00,Default,,0,0,0,,"
    r"{\an8\pos(960,120)\fnMissing Font For Fallback\3c&H336699&}"
    "日本語 မြန်မာ العربية\n"
    r"Dialogue: 2,0:00:01.00,0:00:05.00,Default,,0,0,0,,"
    r"{\an5\pos(960,540)\frx25\fry-20\fax0.2\iclip(950,400,980,650)\blur1.5}"
    r"{\1vc(&H0033FF&,&H995500&,&H3300FF&,&HFFFF00&)}Perspective and gradient" "\n"
    r"Dialogue: 3,0:00:01.00,0:00:05.00,Default,,0,0,0,,"
    r"{\an7\pos(600,500)\c&H304050&\alpha&H44&\clip(m 400 300 l 1500 300 1500 800 400 800)}"
    r"{\p1}m 0 0 l 600 0 600 120 0 120{\p0}" "\n"
    r"Dialogue: 4,0:00:01.00,0:00:05.00,Default,,0,0,0,,"
    r"{\an7\pos(0,280)\xbord0\ybord3\xblur2\yblur0}Horizontal blur / vertical outline" "\n"
    r"Dialogue: 5,0:00:01.00,0:00:05.00,Default,,0,0,0,,"
    r"{\an3\pos(1920,900)\xbord4\ybord0\xblur0\yblur2}Vertical blur / horizontal outline" "\n"
), encoding="utf-8-sig")
ssa = output / "legacy.ssa"
ssa.write_text(
    "[Script Info]\nScriptType: v4.00\nPlayResX: 1920\nPlayResY: 1080\n\n"
    "[V4 Styles]\n"
    "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, TertiaryColour, BackColour,"
    " Bold, Italic, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, AlphaLevel, Encoding\n"
    "Style: Default,Arial,48,16777215,65535,0,0,-1,0,1,3,2,2,30,30,40,0,1\n\n"
    "[Events]\nFormat: Marked, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
    "Dialogue: Marked=0,0:00:01.00,0:00:05.00,Default,,0,0,0,,SSA with an outline and shadow\n",
    encoding="ascii"
)
regressions = {}
summary += ["", "### VSFilterMod visual regression", "",
            "| Fixture | Original vs candidate | Oborodzuki baseline vs candidate |",
            "|---|---|---|"]
for fixture in (source, animated, heavy, compatibility, ssa):
    engine_hashes = {engine[0]: run_engine(engine, fixture, "regression")["hashes"]
                     for engine in (engines[0], engines[1], engines[-1])}
    regressions[fixture.name] = engine_hashes
    actual = engine_hashes[engines[-1][0]]
    original_match = actual == engine_hashes[engines[0][0]]
    baseline_match = actual == engine_hashes[engines[1][0]]
    summary.append(f"| {fixture.name} | {'match' if original_match else 'different'} | "
                   f"{'match' if baseline_match else 'different'} |")
    if not baseline_match:
        mismatches.append(fixture.name)
(output / "regression.json").write_text(json.dumps(regressions, indent=2), encoding="utf-8")
report = "\n".join(summary) + "\n"
(output / "summary.md").write_text(report, encoding="utf-8")
print(report)
if "GITHUB_STEP_SUMMARY" in os.environ:
    with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as target:
        target.write(report)
if mismatches:
    raise SystemExit("Oborodzuki pixel regression detected")
