"""Differential CSRI checks for outline reuse, phase changes and transform plateaus."""
import ctypes as c
import hashlib
import math
import os
from pathlib import Path
import sys

BGRX = 0x102


class Format(c.Structure):
    _fields_ = [("pixfmt", c.c_int), ("width", c.c_uint), ("height", c.c_uint)]


class Frame(c.Structure):
    _fields_ = [("pixfmt", c.c_int), ("planes", c.c_void_p * 4), ("strides", c.c_ssize_t * 4)]


def render(path, source, workers):
    os.environ["OBORODZUKI_WORKERS"] = str(workers)
    dll = c.CDLL(str(path.resolve()))
    def api(name, result, *args):
        fn = getattr(dll, "csri_" + name)
        fn.restype, fn.argtypes = result, args
        return fn
    renderer = api("renderer_default", c.c_void_p)()
    instance = api("open_mem", c.c_void_p, c.c_void_p, c.c_void_p, c.c_size_t, c.c_void_p)(
        renderer, source, len(source), None)
    if not instance:
        raise RuntimeError("CSRI rejected the coverage fixture")
    fmt = api("request_fmt", c.c_int, c.c_void_p, c.POINTER(Format))
    draw = api("render", None, c.c_void_p, c.POINTER(Frame), c.c_double)
    close = api("close", None, c.c_void_p)
    results = []
    try:
        for w, h in [(640, 360), (923, 519), (640, 360)]:
            if fmt(instance, c.byref(Format(BGRX, w, h))):
                raise RuntimeError("CSRI rejected the test dimensions")
            stride = 4 * w + 12
            pixelbuf = c.create_string_buffer(stride * h)
            times = [0, 1000, 1099, 1100, 1150, 1199, 1200, 1201, 1450, 1499,
                     1500, 1537, 1620, 1999, 2500, 1150, 1201, 1100, 1000, 2500]
            for background in [bytes((0, 0, 0, 255)), bytes((255, 255, 255, 255))]:
                blank = (background * w + bytes([31]) * 12) * h
                for ms in times:
                    c.memmove(pixelbuf, blank, len(blank))
                    frame = Frame(BGRX)
                    frame.planes[0], frame.strides[0] = c.addressof(pixelbuf), stride
                    draw(instance, c.byref(frame), math.nextafter(ms / 1000, math.inf))
                    results.append(hashlib.sha256(pixelbuf.raw).digest())
    finally:
        close(instance)
    return results


if __name__ == "__main__":
    base, candidate = map(Path, sys.argv[1:])
    header = Path(".github/ci/shared.ass").read_text(encoding="utf-8").split("Dialogue:")[0]
    lines = []
    for i in range(12):
        # Several paths share the same sampled endpoints and different interior
        # segments. Paint and border settings vary while positions overlap.
        tags = (rf"\an5\pos({510 + 80 * (i % 4)},{270 + 120 * (i // 4)})"
                r"\t(100,200,\fscx147\frz-23)\t(500,620,\frx24)"
                rf"\1vc(&H01020{i % 9}&,&H011122&,&H77EE23&,&HCC9922&)"
                r"\3va(&H10&,&H30&,&H50&,&H70&)")
        text = "日本語 မြန်မာ"
        if i % 3:
            tags += rf"\xbord{2 + i % 4}\ybord{6 - i % 4}\blur{1 + i % 3}\p1"
            text = f"m 0 0 l 100 0 100 30 50 {20 + i} 0 30 m 16 8 l 16 20 32 20 32 8"
        lines.append(f"Dialogue: {i % 2},0:00:01.00,0:00:03.00,Default,actor{i},0,0,0,,{{{tags}}}{text}\n")
    source = (header + "".join(lines)).encode("utf-8-sig")
    expected = render(base, source, 1)
    if len(set(expected[:20])) < 5:
        raise RuntimeError("Expected changing coverage throughout the animation")
    for workers in (1, 2, 4, 6, 8):
        actual = render(candidate, source, workers)
        if actual != expected:
            index = next(i for i, pair in enumerate(zip(expected, actual)) if pair[0] != pair[1])
            raise RuntimeError(f"Coverage mismatch at {workers} workers, sample {index}")
    print(f"{len(expected) * 5} full-buffer hashes matched on black and white backgrounds")
