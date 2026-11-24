"""Run the CI-built renderer hosts and record timings from this PC."""
import csv
import ctypes
from ctypes import wintypes
from datetime import datetime
import hashlib
import html
import itertools
import json
import math
import os
from pathlib import Path
import platform
import shutil
import statistics
import subprocess
import sys
import threading
import time

from bench_fixtures import sampled_fixtures
from fixture import inspect_fixture
from scale_fixtures import scale_fixtures

ROOT = Path(__file__).resolve().parents[2]
HOST = ROOT / "binaries" / "hosts"
BRANCH = "astra/extra-optimization-JustInCase"
REPO = "amanosatosi/VSFilterMod_Oborodzuki"
BASELINE = "79db5609394b6f27465cd7f24ccf804f803e075e"
ORIGINAL = "64ea41f6cf58c2e60e4ede75fe0a56de76ffef54"
MANGETSU = "39499c79d933c3fdd30c669964d2758a7721d679"
ENGINES = {
    "Original VSFilterMod": ("original", "csri"),
    "Oborodzuki baseline": ("baseline", "csri"),
    "Mangetsu legacy": ("mangetsu", "legacy"),
    "Mangetsu RGBA": ("mangetsu", "rgba"),
    "Oborodzuki candidate": ("candidate", "csri"),
}
BASE = "Oborodzuki baseline"
CANDIDATE = "Oborodzuki candidate"
TRIALS = 2
MAX_WORKERS = {BASE: 8, CANDIDATE: 12}
METRICS = ("avg_ms", "median_ms", "p95_ms", "p99_ms", "fps", "peak_working_set_bytes")


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def write_json(path, data):
    temporary = path.with_suffix(".json.tmp")
    temporary.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def renderer_path(component):
    return ROOT / "binaries" / component / "renderer.dll"


def verify_package():
    manifest = read_json(ROOT / "manifest.json")
    if (manifest["schema"], manifest["repository"], manifest["branch"]) != (1, REPO, BRANCH):
        raise RuntimeError("Unexpected package identity")
    expected = dict(candidate=manifest["revision"], baseline=BASELINE, original=ORIGINAL, mangetsu=MANGETSU)
    if {row["kind"]: row["ref"] for row in manifest["renderers"]} != expected:
        raise RuntimeError("Renderer revisions do not match the package manifest")
    required = ["binaries/hosts/render-bench.exe", "binaries/hosts/kfx-playback.exe",
                "binaries/hosts/build.json", "fixtures/shared.ass", "fixtures/op.fx.4testing.ass"]
    for kind in expected:
        required.extend(f"binaries/{kind}/{name}" for name in ("renderer.dll", "build.json"))
    if not set(required).issubset(manifest["files"]):
        raise RuntimeError("Required binaries or fixtures are missing from the manifest")
    for name, digest in manifest["files"].items():
        path = (ROOT / name).resolve()
        if not path.is_relative_to(ROOT) or sha256(path) != digest:
            raise RuntimeError(f"Package SHA-256 mismatch for {name}")
    for kind, revision in expected.items():
        build = read_json(renderer_path(kind).with_name("build.json"))
        if (build["component"], build["revision"], build["platform"], build["instrumented"]) != (
                kind, revision, "x64", False):
            raise RuntimeError(f"Unexpected {kind} build metadata")
    build = read_json(HOST / "build.json")
    if build["revision"] != expected["candidate"] or build["headers_revision"] != MANGETSU:
        raise RuntimeError("Benchmark host revision or Mangetsu headers do not match")
    return manifest


def smoke_package():
    manifest = verify_package()
    for name in manifest["files"]:
        if name.startswith("binaries/") and name.endswith(".dll"):
            dll = ctypes.WinDLL(str(ROOT / name))
            export = "ass_library_init" if name.startswith("binaries/mangetsu/") else "csri_renderer_default"
            getattr(dll, export)
    for name, expected_code in (("render-bench.exe", 2), ("kfx-playback.exe", 2)):
        child = subprocess.run([HOST / name], capture_output=True, timeout=20)
        if child.returncode != expected_code:
            raise RuntimeError(f"{name} could not start: {child.returncode} {child.stderr!r}")
    print(f"Portable binaries and embedded runtime load successfully for {manifest['revision']}.")


