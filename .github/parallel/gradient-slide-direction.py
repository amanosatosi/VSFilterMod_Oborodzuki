"""Focused CSRI pixel tests for the independent slide direction."""
import ctypes as c
import math
from pathlib import Path
import runpy
import sys


helper = runpy.run_path(".github/parallel/coverage-check.py")
Format, Frame, BGRX = (helper[name] for name in ("Format", "Frame", "BGRX"))
W, H = 640, 360
HEADER = (Path(".github/ci/shared.ass").read_text(encoding="utf-8")
          .split("Dialogue:", 1)[0].replace("1920", "640").replace("1080", "360"))
DRAW = r"\an7\pos(100,100)\bord0\shad0\p1"
RECT = "m 0 0 l 200 0 200 100 0 100"
RED = "(0,&H0000FF&,&H0000FF&)"
BLUE = "(0,&HFF0000&,&HFF0000&)"
DIAGONAL = "(45,&HFF0000&,&HFFFFFF&)"
TIMES = [0, 500, 1000, 720, 500, 250]


def script(tags, text=RECT, metadata=""):
    return (HEADER + metadata + "Dialogue: 0,0:00:00.00,0:00:05.00,Default,Nene,0,0,0,,{"
            + tags + "}" + text + "\n").encode("utf-8-sig")


def fill(setting="", target=BLUE, width=r"\grds0"):
    return script(DRAW + r"\1grd" + RED + width + setting + rf"\t(0,1000,\1grd{target})")


class CSRI:
    def __init__(self, path):
        dll = c.CDLL(str(path.resolve()))

        def api(name, returns, *args):
            fn = getattr(dll, f"csri_{name}")
            fn.restype, fn.argtypes = returns, args
            return fn

        self.renderer = api("renderer_default", c.c_void_p)()
        self.open = api("open_mem", c.c_void_p, c.c_void_p, c.c_char_p, c.c_size_t, c.c_void_p)
        self.format = api("request_fmt", c.c_int, c.c_void_p, c.POINTER(Format))
        self.draw = api("render", None, c.c_void_p, c.POINTER(Frame), c.c_double)
        self.close = api("close", None, c.c_void_p)

    def frames(self, data, times=TIMES):
        instance = self.open(self.renderer, data, len(data), None)
        assert instance, "CSRI rejected the slide fixture"
        try:
            assert self.format(instance, c.byref(Format(BGRX, W, H))) == 0
            pixels = c.create_string_buffer(W * H * 4)
            frame = Frame(BGRX)
            frame.planes[0], frame.strides[0] = c.addressof(pixels), W * 4
            results = []
            for ms in times:
                c.memset(pixels, 0, len(pixels))
                self.draw(instance, c.byref(frame), math.nextafter(ms / 1000, math.inf))
                results.append(pixels.raw)
            return results
        finally:
            self.close(instance)


def rgb(frame, x, y):
    i = (y * W + x) * 4
    return frame[i + 2], frame[i + 1], frame[i]


def main(path):
    render = CSRI(path).frames
    red, blue = (255, 0, 0), (0, 0, 255)
    automatic = render(fill())
    for direction, front, back in [
        (0, (140, 140), (260, 140)), (180, (260, 140), (140, 140)),
        (90, (180, 120), (180, 180)), (270, (180, 180), (180, 120)),
    ]:
        start, half, end, _, seek, _ = render(fill(rf"\grdsd{direction}"))
        assert (rgb(half, *front), rgb(half, *back)) == (blue, red), direction
        assert rgb(start, *front) == red and rgb(end, *back) == blue, direction
        assert half == seek, direction
        if direction == 0:
            assert half == automatic[1], "explicit right should match an automatic right wipe"

    diagonal = render(fill(target=DIAGONAL))
    horizontal = render(fill(r"\grdsd0", target=DIAGONAL))
    assert diagonal[1] != horizontal[1] and diagonal[2] == horizontal[2]
    assert rgb(horizontal[1], 150, 125) != rgb(horizontal[1], 150, 175), "endpoint rotated"
    assert horizontal == render(fill(r"\grdsd-360", target=DIAGONAL))
    assert render(fill(r"\grdsd-45")) == render(fill(r"\grdsd315"))
    assert horizontal == render(fill(r"\grdsd0\grdsd90x\grdsdnan\grdsd()\grdsd(180)", target=DIAGONAL))
    assert diagonal == render(fill(r"\grdsd0\grdsd", target=DIAGONAL))
    assert render(fill(r"\grdsd180", width="")) == render(fill(width=""))
    assert render(fill(r"\grdsd180", width=r"\grds20"))[1] != render(fill(r"\grdsd180"))[1]

    # A later wipe inherits automatic direction again. Changing the outer
    # direction after the transforms cannot rotate either constructed wipe.
    first = DRAW + r"\grds0\1grd" + RED + rf"\t(0,1000,\grdsd180\1grd{BLUE})"
    second = rf"\t(250,1000,\1grd{DIAGONAL})"
    assert render(script(first + second)) == render(script(first + r"\grdsd" + second + r"\grdsd90"))
    assert render(script(first + second))[1] != render(script(first + r"\grdsd90" + second))[1]

    for before, after, body in (
        (DRAW + r"\1c&HFFFFFF&\1gra(0,&H00&,&H00&)", r"\1gra(0,&HC0&,&HC0&)", RECT),
        (DRAW + r"\1vc(&H0000FF&,&H0000FF&,&H0000FF&,&H0000FF&)",
         r"\1vc(&HFF0000&,&HFF0000&,&HFF0000&,&HFF0000&)", RECT),
        (DRAW + r"\1c&HFFFFFF&\pgrd(100,100,300,200,0,&H0000FF&,&H0000FF&)",
         r"\pgrd(100,100,300,200,0,&HFF0000&,&HFF0000&)", RECT),
        (DRAW + r"\1c&HFFFFFF&\2bs10\2bgrd" + RED, r"\2bgrd" + BLUE, RECT),
        (r"\an5\pos(320,180)\fs64\bs4\boxp16\2bbs8\2bbgrd" + RED, r"\2bbgrd" + BLUE, "MMMM"),
    ):
        def wipe(direction):
            return render(script(before + rf"\grds0\grdsd{direction}\t(0,1000,{after})", body))
        right, left = wipe(0), wipe(180)
        assert right[0] == left[0] and right[2] == left[2] and right[1] != left[1], after

    style = "Dialogue: 0,0:00:00.00,0:00:10.00,Default,,0,0,0,OBRS,{\\grdsd180}\n"
    actor = "Dialogue: 0,0:00:00.00,0:00:10.00,Default,Nene,0,0,0,OBRCC,{\\grdsd270}\n"
    paint = DRAW + r"\grds0\1grd" + RED + rf"\t(0,1000,\1grd{BLUE})"
    for metadata, expected in ((style, 180), (style + actor, 270)):
        inherited = render(script(r"\grdsd0\grdsd" + paint, metadata=metadata))
        reset = render(script(r"\grdsd0\r" + paint, metadata=metadata))
        assert inherited == reset == render(fill(rf"\grdsd{expected}")), metadata
    print("CSRI slide direction, resets, paint targets and seeking passed.")


if __name__ == "__main__":
    main(Path(sys.argv[1]))
