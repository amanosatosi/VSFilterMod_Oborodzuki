"""Windows CI pixel and deterministic-seek checks using the existing CSRI/RGBA hosts."""
import json
import os
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path("parallel-results/paint")
ROOT.mkdir(parents=True, exist_ok=True)
HEADER = Path(".github/ci/shared.ass").read_text(encoding="utf-8").split("Dialogue:", 1)[0]
RED, BLUE, GREEN, WHITE, BLACK = "&H0000FF&", "&HFF0000&", "&H00FF00&", "&HFFFFFF&", "&H000000&"
BOX = "m 0 0 l 200 0 200 100 0 100"
HOST = Path("hosts/kfx-playback.exe").resolve()
BENCH = Path("hosts/render-bench.exe").resolve()
CANDIDATE = Path("renderer/renderer.dll").resolve()
MANG = Path("mangetsu-paint/renderer.dll").resolve()


def gd(angle, *stops):
    return rf"\grd({angle}," + ",".join(stops) + ")"


def t(tags, accel=""):
    return rf"\t(0,1000,{accel}{tags})"


def write(name, entries):
    path = ROOT / (name + ".ass")
    ass = HEADER
    for index, (x, y, tags, body) in enumerate(entries):
        ass += f"Dialogue: {index % 3},0:00:01.00,0:00:05.00,Default,,0,0,0,,"
        ass += rf"{{\an7\pos({x},{y})\bord0\shad0{tags}}}" + body + "\n"
    path.write_text(ass, encoding="utf-8-sig")
    return path.resolve(), len(entries)


linear = gd(0, RED, BLUE)
slide = gd(0, RED, RED) + r"\grds0" + t(gd(0, BLUE, BLUE))
fade = rf"\1c{RED}\fad(1000,1000,{WHITE},{BLACK})"
heavy = write("animated-scenes", [
    (100 + (i % 4) * 375, 65 + (i % 5) * 165,
     gd(18, RED, "35%", GREEN, BLUE) + r"\1gra(90,&H00&,&HC0&)\grds35" +
     t(gd(-30, BLUE, "65%", WHITE, RED), "2,") +
     rf"\t(200,1600,\1gra(0,&H10&,&H30&))\fad(400,300,{BLACK}+a,{WHITE})"
     r"\3grd(0,$white,$black)\4grd(90,$black,$white)\bord3\shad5"
     r"\clip(m 160 80 l 1700 80 1700 940 160 940)\blendadd\kf100",
     "Paint scenes across workers")
    for i in range(20)
])
legacy = write("animated-scenes-solid", [
    (100 + (i % 4) * 375, 65 + (i % 5) * 165,
     rf"\1c{RED}\t(0,1000,2,\1c{BLUE})\fad(400,300)"
     r"\bord3\shad5\clip(m 160 80 l 1700 80 1700 940 160 940)\blendadd\kf100",
     "Paint scenes across workers")
    for i in range(20)
])


def benchmark(dll, scenario, workers, mode):
    env = dict(os.environ, OBORODZUKI_WORKERS=str(workers), OBORODZUKI_BENCH_ITERATION="1")
    p = subprocess.run([str(BENCH), "csri", str(dll), str(scenario[0]), mode], check=True,
                       env=env, capture_output=True, text=True, timeout=180)
    result = json.loads(p.stdout)
    if mode == "animation" and result["frames"] != 120:
        raise RuntimeError("Render benchmark did not receive its iteration environment")
    return result