def worker_counts(logical, physical):
    proposed = {1, 2, 4, 6, 8, 12, physical or 1, logical}
    return sorted(n for n in proposed if 1 <= n <= min(logical, MAX_WORKERS[CANDIDATE]))


def windows_hardware():
    class Memory(ctypes.Structure):
        _fields_ = [("size", wintypes.DWORD), ("load", wintypes.DWORD)] + [
            (name, ctypes.c_uint64) for name in
            ("total", "available", "page", "page_available", "virtual", "virtual_available", "extended")]
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    memory = Memory()
    memory.size = ctypes.sizeof(memory)
    memory_total = memory.total if kernel.GlobalMemoryStatusEx(ctypes.byref(memory)) else None
    size = wintypes.DWORD()
    # RelationProcessorCore = 0. Each variable-size record represents one physical core.
    kernel.GetLogicalProcessorInformationEx(0, None, ctypes.byref(size))
    physical = None
    if size.value:
        buffer = ctypes.create_string_buffer(size.value)
        if kernel.GetLogicalProcessorInformationEx(0, buffer, ctypes.byref(size)):
            offset, physical = 0, 0
            while offset < size.value:
                _, record_size = (wintypes.DWORD * 2).from_buffer(buffer, offset)
                if record_size < 8 or offset + record_size > size.value:
                    physical = None
                    break
                physical += 1
                offset += record_size
    kernel.GetActiveProcessorCount.restype = wintypes.DWORD
    return {"physical_cores": physical, "logical_processors": kernel.GetActiveProcessorCount(0xffff),
            "available_logical_processors": getattr(os, "process_cpu_count", os.cpu_count)() or 1,
            "ram_bytes": memory_total}


def registered_fonts():
    import winreg
    names = set()
    key = r"SOFTWARE\Microsoft\Windows NT\CurrentVersion\Fonts"
    for hive in (winreg.HKEY_LOCAL_MACHINE, winreg.HKEY_CURRENT_USER):
        try:
            with winreg.OpenKey(hive, key) as fonts:
                for i in range(winreg.QueryInfoKey(fonts)[1]):
                    name = winreg.EnumValue(fonts, i)[0]
                    names.add(name.rsplit(" (", 1)[0])
        except FileNotFoundError:
            continue
    return sorted(names, key=str.casefold)


def bar(value, total, width=16):
    filled = int(width * min(value, total) / total) if total else 0
    return "[" + "#" * filled + "-" * (width - filled) + "]"


def duration(seconds):
    minutes, seconds = divmod(max(0, int(seconds)), 60)
    hours, minutes = divmod(minutes, 60)
    return f"{hours:02}:{minutes:02}:{seconds:02}"


class Progress:
    def __init__(self, total):
        self.total = total
        self.start = time.monotonic()
        self.tty = False
        if sys.stdout.isatty():
            kernel = ctypes.WinDLL("kernel32")
            kernel.GetStdHandle.restype = wintypes.HANDLE
            stdout = kernel.GetStdHandle(-11)
            kernel.GetConsoleMode.argtypes = (wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD))
            kernel.SetConsoleMode.argtypes = (wintypes.HANDLE, wintypes.DWORD)
            mode = wintypes.DWORD()
            self.tty = bool(kernel.GetConsoleMode(stdout, ctypes.byref(mode)) and
                            kernel.SetConsoleMode(stdout, mode.value | 4))
        self.previous_lines = False

    def update(self, completed=None, total=None):
        elapsed = time.monotonic() - self.start
        label = f"{self.phase} | {self.workload} | {self.engine} | {self.workers}w"
        if not self.tty:
            if completed is None:
                print(f"{self.index}/{self.total} {label}", flush=True)
            return
        width = max(20, shutil.get_terminal_size().columns - 1)
        overall = f"Overall {bar(self.index - 1, self.total)} {self.index - 1}/{self.total} tests | elapsed {duration(elapsed)}"
        if total is None:
            current = "Current [----------------] Preparing renderer or checking pixels"
        elif completed == total:
            current = f"Current {bar(completed, total)} {completed}/{total} frames | Checking or saving"
        else:
            current = f"Current {bar(completed, total)} {completed}/{total} frames"
        print(("\x1b[2A" if self.previous_lines else "") + "\n".join(
            f"\r{line[:width].ljust(width)}" for line in (overall, label, current)), end="", flush=True)
        self.previous_lines = True

    def begin(self, index, phase, workload, engine, workers):
        if self.tty and not self.previous_lines:
            print("Starting local benchmark tests")
        self.index, self.phase, self.workload, self.engine, self.workers = index, phase, workload, engine, workers
        self.update()

    def finish(self, completed):
        if self.tty and self.previous_lines:
            print()
        print(f"Completed {completed}/{self.total} tests in {duration(time.monotonic() - self.start)}.", flush=True)


