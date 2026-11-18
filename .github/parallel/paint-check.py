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
ROUND3 = os.environ.get("KFX_ROUND3") == "true"


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

combined = None
if ROUND2:
    combined = write("combined-extensions", [
        (140 + (i % 4) * 330, 150 + (i // 4) * 130,
         r"\bs5\bord($height/30)\2bs5\2bgrd(30,$white,$black)\2bga(90,$0,$150)"
         r"\t(0,25%,\2bs~+2\2bgrd(90,$black,$white))\grds($width/4)"
         r"\pgrd(100,100,1600,900,40,&H00FF00&,$white)\t(300,800,\pos(~+15,~-8))"
         r"\pgrdalpha(100,100,1600,900,20,$0,$120)"
         r"\fad(400,250,&HFF0000&+a,$black)\p1"
         if i % 2 else r"\bs4\boxp(7,9,8,12)\boxr10\bbs4\bbgrd(65,$white,$black)"
                       r"\4a$0\t(200,850,\frz20\scale90)\1grd(45,$white,$black)\p1",
         BOX)
        for i in range(16)
    ])
paint_scenes = [legacy, heavy] + ([combined] if combined else [])


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
               for scene in paint_scenes for n in (1, 2, 4)]
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
    script = numeric[0].read_text(encoding="utf-8-sig")
    numeric[0].write_text(script.replace("PlayResX: 1920", "PlayResX: ၁၉၂၀")
                          .replace("Default,Arial,48,", "Default,Arial,၄၈,")
                          .replace("0:00:01.00,0:00:05.00", "０:００:０１.００,０:００:０５.００"),
                          encoding="utf-8-sig")
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

    decoration = write("decoration-paint", [
        (120, 90, r"\fs64\u1\1c$white\5c&HFF0000&\5a$128", "MMMM"),
        (520, 90, r"\fs64\s1\1c$white\5grd(0,&H0000FF&,&HFF0000&)", "MMMM"),
    ])
    positions = {"underline": (120, 90), "strike": (520, 90)}
    playback("csri", CANDIDATE, decoration, ROOT / "decoration")
    playback("rgba", MANG, decoration, ROOT / "decoration-reference")
    for getter in (captures(ROOT / "decoration", positions),
                   captures(ROOT / "decoration-reference", positions)):
        underline = [getter(90, "underline", x, y) for y in range(85) for x in range(265)]
        strike = [getter(90, "strike", x, y) for y in range(85) for x in range(265)]
        assert sum(112 <= b <= 145 and r < 12 and g < 12 for r, g, b in underline) >= 30
        assert sum(r > 210 and g > 210 and b > 210 for r, g, b in underline) >= 100
        assert sum(r > 110 and r > 2*b and g < 12 for r, g, b in strike) >= 15
        assert sum(b > 110 and b > 2*r and g < 12 for r, g, b in strike) >= 15

    details = write("ring-and-slide-regressions", [
        (120, 120, r"\bord4\3c&H0000FF&\2bs6\2bc&H00FF00&\2bblur2\p1", BOX),
        (480, 120, r"\bord4\3c&H0000FF&\2bs6\2bc&H00FF00&\blur2\1bblur0\2bblur0\p1", BOX),
        (840, 120, r"\10bs5\10bc&HFF0000&\p1", BOX),
        (1200, 120, (r"\grds($width/4)" + gd(0, RED, RED) + t(gd(0, BLUE, BLUE)) + r"\p1"), BOX),
    ])
    locations = {"blur": (120, 120), "sharp": (480, 120), "ten": (840, 120), "slide": (1200, 120)}
    playback("csri", CANDIDATE, details, ROOT / "details")
    playback("rgba", MANG, details, ROOT / "details-reference")
    for getter in (captures(ROOT / "details", locations), captures(ROOT / "details-reference", locations)):
        assert getter(60, "blur", -12, 50)[1] > 5
        close(getter(60, "sharp", -12, 50), (0, 0, 0))
        close(getter(60, "ten", -3, 50), (0, 0, 255))
        close(getter(90, "slide", 25, 50), (0, 0, 255))
        close(getter(90, "slide", 175, 50), (255, 0, 0))

    alignment = write("logical-line-alignment", [
        (120, 120, r"\fs48\1c$white\ta7", r"MMMM{\ta3}\NM\NM"),
    ])
    locations = {"lines": (120, 120)}
    playback("csri", CANDIDATE, alignment, ROOT / "alignment")
    playback("rgba", MANG, alignment, ROOT / "alignment-reference")
    for getter in (captures(ROOT / "alignment", locations), captures(ROOT / "alignment-reference", locations)):
        first_x = []
        in_line = False
        for y in range(200):
            hits = [x for x in range(210) if min(getter(90, "lines", x, y)) > 200]
            if hits and not in_line:
                first_x.append(hits[0])
            in_line = bool(hits)
        assert len(first_x) == 3 and first_x[0] + 45 < first_x[1] and abs(first_x[1] - first_x[2]) <= 2, first_x

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

    opacity = write("positioned-opacity", [
        (120, 90, r"\1c&H0000FF&\4c&H00FF00&\4a$0\shad8"
                  r"\pgrdalpha(145,70,270,210,0,$128,$128)\p1", BOX),
        (500, 90, r"\bs4\4c&HFF0000&\4a$0\boxp10"
                  r"\pgrdalpha(490,70,720,220,90,$255,$255)\p1", BOX),
        (900, 90, r"\bord8\3c&H00FF00&\3sgrd(0,&H0000FF&,&HFF0000&)\kt25\k25\p1", BOX),
        (1300, 90, r"\bord8\3c&H00FF00&\kt25\kO25\p1", BOX),
    ])
    positions = {"mask": (120, 90), "box": (500, 90), "wait": (900, 90), "reveal": (1300, 90)}
    playback("csri", CANDIDATE, opacity, ROOT / "opacity")
    playback("rgba", MANG, opacity, ROOT / "opacity-reference")
    actual = captures(ROOT / "opacity", positions)
    expected_opacity = captures(ROOT / "opacity-reference", positions)
    for frame, case, x, y, color in [
        (60, "mask", 50, 50, (127, 0, 0)),
        (60, "mask", 180, 50, (255, 0, 0)),
        (60, "box", 50, 50, (0, 0, 255)),
        (60, "wait", 25, -4, (215, 0, 40)),
        (90, "wait", 25, -4, (0, 255, 0)),
        (60, "reveal", 50, 50, (0, 0, 0)),
        (90, "reveal", 50, 50, (255, 255, 239)),
    ]:
        try:
            close(actual(frame, case, x, y), color)
            close(expected_opacity(frame, case, x, y), color)
        except AssertionError as error:
            raise AssertionError(f"{case} at {frame} ({x},{y}): {error}") from error

