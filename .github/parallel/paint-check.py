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
ROUND2 = os.environ.get("KFX_ROUND2") == "true"


def gd(angle, *stops):
    return rf"\grd({angle}," + ",".join(stops) + ")"


def t(tags, accel=""):
    return rf"\t(0,1000,{accel}{tags})"


def write(name, entries):
    path = ROOT / (name + ".ass")
    ass = HEADER
    for index, (x, y, tags, body) in enumerate(entries):
        ass += f"Dialogue: {index % 3},0:00:01.00,0:00:05.00,Default,,0,0,0,,"
        position = "" if r"\move(" in tags else rf"\pos({x},{y})"
        alignment = "" if r"\an" in tags else r"\an7"
        ass += rf"{{{alignment}{position}\bord0\shad0{tags}}}" + body + "\n"
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
if ROUND2:
    cases.update({
        "rings": (r"\bord5\3c&H0000FF&\3a&H00&\2bs3\2bc&H00FF00&\3bs2\3bc&HFF0000&"),
        "ring-axes": r"\1bs0\2bsx8\2bsy0\2bc&HFF0000&",
        "ring-alpha": r"\bord4\3c&H0000FF&\2bs4\2bc&H00FF00&\3a&H80&\1ba&H00&",
        "ring-motion": r"\2bs2\2bc&HFF0000&\t(0,1000,\2bs10)",
        "ring-gradient": r"\bord2\2bs8\2bgrd(0,&H0000FF&,&HFF0000&)\2bga(90,&H00&,&H00&)",
        "ring-wait": r"\2bs8\2bc&H00FF00&\2bsc&HFF0000&\kt25\k25",
    })