def run_child(command, env, directory, dest, log, progress):
    stdout = dest / "stdout.txt"
    with stdout.open("wb") as output:
        child = subprocess.Popen(command, cwd=directory, env=env, stdout=output, stderr=subprocess.PIPE,
                                 text=True, encoding="utf-8", errors="replace", bufsize=1)
        deadline = threading.Timer(1800, child.kill)
        deadline.daemon = True
        deadline.start()
        try:
            with child.stderr:
                for line in child.stderr:
                    if line.startswith("BENCH_PROGRESS "):
                        completed, total = map(int, line.split()[1:])
                        progress.update(completed, total)
                    else:
                        log.write(line)
            code = child.wait()
        finally:
            deadline.cancel()
            if child.poll() is None:
                child.kill()
                child.wait()
    if code:
        raise RuntimeError(f"Host exited with code {code}. See {dest} and benchmark.log.")
    return stdout


def percentile(sorted_values, fraction):
    return sorted_values[math.ceil(len(sorted_values) * fraction) - 1]


def playback_metrics(frames, raw, frame_limit):
    with frames.open(newline="", encoding="ascii") as stream:
        rows = list(csv.DictReader(stream))
    if len(rows) != frame_limit or any(int(row["frame"]) != index for index, row in enumerate(rows)):
        raise RuntimeError(f"Incomplete sustained playback timestamps in {frames}")
    metrics = read_json(raw)
    if metrics["frames"] != frame_limit or not metrics["visible_frames"]:
        raise RuntimeError("Sustained playback produced incomplete or invisible output")
    values = [float(row["render_ms"]) for row in rows]
    ordered = sorted(values)
    metrics.update(avg_ms=statistics.mean(values), median_ms=statistics.median(ordered),
                   p95_ms=percentile(ordered, .95), p99_ms=percentile(ordered, .99),
                   fps=1000 * len(values) / sum(values), frame_times_ms=values)
    return metrics, [row["rgb_hash"] for row in rows]


def run_step(step, sequence, dest, inputs, plan, log, progress):
    phase, workload, name, workers, trial, fixture, timeline = step
    component, mode = ENGINES[name]
    env = {key: value for key, value in os.environ.items() if not key.startswith("OBORODZUKI_BENCH_")}
    env.update(OBORODZUKI_WORKERS=str(workers), OBORODZUKI_BENCH_ITERATION="1", OBORODZUKI_BENCH_PORTABLE="1")
    dest.mkdir()
    dll = renderer_path(component)
    if timeline == "playback":
        command = [HOST / "kfx-playback.exe", mode, dll, fixture, dest, str(plan["dialogue_events"]),
                   str(plan["size_bytes"])]
        run_child(command, env, inputs, dest, log, progress)
        result, hashes = playback_metrics(dest / "frames.csv", dest / "metrics.json", plan["frames"])
    else:
        command = [HOST / "render-bench.exe", mode, dll, fixture, timeline]
        if timeline == "real":
            span = (55000, 65000) if workload == "kfx-sample" else (plan["first_event_ms"], plan["last_event_ms"])
            command.extend(map(str, span))
        stdout = run_child(command, env, inputs, dest, log, progress)
        result = read_json(stdout)
        hashes = result.get("hashes", [])
    result.update(phase=phase, workload=workload, renderer=name, workers=workers, trial=trial,
                  sequence=sequence, fixture=fixture.name, fixture_sha256=sha256(fixture),
                  timeline=timeline, raw_directory=dest.parent.name + "/" + dest.name)
    return result, hashes