for frame, color in [(60, 2), (120, 0)]:
    assert sum(new(frame, "karaoke", x, y)[color] > 200
               for x in range(250) for y in range(85)) > 100, "karaoke channel unavailable"

if ROUND3:
    curved = write("native-curved-text", [
        (340, 270, r"\fs56\1c$white\ctan5\ct(m -160 0 l 160 0)\t(0,1000,\ctx100)", "MMMM"),
        (760, 270, r"\fs56\1c$white\ctan5\ct(m 0 -160 l 0 160)", "MMMM"),
        (1290, 270, r"\fs44\fnMyanmar Text\1c$white\ctan5\ct(m -250 0 b -160 -130 160 -130 250 0)",
         "မြန်မာစာ သင်္ချာ ကြိုဆိုပါတယ်"),
        (340, 710, r"\fs56\1c$white\ct(m 0 0 l 0 0)", "MMMM"),
        (760, 710, r"\fs56\1c$white", "MMMM"),
    ])
    centers = {"line": (340,270), "upright": (760,270), "myanmar": (1290,270),
               "invalid": (340,710), "flat": (760,710)}
    playback("csri", CANDIDATE, curved, ROOT / "curved")
    playback("rgba", MANG, curved, ROOT / "curved-reference")
    actual = captures(ROOT / "curved", centers)
    reference = captures(ROOT / "curved-reference", centers)

    def ink_bounds(getter, frame, case, left=-220, right=220, top=-180, bottom=160):
        ink = [(x, y) for y in range(top, bottom) for x in range(left, right)
               if min(getter(frame, case, x, y)) >= 200]
        assert len(ink) >= 120, f"Missing shaped ink for {case} at frame {frame}"
        return min(x for x, _ in ink), min(y for _, y in ink), max(x for x, _ in ink), max(y for _, y in ink)

    for getter in (actual, reference):
        h0 = ink_bounds(getter, 60, "line", right=280)
        h1 = ink_bounds(getter, 90, "line", right=280)
        v = ink_bounds(getter, 60, "upright")
        assert h0[2] - h0[0] > 2 * (h0[3] - h0[1]), h0
        assert 46 <= h1[0] - h0[0] <= 54, (h0, h1)
        assert v[3] - v[1] > 2 * (v[2] - v[0]), v
        ink_bounds(getter, 60, "myanmar")
    assert ink_bounds(actual, 60, "invalid", left=0, top=0) == ink_bounds(actual, 60, "flat", left=0, top=0)
    expected = benchmark(CANDIDATE, curved, 1, "regression")["hashes"]
    for workers in (2, 4, 6, 8):
        assert benchmark(CANDIDATE, curved, workers, "regression")["hashes"] == expected

    vertical = write("native-vertical-text", [
        (220, 200, r"\fs48\1c$white\vert1\vtype2\vsp3", "minimum"),
        (520, 200, r"\fs48\1c$white\vert1\vtype3\fnMyanmar Text", "မြန်မာနိုင်ငံ"),
        (860, 200, r"\fs45\1c$white\vert1\vdir1\vcolsp30", r"日A\N日本"),
        (1260, 200, r"\fs45\1c$white\vert1\vdir2\vcolsp30", r"日A\N日本"),
        (220, 690, r"\fs48\1c$white\vert0", r"minimum{\vert1}"),
        (520, 690, rf"\fs48\vert1\vtype2\1c{RED}\2c{BLUE}\kf100", "MMMM"),
    ])
    centers = {"latin": (220, 200), "myanmar": (520, 200), "left": (860, 200),
               "right": (1260, 200), "disabled": (220, 690), "karaoke": (520, 690)}
    playback("csri", CANDIDATE, vertical, ROOT / "vertical")
    playback("rgba", MANG, vertical, ROOT / "vertical-reference")
    for getter in (captures(ROOT / "vertical", centers), captures(ROOT / "vertical-reference", centers)):
        latin = ink_bounds(getter, 60, "latin", top=-5, bottom=300)
        myanmar = ink_bounds(getter, 60, "myanmar", top=-5, bottom=330)
        flat = ink_bounds(getter, 60, "disabled", top=-5, right=350)
        assert latin[3] - latin[1] > 2 * (latin[2] - latin[0]), latin
        assert myanmar[3] - myanmar[1] > myanmar[2] - myanmar[0], myanmar
        assert flat[2] - flat[0] > 2 * (flat[3] - flat[1]), flat
        for case, sign in (("left", 1), ("right", -1)):
            bounds = ink_bounds(getter, 60, case, top=-5, bottom=210, right=170)
            midpoint = (bounds[0] + bounds[2]) // 2
            left = sum(min(getter(60, case, x, y)) > 200
                       for y in range(45, 130) for x in range(bounds[0], midpoint))
            right = sum(min(getter(60, case, x, y)) > 200
                        for y in range(45, 130) for x in range(midpoint, bounds[2] + 1))
            assert sign * (left - right) > 30, (case, left, right)
        red, blue = [], []
        for y in range(240):
            for x in range(-15, 100):
                r, _, b = getter(90, "karaoke", x, y)
                if r > 210 and b < 40:
                    red.append(y)
                if b > 210 and r < 40:
                    blue.append(y)
        assert red and blue and max(red) <= min(blue) + 3, (
            len(red), len(blue), max(red, default=None), min(blue, default=None))
    expected = benchmark(CANDIDATE, vertical, 1, "regression")["hashes"]
    for workers in (2, 4, 6, 8):
        assert benchmark(CANDIDATE, vertical, workers, "regression")["hashes"] == expected

    bars = ("m 0 0 l 200 0 200 4 0 4 "
            "m 0 55 l 200 55 200 65 0 65 m 0 116 l 200 116 200 120 0 120")
    pin = r"\mpersp(0,0,1,0,.75,1,.25,1)"
    perspective = write("projective-pins", [
        (200, 200, r"\1c$white\p1" + pin, bars),
        (580, 200, r"\1c$white\p1\mpersp(0,0,1,0,0,1,1,1)", bars),
        (960, 200, r"\1c$white\p1" + pin + r"\mpersp()", bars),
        (1340, 200, r"\1c$white\p1\t(0,1000," + pin + ")", bars),
        (200, 570, rf"\fs56\1c{WHITE}\2c{RED}\kf100\3grd(0,{BLUE},{GREEN})"
         rf"\bord4\2bs4\shad3\blur.7\fad(300,300,{RED},{BLUE}+a)" + pin, "ABCD"),
    ])
    centers = {"warped": (200, 200), "crossed": (580, 200), "reset": (960, 200),
               "animated": (1340, 200), "paint": (200, 570)}
    playback("csri", CANDIDATE, perspective, ROOT / "perspective")
    actual = captures(ROOT / "perspective", centers)
    assert min(actual(60, "warped", 100, 80)) > 220
    assert max(actual(60, "warped", 100, 60)) < 12, "Bilinear midpoint used as the perspective midpoint"
    for case in ("reset", "crossed"):
        assert min(actual(60, case, 100, 60)) > 220
        assert max(actual(60, case, 100, 80)) < 12
    assert min(actual(60, "animated", 100, 60)) > 220
    assert min(actual(90, "animated", 100, 69)) > 220
    assert min(actual(120, "animated", 100, 80)) > 220
    assert any(actual(60, "paint", x, y)[0] > 180 for y in range(90) for x in range(200))
    expected = benchmark(CANDIDATE, perspective, 1, "regression")["hashes"]
    for workers in (2, 4, 6, 8):
        assert benchmark(CANDIDATE, perspective, workers, "regression")["hashes"] == expected

    ruby = write("native-horizontal-ruby", [
        (270, 200, rf"\fs55\1c{WHITE}\furis65", "<漢字|かんじ|KANJI>"),
        (660, 200, rf"\fs55\1c{WHITE}\furisx60\furisy70\furistyle2\furipos(0,4)", "<漢字||KANJI>"),
        (1100, 200, rf"\an8\fs46\1c{WHITE}\fnMyanmar Text\furis70\furichangepos2",
         r"<satou|ဆာတို့|佐藤>-<san|စံ>\N<suzuki|စူဇူကီ|鈴木>-<kun|ကွန်း>"),
        (300, 610, rf"\fs52\1c{WHITE}\furiplaceauto1\furiap0\furifsp2", r"<明日|あした>\N<明日|あした>"),
        (690, 620, rf"\fs55\1c{WHITE}\furi0", "<漢字|かんじ|KANJI>"),
        (1260, 630, rf"\fs61\1c{WHITE}\2c{BLUE}\furichangepos1",
         r"<病|{\k30}や{\k26}ま{\k10}い> <掴|{\k40}つ{\k60}か>ん{\k70}だ"),
    ])
    centers = {"both": (270, 200), "reverse": (660, 200), "mixed": (1100, 200),
               "outward": (300, 610), "disabled": (690, 620), "timed": (1260, 630)}
    playback("csri", CANDIDATE, ruby, ROOT / "ruby")
    playback("rgba", MANG, ruby, ROOT / "ruby-reference")
    for getter in (captures(ROOT / "ruby", centers), captures(ROOT / "ruby-reference", centers)):
        both = ink_bounds(getter, 60, "both", left=-120, right=210, top=-140, bottom=185)
        reverse = ink_bounds(getter, 60, "reverse", left=-120, right=230, top=-100, bottom=185)
        disabled = ink_bounds(getter, 60, "disabled", left=-30, right=380, top=-90, bottom=190)
        assert both[1] < reverse[1] - 9 and reverse[3] > 50, (both, reverse)
        assert both[3] - both[1] > disabled[3] - disabled[1] + 20, (both, disabled)
        mixed = ink_bounds(getter, 60, "mixed", left=-310, right=310, top=-150, bottom=310)
        outward = ink_bounds(getter, 60, "outward", left=-120, right=230, top=-120, bottom=280)
        assert mixed[1] < 0 and mixed[3] > 60, mixed
        assert outward[1] < 0 and outward[3] > 100, outward
        assert any(getter(60, "timed", x, y) != getter(90, "timed", x, y)
                   for y in range(160) for x in range(260)), "Ruby's reading timing should affect visible paint"
    expected = benchmark(CANDIDATE, ruby, 1, "regression")["hashes"]
    for workers in (2, 4, 6, 8):
        assert benchmark(CANDIDATE, ruby, workers, "regression")["hashes"] == expected