if sys.argv[1:] == ["--profile"]:
    results = [dict(benchmark(Path("bundle/renderer.dll").resolve(), scene, n, "animation"),
                    scene=scene[0].stem, workers=n)
               for scene in (legacy, heavy) for n in (1, 2, 4)]
    for result in results:
        band_plan = next(stage for stage in result["profile"] if stage["name"] == "band_plan")
        if bool(band_plan["calls"]) != (result["workers"] > 1):
            raise RuntimeError(f"Unexpected band planning at {result['workers']} workers")
    (ROOT / "paint-profile.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
    sys.exit()

cases = {
    "linear": linear,
    "repeat": gd(0, RED, "45%", GREEN, "45%", BLUE, WHITE),
    "alpha": r"\1c&HFFFFFF&\１gra(0,$０,$၂၅၅)",
    "reset": linear + r"\1c&H00FF00&\1gra(0,&H40&,&HC0&)\1gra()",
    "malformed": linear + r"\grd(0,$white,45,,$black)\grd(90,$white,$black",
    "angle": gd(350, RED, BLUE) + t(gd(10, RED, BLUE)),
    "union": gd(0, RED, "40%", GREEN, BLUE) + t(gd(90, BLUE, "65%", WHITE, RED), "2,"),
    "slide": slide,
    "wide-slide": slide.replace(r"\grds0", r"\grds1000"),
    "off": slide.replace(r"\grds0", r"\grds0\1grdsoff"),
    "inherit": slide.replace(r"\grds0", r"\grds0\1grds120\1grds()"),
    "overlap": slide + r"\t(250,1250,\1grd(90,&H00FF00&,&H00FF00&))",
    "solid-slide": rf"\1c{RED}\grds0" + t(gd(0, BLUE, BLUE)),
    "gradient-solid": gd(0, RED, RED) + r"\grds0" + t(rf"\1c{BLUE}"),
    "gradient-solid-ordinary": gd(0, RED, BLUE) + t(rf"\1c{GREEN}"),
    "fade": fade,
    "fade-alpha": fade.replace(WHITE, WHITE + "+a"),
    "fade-bad": fade.replace(WHITE, "invalid"),
    "fade-empty": fade.replace(WHITE, ""),
    "fade-gradient": linear + rf"\fad(1000,1000,{WHITE},{BLACK})" + t(gd(90, BLUE, RED)),
    "legacy": r"\1vc(&H0000FF&,&H00FF00&,&HFF0000&,&HFFFFFF&)\1va(0,30,60,90)",
    "clip": linear,
    "vector-clip": linear,
    "names": gd(0, "$SHIRO", "$kuro"),
    "outline-shadow": (gd(0, RED, BLUE) + r"\3grd(90,$white,$black)"
                       r"\4grd(0,&H00FF00&,&H00FF00&)\bord8\shad16\4a&H00&"),
    "alpha-slide": r"\1c&HFFFFFF&\1gra(0,&H00&,&H00&)\1gras0" + t(r"\1gra(0,&HC0&,&HC0&)"),
    "raised-fill-alpha": (r"\1c&HFFFFFF&\1a&HFF&\1gra(0,&H00&,&H30&)"
                          r"\4c&H00FF00&\4a&H00&\shad16"),
}
coords = {}
entries = []
for i, (case, tags) in enumerate(cases.items()):
    x, y = 75 + (i % 5) * 340, 60 + (i // 5) * 163
    coords[case] = (x, y)
    if case == "clip":
        tags += rf"\clip({x+80},{y},{x+200},{y+100})"
    if case == "vector-clip":
        tags += rf"\clip(m {x+80} {y} l {x+200} {y} {x+200} {y+100} {x+80} {y+100})"
    if case == "malformed":
        # Keep the unclosed tag at the end of the override block.
        tags = r"\p1" + tags
    else:
        tags += r"\p1"
    entries.append((x, y, tags, BOX))
coords["karaoke"] = (1350, 960)
entries.append((1350, 960, r"\fs64\kf100\1grd(0,&H0000FF&,&H0000FF&)\2grd(0,&HFF0000&,&HFF0000&)", "MMMM"))
grid = write("pixel-grid", entries)
frames = [60, 75, 90, 120, 270]


def playback(mode, dll, case, path, workers=1):
    env = dict(os.environ, OBORODZUKI_WORKERS=str(workers))
    subprocess.run([str(HOST), mode, str(dll), str(case[0]), str(path.resolve()), str(case[1]),
                    str(case[0].stat().st_size), *map(str, frames),
                    "--window", str(frames[0]), str(frames[-1] + 1)], check=True, env=env, timeout=180)


def captures(path):
    images = {frame: (path / f"frame-{frame}.bmp").read_bytes() for frame in frames}
    offsets = {frame: struct.unpack_from("<I", image, 10)[0] for frame, image in images.items()}

    def get(frame, case, x=100, y=50):
        left, top = coords[case]
        i = offsets[frame] + ((top + y) * 1920 + left + x) * 4
        b, g, r = images[frame][i:i + 3]
        return r, g, b
    return get


def close(actual, expected, tolerance=8):
    if any(abs(a - b) > tolerance for a, b in zip(actual, expected)):
        raise AssertionError(f"RGB {actual} differs from {expected} by more than {tolerance}")


playback("csri", CANDIDATE, grid, ROOT / "oborodzuki")
playback("rgba", MANG, grid, ROOT / "mangetsu")
new = captures(ROOT / "oborodzuki")
reference = captures(ROOT / "mangetsu")
probes = [
    (60, "linear", 25, 50, (223, 0, 32)),
    (60, "linear", 175, 50, (31, 0, 224)),
    (60, "repeat", 87, 50, (7, 248, 0)),
    (60, "repeat", 94, 50, (11, 11, 255)),
    (60, "alpha", 25, 50, (223, 223, 223)),
    (60, "reset", 75, 50, (0, 255, 0)),
    (60, "malformed", 25, 50, (223, 0, 32)),
    (90, "angle", 25, 50, (223, 0, 32)),
    (90, "slide", 25, 50, (0, 0, 255)),
    (90, "slide", 175, 50, (255, 0, 0)),
    (60, "wide-slide", 100, 50, (255, 0, 0)),
    (90, "wide-slide", 100, 50, (128, 0, 127)),
    (120, "wide-slide", 100, 50, (0, 0, 255)),
    (90, "off", 25, 50, (127, 0, 127)),
    (90, "inherit", 25, 50, (0, 0, 255)),
    (90, "solid-slide", 25, 50, (0, 0, 255)),
    (90, "gradient-solid", 25, 50, (0, 0, 255)),
    (90, "overlap", 25, 20, (0, 255, 0)),
    (90, "overlap", 25, 70, (0, 0, 255)),
    (90, "overlap", 175, 70, (255, 0, 0)),
    (60, "fade", 100, 50, (255, 255, 255)),
    (90, "fade", 100, 50, (255, 127, 127)),
    (270, "fade", 100, 50, (127, 0, 0)),
    (90, "fade-alpha", 100, 50, (128, 64, 64)),
    (60, "fade-bad", 100, 50, (0, 0, 0)),
    (60, "fade-empty", 100, 50, (0, 0, 0)),
    (60, "clip", 25, 50, (0, 0, 0)),
    (60, "clip", 100, 50, (127, 0, 128)),
    (60, "vector-clip", 25, 50, (0, 0, 0)),
    (60, "vector-clip", 100, 50, (127, 0, 128)),
    (60, "names", 25, 50, (223, 223, 223)),
    (90, "alpha-slide", 25, 50, (63, 63, 63)),
    (90, "alpha-slide", 175, 50, (255, 255, 255)),
    (60, "outline-shadow", -4, 50, (127, 127, 127)),
    (60, "outline-shadow", 213, 108, (0, 255, 0)),
]
for frame, case, x, y, expected in probes:
    try:
        close(new(frame, case, x, y), expected)
        close(reference(frame, case, x, y), expected)
        close(new(frame, case, x, y), reference(frame, case, x, y))
    except AssertionError as error:
        raise AssertionError(f"{case} at frame {frame}, pixel ({x},{y}): {error}") from error

for case in ("union", "fade-gradient", "overlap"):
    for x, y in [(25,20), (155,45), (80,78)]:
        close(new(90, case, x, y), reference(90, case, x, y), tolerance=14)
    assert new(60, case, 25, 20) != new(90, case, 25, 20), case

close(new(90, "gradient-solid-ordinary", 25, 50), (111, 127, 16))
close(new(120, "gradient-solid-ordinary"), (0, 255, 0))
close(new(60, "raised-fill-alpha", 208, 108), (0, 255, 0))

for frame, color in [(60, 2), (120, 0)]:
    assert sum(new(frame, "karaoke", x, y)[color] > 200
               for x in range(250) for y in range(85)) > 100, "karaoke channel unavailable"

# Check old corner math against the validated VSFilterMod path rather than
# requiring its different legacy bitmap geometry to agree with libass.
playback("csri", Path("threading-baseline/renderer/renderer.dll").resolve(), grid, ROOT / "previous")
previous = captures(ROOT / "previous")
for x in (25, 100, 175):
    for y in (25, 50, 75):
        assert new(90, "legacy", x, y) == previous(90, "legacy", x, y)

print("Analytic and Mangetsu pixel checks passed.", flush=True)
expected_hashes = None
for n in (1, 2, 4, 6, 8):
    hashes = benchmark(CANDIDATE, heavy, n, "regression")["hashes"]
    if expected_hashes is None:
        expected_hashes = hashes
    assert hashes == expected_hashes, f"gradient seek/frame hashes differ at {n} workers"

print("Backward seeks and five worker budgets produced identical hashes.", flush=True)
results = []
settings = [(scene, n) for n in (1, 2, 4) for scene in (legacy, heavy)]
for trial in range(3):
    for scene, n in settings if trial % 2 == 0 else reversed(settings):
        results.append(dict(benchmark(CANDIDATE, scene, n, "animation"),
                            scene=scene[0].stem, workers=n, trial=trial + 1))
(ROOT / "paint-benchmarks.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
print("Paint pixels, Mangetsu reference samples, backward seeks and five worker budgets passed.", flush=True)