def test_plan(inputs, workers, plan):
    shared = ROOT / "fixtures" / "shared.ass"
    primary = Path(plan["fixture"])
    animated, heavy, compatibility, ssa = sampled_fixtures(shared, inputs)
    scenarios = scale_fixtures(shared, inputs)
    probes = [("Worker verification", "overlap-64", name, count, 1, inputs / "overlap-64.ass", "worker-probe")
              for name in (BASE, CANDIDATE) for count in workers if count <= MAX_WORKERS[name]]
    common = {"idle": (shared, "idle"), "static": (shared, "static"), "animation": (animated, "animation"),
              "heavy": (heavy, "heavy"), "seek": (animated, "seek"), "real": (primary, "real")}
    checks = [("Visual regression", fixture.name, name, 1, 1, fixture, "regression")
              for fixture in (shared, animated, heavy, compatibility, ssa)
              for name in ("Original VSFilterMod", BASE, CANDIDATE)]
    checks.extend(("Visual regression", "lua-control", name, 1, 1, inputs / "lua-disabled.ass", "animation")
                  for name in (BASE, CANDIDATE))
    samples = []
    scaling = []
    ranges = {name: (inputs / f"{name}.ass", name if name in ("idle", "static") else "animation")
              for name in scenarios}
    ranges.update({"heavy": (heavy, "heavy"), "real": (primary, "real"), "kfx-sample": (primary, "real"),
                   "kfx-sustained": (primary, "playback")})
    settings = [(name, count) for count in workers for name in (BASE, CANDIDATE) if count <= MAX_WORKERS[name]]
    for key, (fixture, timeline) in common.items():
        for trial in range(1, TRIALS + 1):
            names = list(ENGINES)
            if trial % 2 == 0:
                names.reverse()
            samples.extend(("Single worker", key, name, 1, trial, fixture, timeline) for name in names)
    for key, (fixture, timeline) in ranges.items():
        for trial in range(1, TRIALS + 1):
            order = settings if trial % 2 else settings[::-1]
            scaling.extend(("Worker scaling", key, name, count, trial, fixture, timeline) for name, count in order)
    # Set aside the no-Lua controls until the corresponding Lua reference exists.
    return probes + samples + scaling + checks


def check_pixels(step, result, hashes, references, report):
    phase, workload, name, workers, trial, _, timeline = step
    label = f"{phase} / {workload} / {name} / {workers}w / trial {trial}"
    if timeline == "worker-probe":
        if result["observed_workers"] != workers:
            raise RuntimeError(f"{label}: expected {workers} workers, observed {result['observed_workers']}")
        report.write(f"{label}: worker count verified via dedicated overlap probe\n")
        return
    if not hashes:
        raise RuntimeError(f"{label}: no RGB hashes")
    if workload == "lua-control":
        if hashes == references["Worker scaling", "lua", CANDIDATE]:
            raise RuntimeError(f"{label}: disabling Lua did not change pixels")
        report.write(f"{label}: Lua-disabled control rendered different pixels\n")
        return
    group = "Oborodzuki" if name in (BASE, CANDIDATE) else name
    key = (phase, workload, group)
    expected = references.setdefault(key, hashes)
    if expected != hashes:
        index = next((i for i, pair in enumerate(zip(expected, hashes)) if pair[0] != pair[1]), None)
        raise RuntimeError(f"{label}: unexpected RGB mismatch at sample/frame {index} against {group} reference")
    if workload == "lua":
        references[phase, workload, CANDIDATE] = expected
    if phase == "Visual regression" and name == CANDIDATE:
        original = references[phase, workload, "Original VSFilterMod"]
        report.write(f"{label}: original renderer RGB {'matches' if original == hashes else 'differs (informational)'}\n")
    report.write(f"{label}: {len(hashes)} RGB hashes consistent with {group} reference\n")


def aggregate(samples):
    grouped = {}
    for row in samples:
        key = tuple(row[field] for field in ("phase", "workload", "renderer", "workers"))
        grouped.setdefault(key, []).append(row)
    summaries = []
    for key, rows in grouped.items():
        row = dict(zip(("phase", "workload", "renderer", "workers"), key))
        row["completed_trials"] = len(rows)
        row.update({metric: statistics.median(sample[metric] for sample in rows) for metric in METRICS})
        one = grouped.get((*key[:3], 1))
        row["throughput_scaling"] = row["fps"] / statistics.median(s["fps"] for s in one) if one else None
        row["efficiency"] = row["throughput_scaling"] / row["workers"] if one else None
        row["comparison"] = ("Feature-dependent reference" if row["workload"] == "real" and
                             row["renderer"] not in MAX_WORKERS else
                             "Same-renderer scaling" if key[0] == "Worker scaling" else
                             "Shared input, renderer-specific pixels")
        summaries.append(row)
    return sorted(summaries, key=lambda r: (r["phase"], r["workload"], r["renderer"], r["workers"]))