# Check old corner math against the validated VSFilterMod path rather than
# requiring its different legacy bitmap geometry to agree with libass.
playback("csri", Path("threading-baseline/renderer/renderer.dll").resolve(), grid, ROOT / "previous")
previous = captures(ROOT / "previous")
for x in (25, 100, 175):
    for y in (25, 50, 75):
        assert new(90, "legacy", x, y) == previous(90, "legacy", x, y)

print("Analytic and Mangetsu pixel checks passed.", flush=True)
for scene in paint_scenes[1:]:
    expected_hashes = benchmark(CANDIDATE, scene, 1, "regression")["hashes"]
    for n in (2, 4, 6, 8):
        hashes = benchmark(CANDIDATE, scene, n, "regression")["hashes"]
        assert hashes == expected_hashes, f"{scene[0].stem} seek/frame hashes differ at {n} workers"

print("Backward seeks and five worker budgets produced identical hashes.", flush=True)
results = []
settings = [(scene, n) for n in (1, 2, 4) for scene in paint_scenes]
for trial in range(3):
    for scene, n in settings if trial % 2 == 0 else reversed(settings):
        results.append(dict(benchmark(CANDIDATE, scene, n, "animation"),
                            scene=scene[0].stem, workers=n, trial=trial + 1))
(ROOT / "paint-benchmarks.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
print("Paint pixels, Mangetsu reference samples, backward seeks and five worker budgets passed.", flush=True)