coords = {}
entries = []
for i, (case, tags) in enumerate(cases.items()):
    x, y = 75 + (i % 5) * 340, 60 + (i // 5) * (138 if ROUND2 else 163)
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
coords["karaoke"] = (1350, 986 if ROUND2 else 960)
entries.append((*coords["karaoke"], r"\fs64\kf100\1grd(0,&H0000FF&,&H0000FF&)\2grd(0,&HFF0000&,&HFF0000&)", "MMMM"))
grid = write("pixel-grid", entries)
frames = [60, 75, 90, 120, 270]


def playback(mode, dll, case, path, workers=1):
    env = dict(os.environ, OBORODZUKI_WORKERS=str(workers))
    subprocess.run([str(HOST), mode, str(dll), str(case[0]), str(path.resolve()), str(case[1]),
                    str(case[0].stat().st_size), *map(str, frames),
                    "--window", str(frames[0]), str(frames[-1] + 1)], check=True, env=env, timeout=180)


def captures(path, positions=coords):
    images = {frame: (path / f"frame-{frame}.bmp").read_bytes() for frame in frames}
    offsets = {frame: struct.unpack_from("<I", image, 10)[0] for frame, image in images.items()}

    def get(frame, case, x=100, y=50):
        left, top = positions[case]
        if not (0 <= left + x < 1920 and 0 <= top + y < 1080):
            raise ValueError(f"Pixel probe for {case} is outside the frame")
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
if ROUND2:
    probes.extend([
        (60, "rings", -3, 50, (255, 0, 0)),
        (60, "rings", -7, 50, (0, 255, 0)),
        (60, "rings", -9, 50, (0, 0, 255)),
        (60, "rings", -15, 50, (0, 0, 0)),
        (60, "ring-axes", -5, 50, (0, 0, 255)),
        (60, "ring-axes", 100, -5, (0, 0, 0)),
        (60, "ring-alpha", -2, 50, (255, 0, 0)),
        (60, "ring-alpha", -6, 50, (0, 127, 0)),
        (60, "ring-motion", -5, 50, (0, 0, 0)),
        (90, "ring-motion", -5, 50, (0, 0, 255)),
        (60, "ring-gradient", 25, -6, (214, 0, 41)),
        (60, "ring-gradient", 175, -6, (41, 0, 214)),
        (60, "ring-wait", -5, 50, (0, 0, 255)),
        (90, "ring-wait", -5, 50, (0, 255, 0)),
    ])
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

if ROUND2:
    boxes = write("event-boxes", [
        (120, 90, r"\bs4\4c&HFF0000&\4a&H00&\boxp(10,20,8,12)\bbs4\bbc&H00FF00&"
                  r"\2bbs6\2bbgrd(0,&H0000FF&,&H0000FF&)\p1", BOX),
        (500, 90, r"\bs1\box1\4c&HFF0000&\4a&H00&\boxp16\boxr14\clip(496,60,800,250)\p1", BOX),
        (900, 90, r"\bs4\box0\4c&HFF0000&\4a&H00&\boxp16\p1", BOX),
    ])
    locations = {"rings": (120, 90), "rounded": (500, 90), "off": (900, 90)}
    playback("csri", CANDIDATE, boxes, ROOT / "boxes")
    playback("rgba", MANG, boxes, ROOT / "boxes-reference")
    tested = captures(ROOT / "boxes", locations)
    expected_box = captures(ROOT / "boxes-reference", locations)
    for case, x, y, expected in [
        ("rings", -6, 50, (0, 0, 255)),
        ("rings", -12, 50, (0, 255, 0)),
        ("rings", -17, 50, (255, 0, 0)),
        ("rings", -23, 50, (0, 0, 0)),
        ("rings", 212, 50, (0, 0, 255)),
        ("rounded", -5, 50, (0, 0, 0)),
        ("rounded", 100, -8, (0, 0, 255)),
        ("off", -6, 50, (0, 0, 0)),
    ]:
        close(tested(60, case, x, y), expected)
        close(expected_box(60, case, x, y), expected)

    fixed = write("positioned-paint", [
        (100, 460, r"\move(100,460,200,460,0,1000)\1c&H00FF00&"
                   r"\pgrd(160,460,260,560,0,&H0000FF&,&HFF0000&)\p1", BOX),
    ])
    locations = {"moving": (0, 460)}
    playback("csri", CANDIDATE, fixed, ROOT / "fixed")
    playback("rgba", MANG, fixed, ROOT / "fixed-reference")
    moving = captures(ROOT / "fixed", locations)
    fixed_reference = captures(ROOT / "fixed-reference", locations)
    for frame, x, rgb in [
        (60, 145, (0, 255, 0)), (60, 220, (101, 0, 154)), (90, 220, (101, 0, 154)),
        (120, 220, (101, 0, 154)), (120, 285, (0, 255, 0)),
    ]:
        close(moving(frame, "moving", x), rgb)
        close(fixed_reference(frame, "moving", x), rgb)

    numeric = write("numeric-overrides", [
        (120, 90, r"\1c$white\bord($height/20)\3c&H0000FF&\3a$0\2bs~+3"
                  r"\2bc&H00FF00&\t(0,25%,\2bs~+4)\p1", BOX),
    ])
    playback("csri", CANDIDATE, numeric, ROOT / "numeric")
    evaluated = captures(ROOT / "numeric", {"numbers": (120, 90)})
    for frame, x, color in [
        (60, -3, (255, 0, 0)), (60, -6, (0, 255, 0)), (60, -10, (0, 0, 0)),
        (90, -9, (0, 255, 0)), (120, -11, (0, 255, 0)),
    ]:
        close(evaluated(frame, "numbers", x, 50), color)

    motion = write("competing-motion", [
        (500, 500, r"\1c$white\t(0,500,\pos(200,500))\t(250,800,\pos(~+350,~+100))\p1", BOX),
        (120, 90, r"\1c$white\clip(120,90,210,190)\clippos(30,0)\p1", BOX),
        (1100, 90, r"\1c$white\scale50\p1", BOX),
    ])
    locations = {"moving": (0, 0), "clip": (120, 90), "scale": (1100, 90)}
    playback("csri", CANDIDATE, motion, ROOT / "motion")
    playback("rgba", MANG, motion, ROOT / "motion-reference")
    actual = captures(ROOT / "motion", locations)
    expected_motion = captures(ROOT / "motion-reference", locations)
    for frame, case, x, y, color in [
        (60, "moving", 650, 550, (255, 255, 255)),
        (60, "moving", 430, 550, (0, 0, 0)),
        (75, "moving", 352, 550, (255, 255, 255)),
        (90, "moving", 365, 470, (255, 255, 255)),
        (90, "moving", 350, 470, (0, 0, 0)),
        (120, "moving", 715, 440, (255, 255, 255)),
        (60, "clip", 25, 50, (0, 0, 0)),
        (60, "clip", 35, 50, (255, 255, 255)),
        (60, "scale", 85, 25, (255, 255, 255)),
        (60, "scale", 115, 25, (0, 0, 0)),
    ]:
        try:
            close(actual(frame, case, x, y), color)
            close(expected_motion(frame, case, x, y), color)
        except AssertionError as error:
            raise AssertionError(f"{case} at {frame} ({x},{y}): {error}") from error

    geometry = write("geometric-borders", [
        (120, 90, r"\bs5\bord12\3c&H0000FF&\2bs8\2bc&HFF0000&\p1", BOX),
        (500, 90, r"\bord12\3c&H0000FF&\p1", BOX),
        (900, 90, r"\an9\tan7\1c$white\p1", BOX),
    ])
    locations = {"miter": (120, 90), "round": (500, 90), "anchor": (900, 90)}
    playback("csri", CANDIDATE, geometry, ROOT / "geometry")
    playback("rgba", MANG, geometry, ROOT / "geometry-reference")
    actual = captures(ROOT / "geometry", locations)
    expected_geometry = captures(ROOT / "geometry-reference", locations)
    for case, x, y, color in [
        ("miter", -10, -10, (255, 0, 0)),
        ("miter", -17, -17, (0, 0, 255)),
        ("round", -10, -10, (0, 0, 0)),
        ("anchor", 25, 50, (255, 255, 255)),
        ("anchor", -25, 50, (0, 0, 0)),
    ]:
        try:
            close(actual(60, case, x, y), color)
            close(expected_geometry(60, case, x, y), color)
        except AssertionError as error:
            raise AssertionError(f"{case} at ({x},{y}): {error}") from error

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
scenes = [heavy]
if ROUND2:
    combined = write("combined-extension-seeks", [
        (140 + (i % 4) * 330, 150 + (i // 4) * 130,
         r"\bs5\bord($height/30)\2bs5\2bgrd(30,$white,$black)\2bga(90,$0,$150)"
         r"\t(0,25%,\2bs~+2\2bgrd(90,$black,$white))\grds($width/4)"
         r"\pgrd(100,100,1600,900,40,&H00FF00&,$white)\t(300,800,\pos(~+15,~-8))"
         r"\fad(400,250,&HFF0000&+a,$black)\p1"
         if i % 2 else r"\bs4\boxp(7,9,8,12)\boxr10\bbs4\bbgrd(65,$white,$black)"
                       r"\4a$0\t(200,850,\frz20\scale90)\1grd(45,$white,$black)\p1",
         BOX)
        for i in range(16)
    ])
    scenes.append(combined)
for scene in scenes:
    expected_hashes = benchmark(CANDIDATE, scene, 1, "regression")["hashes"]
    for n in (2, 4, 6, 8):
        hashes = benchmark(CANDIDATE, scene, n, "regression")["hashes"]
        assert hashes == expected_hashes, f"{scene[0].stem} seek/frame hashes differ at {n} workers"

print("Backward seeks and five worker budgets produced identical hashes.", flush=True)
results = []
settings = [(scene, n) for n in (1, 2, 4) for scene in (legacy, heavy)]
for trial in range(3):
    for scene, n in settings if trial % 2 == 0 else reversed(settings):
        results.append(dict(benchmark(CANDIDATE, scene, n, "animation"),
                            scene=scene[0].stem, workers=n, trial=trial + 1))
(ROOT / "paint-benchmarks.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
print("Paint pixels, Mangetsu reference samples, backward seeks and five worker budgets passed.", flush=True)