def csv_report(path, rows):
    columns = ["phase", "workload", "renderer", "workers", "completed_trials", *METRICS,
               "throughput_scaling", "efficiency", "comparison"]
    with path.open("w", newline="", encoding="utf-8-sig") as output:
        writer = csv.DictWriter(output, columns)
        writer.writeheader()
        writer.writerows(rows)


def charts(title, series, labels, unit):
    if not series:
        return ""
    palette = ["#215eb5", "#b54025", "#508030", "#7856a8", "#007376"]
    width, height, left, top, bottom = 640, 245, 63, 15, 175
    maximum = max(0.001, max(value for _, values in series for value in values.values()))
    spacing = (width - left - 25) / max(1, len(labels))
    parts = [f'<figure><figcaption>{html.escape(title)}</figcaption>',
             f'<svg viewBox="0 0 {width} {height}" role="img" aria-label="{html.escape(title)}">']
    for i in range(5):
        y = bottom - (bottom - top) * i / 4
        parts.append(f'<path stroke="#d1dce8" d="M{left},{y}H615"/>'
                     f'<text x="{left - 7}" y="{y + 4}" text-anchor="end">{maximum * i / 4:.3g}</text>')
    for col, label in enumerate(labels):
        x = left + col * spacing + spacing / 2
        parts.append(f'<text x="{x}" y="194" text-anchor="middle">{html.escape(str(label))}</text>')
        for index, (name, values) in enumerate(series):
            if label not in values:
                continue
            value = values[label]
            length = value / maximum * (bottom - top)
            bar_width = min(36, spacing / (len(series) + 1))
            bar_x = x + (index - len(series) / 2) * bar_width
            tip = html.escape(f"{name}: {value:.4g} {unit}")
            parts.append(f'<rect x="{bar_x}" y="{bottom - length}" width="{bar_width - 2}" '
                         f'height="{length}" fill="{palette[index]}"><title>{tip}</title></rect>')
    parts.append("</svg><ul>")
    for index, (name, _) in enumerate(series):
        parts.append(f'<li><span style="background:{palette[index]}"></span>{html.escape(name)}</li>')
    return "".join(parts) + "</ul></figure>"


