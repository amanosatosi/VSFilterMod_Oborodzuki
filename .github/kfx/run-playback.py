"""Run each full KFX playback sequentially in a fresh process."""
import csv
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

from fixture import FIXTURE, EXPECTED_SHA256, FPS, FRAMES


profile = sys.argv[1:] == ["--profile"]
root = Path("kfx-profile" if profile else "kfx-raw")
root.mkdir(exist_ok=True)
plan = json.loads(Path("kfx-plan.json").read_text(encoding="utf-8"))
fixture = FIXTURE.resolve(strict=True)
digest = hashlib.sha256(fixture.read_bytes()).hexdigest()
if digest != EXPECTED_SHA256 or digest != plan["sha256"] or fixture.stat().st_size != plan["size_bytes"]:
    raise RuntimeError("Playback fixture differs from Prepare job")

environment = json.loads((root / "environment.json").read_text(encoding="utf-8"))
harness = environment["harness"]["path"]
renderers = environment["renderers"]
engines = [("profile", "profile", "csri")] if profile else [
    ("original", "original", "csri"), ("baseline", "baseline", "csri"),
    ("candidate", "candidate", "csri"), ("mangetsu-legacy", "mangetsu", "legacy"),
    ("mangetsu-rgba", "mangetsu", "rgba"),
]
repetitions = 1 if profile else int(os.environ["KFX_REPETITIONS"])
if not 1 <= repetitions <= 5:
    raise ValueError("KFX repetitions must be between 1 and 5")
(root / "input.json").write_text(json.dumps(plan, indent=2), encoding="utf-8")
passes = []

for index in range(repetitions):
    order = engines if index % 2 == 0 else engines[::-1]
    for name, renderer, mode in order:
        dest = root / name / f"pass-{index + 1}"
        dest.mkdir(parents=True, exist_ok=True)
        captures = [] if profile or index else plan["capture_frames"]
        print(f"{name}, pass {index + 1}/{repetitions}", flush=True)
        with (dest / "renderer.log").open("wb") as diagnostic_log:
            process = subprocess.run([
                harness, mode, renderers[renderer]["path"], str(fixture), str(dest.resolve()),
                str(plan["dialogue_events"]), str(plan["size_bytes"]), *map(str, captures),
            ], stderr=diagnostic_log)
        if process.returncode:
            print((dest / "renderer.log").read_text(encoding="utf-8", errors="replace")[-4000:])
            raise RuntimeError(f"{name} exited with code {process.returncode}")

        with (dest / "frames.csv").open(newline="", encoding="utf-8") as source:
            frames = list(csv.DictReader(source))
        if len(frames) != FRAMES:
            raise RuntimeError(f"{name} rendered {len(frames)} frames instead of {FRAMES}")
        for i, frame in enumerate(frames):
            if (int(frame["frame"]) != i or int(frame["renderer_timestamp_ms"]) != i * 1000 // FPS
                    or int(frame["presentation_ns"]) != i * 1_000_000_000 // FPS):
                raise RuntimeError(f"{name} returned an incorrect playback timestamp at frame {i}")

        metrics = json.loads((dest / "metrics.json").read_text(encoding="utf-8"))
        if metrics["input_bytes"] != plan["size_bytes"] or metrics["frames"] != FRAMES:
            raise RuntimeError(f"{name} did not confirm full input and playback length")
        if mode != "csri" and metrics["parsed_events"] != plan["dialogue_events"]:
            raise RuntimeError(f"{name} loaded an unexpected dialogue event count")
        if profile and not metrics.get("profile"):
            raise RuntimeError("Instrumented renderer returned no profiling data")
        passes.append({"engine": name, "pass": index + 1, "directory": dest.relative_to(root).as_posix()})
        (root / "passes.json").write_text(json.dumps(passes, indent=2), encoding="utf-8")

if profile:
    names = ["Frame bookkeeping", "Subtitle parsing and layout", "Font measurement", "Glyph path",
             "Transform", "Scan conversion", "Outline widening", "Rasterization", "Blur", "Compositing"]
    stage_rows = sorted(zip(names, metrics["profile"], strict=True), key=lambda item: item[1]["ns"], reverse=True)
    report = ["## Instrumented 5,400-frame KFX playback", "", "| Stage | Exclusive ms | Calls |", "|---|---:|---:|"]
    report.extend(f"| {name} | {result['ns'] / 1e6:.3f} | {result['calls']} |" for name, result in stage_rows)
    (root / "profile.md").write_text("\n".join(report) + "\n", encoding="utf-8")
    with Path(os.environ["GITHUB_STEP_SUMMARY"]).open("a", encoding="utf-8") as target:
        target.write("\n".join(report) + "\n")