def make_report(results, record):
    rows = aggregate(record["samples"])
    single = [row for row in rows if row["phase"] == "Single worker"]
    multi = [row for row in rows if row["phase"] == "Worker scaling"]
    csv_report(results / "single-thread.csv", single)
    csv_report(results / "multithreading.csv", multi)
    system = record["system"]
    builds = record.get("builds", {})
    notes = [
        f"Status: {record['status']} · {record['completed']}/{record['planned']} tests",
        f"Machine: {system.get('machine', '?')} · {system.get('cpu_registry') or platform.processor()} · "
        f"{system.get('physical_cores', '?')} cores / {system.get('logical_processors', '?')} logical processors",
        f"Workers tested where supported: {', '.join(map(str, record.get('workers', [])))}",
        "Renderer revisions: " + ", ".join(f"{key} {value['revision']}" for key, value in builds.items()),
        "Renderer wall time includes compositing. FPS is measured frames divided by summed rendering wall time. "
        "Warmup, clearing, RGB hashing and process startup are excluded from renderer timings.",
        "Aggregate columns are medians of per-trial statistics. Each speedup uses the same renderer and workload at one worker.",
        "The pinned baseline supports up to eight workers. Candidate-only worker settings use the candidate's one-worker reference.",
        "Original VSFilterMod and Mangetsu runs are descriptive comparisons. The real ASS uses extensions with differing renderer support. "
        "RGB equality is required between the candidate and Oborodzuki baseline, and across settings and trials.",
        "Fonts requested by the real fixture and absent from the font registry: "
        + (", ".join(record.get("unregistered_fixture_fonts", [])) or "none recorded"),
    ]
    if record.get("error"):
        notes.append(f"Error: {record['error']}")
    headings = ["Phase", "Workload", "Renderer", "Workers", "Trials", "Avg ms", "Median ms",
                "P95 ms", "P99 ms", "FPS", "Peak MiB", "Scaling", "Efficiency"]
    table = []
    for r in rows:
        table.append([r["phase"], r["workload"], r["renderer"], str(r["workers"]),
                      str(r["completed_trials"]), *(f"{r[key]:.4f}" for key in METRICS[:4]),
                      f"{r['fps']:.2f}", f"{r['peak_working_set_bytes'] / 2**20:.1f}",
                      f"{r['throughput_scaling']:.2f}×" if r["throughput_scaling"] is not None else "—",
                      f"{r['efficiency']:.1%}" if r["efficiency"] is not None else "—"])
    md = ["# Oborodzuki local benchmark", "", *[f"- {note}" for note in notes], "",
          "| " + " | ".join(headings) + " |", "|" + "---|" * len(headings)]
    md.extend("| " + " | ".join(row) + " |" for row in table)
    (results / "summary.md").write_text("\n".join(md) + "\n", encoding="utf-8")
    graphs = []
    for workload, selected in itertools.groupby(multi, key=lambda r: r["workload"]):
        selected = list(selected)
        labels = sorted({r["workers"] for r in selected})
        graphs.append(f'<details><summary>{html.escape(workload)} · worker scaling and latency</summary>')
        for metric, title, unit in (("throughput_scaling", "Throughput relative to one worker", "×"),
                                    ("efficiency", "Parallel efficiency", "ratio"),
                                    ("p99_ms", "99th-percentile latency", "ms")):
            series = [(name, {r["workers"]: r[metric] for r in selected
                              if r["renderer"] == name and r[metric] is not None}) for name in (BASE, CANDIDATE)]
            graphs.append(charts(title, [s for s in series if s[1]], labels, unit))
        graphs.append("</details>")
    for workload, selected in itertools.groupby(single, key=lambda r: r["workload"]):
        title = f"{workload} · renderer frame-time distribution percentiles"
        series = [(r["renderer"], {label: r[key] for label, key in
                   (("Median", "median_ms"), ("P95", "p95_ms"), ("P99", "p99_ms"))}) for r in selected]
        graphs.append(charts(title, series, ["Median", "P95", "P99"], "ms"))
    html_table = "<table><thead><tr>" + "".join(f"<th>{html.escape(v)}</th>" for v in headings) + "</tr></thead><tbody>"
    html_table += "".join("<tr>" + "".join(f"<td>{html.escape(v)}</td>" for v in row) + "</tr>" for row in table)
    document = """<!doctype html><html lang="en"><meta charset="utf-8">
<title>Oborodzuki local benchmark</title><style>
body{font:16px system-ui;margin:2rem auto;max-width:1280px;padding:0 1rem;color:#172638;background:#f5f8fc}
figure,details{background:white;border:1px solid #d1dce8;border-radius:8px;padding:1rem;margin:1rem 0}
figcaption,summary{font-weight:650} svg{width:100%;max-width:700px;font-size:12px}
figure ul{display:flex;flex-wrap:wrap;gap:1rem;list-style:none;padding:0}
figure span{display:inline-block;width:12px;height:12px;margin-right:5px}
table{border-collapse:collapse;background:white;width:100%;font-size:14px;font-variant-numeric:tabular-nums}
td,th{padding:.45rem;border:1px solid #d1dce8;text-align:left} th{background:#eaf0fb}
.scroll{overflow:auto}</style><main><h1>Oborodzuki local benchmark</h1>"""
    document += "<ul>" + "".join(f"<li>{html.escape(note)}</li>" for note in notes) + "</ul>"
    document += "<h2>Charts</h2>" + "".join(graphs) + '<h2>Measurements</h2><div class="scroll">'
    document += html_table + "</tbody></table></div></main></html>"
    (results / "summary.html").write_text(document, encoding="utf-8")
    record["aggregates"] = rows
    write_json(results / "results.json", record)


def main(results):
    raw = results / "raw"
    inputs = results / "inputs"
    raw.mkdir()
    inputs.mkdir()
    record = {"schema": 1, "status": "incomplete", "planned": 0, "completed": 0,
              "system": read_json(results / "system-info.json"), "samples": [], "validation": []}
    write_json(results / "results.json", record)
    result_code = 1
    progress = None
    with ((results / "benchmark.log").open("a", encoding="utf-8", buffering=1) as log,
          (results / "correctness-report.txt").open("w", encoding="utf-8", buffering=1) as checks,
          (results / "frame-times.csv").open("w", newline="", encoding="utf-8-sig") as times):
        writer = csv.writer(times)
        writer.writerow(("phase", "workload", "renderer", "workers", "trial", "sequence", "frame", "render_ms"))
        try:
            manifest = verify_package()
            record["artifact"] = (read_json(results / "artifact.json") if (results / "artifact.json").exists()
                                  else {"run_id": manifest["run_id"], "run_attempt": manifest["run_attempt"],
                                        "source": "exact revision in extracted portable ZIP"})
            record["builds"] = {row["kind"]: {**read_json(renderer_path(row["kind"]).with_name("build.json")),
                                  "dll_sha256": sha256(renderer_path(row["kind"]))} for row in manifest["renderers"]}
            record["harness"] = read_json(HOST / "build.json")
            record["system"].update(windows_hardware(), registered_fonts=registered_fonts(), python=platform.python_version())
            system = record["system"]
            workers = worker_counts(system["available_logical_processors"], system["physical_cores"])
            record["workers"] = workers
            plan = inspect_fixture(ROOT / "fixtures" / "op.fx.4testing.ass")
            known = set(map(str.casefold, system["registered_fonts"]))
            record["unregistered_fixture_fonts"] = [font for font in plan["style_fonts"] if font.casefold() not in known]
            record["configuration"] = {"resolution": [1920, 1080], "trials": TRIALS,
                                       "sampled_frames": 120, "sampled_warmup": 10,
                                       "real_frames": 60, "real_warmup": 6,
                                       "kfx_sample_ms": [55000, 65000],
                                       "kfx_sustained_frames": plan["frames"], "kfx_sustained_fps": plan["fps"]}
            write_json(results / "input-plan.json", plan)
            write_json(results / "system-info.json", system)
            steps = test_plan(inputs, workers, plan)
            record["planned"] = len(steps)
            write_json(results / "results.json", record)
            progress = Progress(len(steps))
            references = {}
            print(f"Candidate {manifest['revision']} | baseline {BASELINE}", flush=True)
            print(f"Available logical processors: {system['available_logical_processors']} | worker counts: {workers}", flush=True)
            for index, step in enumerate(steps, 1):
                phase, workload, engine, count, trial, _, _ = step
                progress.begin(index, phase, workload, engine, count)
                log.write(f"{datetime.now().astimezone().isoformat()} {index}/{len(steps)} {phase} "
                          f"{workload} {engine}, {count} workers, trial {trial}\n")
                start = time.perf_counter()
                row, hashes = run_step(step, index, raw / f"{index:04}", inputs, plan, log, progress)
                row["process_ms"] = (time.perf_counter() - start) * 1000
                measured = row.pop("frame_times_ms", [])
                if phase in ("Single worker", "Worker scaling"):
                    if (not measured or len(measured) != row["frames"] or
                            any(not math.isfinite(value) or value < 0 for value in measured) or row["fps"] <= 0):
                        raise RuntimeError(f"Missing or invalid frame times for test {index}")
                    writer.writerows((phase, workload, engine, count, trial, index, frame, value)
                                     for frame, value in enumerate(measured))
                    times.flush()
                    row["correctness"] = "pending"
                    record["samples"].append(row)
                else:
                    row["correctness"] = "pending"
                    record["validation"].append(row)
                try:
                    check_pixels(step, row, hashes, references, checks)
                except RuntimeError:
                    row["correctness"] = "failed"
                    raise
                row["correctness"] = "passed"
                record["completed"] = index
                write_json(results / "results.json", record)
            record["status"] = "completed"
            result_code = 0
        except (Exception, KeyboardInterrupt) as error:
            record["status"] = "interrupted" if isinstance(error, KeyboardInterrupt) else "failed"
            record["error"] = str(error) or "Interrupted by the user"
            log.write(record["error"] + "\n")
            checks.write(f"{record['status'].upper()}: {record['error']}\n")
        finally:
            record["finished_at"] = datetime.now().astimezone().isoformat()
            if progress:
                progress.finish(record["completed"])
            if result_code:
                print(record["error"], file=sys.stderr, flush=True)
            make_report(results, record)
    return result_code


if __name__ == "__main__":
    if sys.argv[1:] == ["--check-package"]:
        smoke_package()
    else:
        sys.exit(main(Path(sys.argv[1]).resolve()))
