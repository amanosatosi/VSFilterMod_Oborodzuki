"""Time the same CSRI frames back-to-back and compare the entire BGRX output."""
import argparse
import ctypes as c
from contextlib import ExitStack
import hashlib
import json
import math
import os
from pathlib import Path
import runpy
import time

csri = runpy.run_path(str(Path(__file__).with_name("coverage-check.py")))
Format, Frame, BGRX = (csri[name] for name in ("Format", "Frame", "BGRX"))


def open_renderer(stack, path, source, w, h):
    dll = c.CDLL(str(path.resolve()))

    def api(name, result, *args):
        fn = getattr(dll, "csri_" + name)
        fn.restype, fn.argtypes = result, args
        return fn

    renderer = api("renderer_default", c.c_void_p)()
    instance = api("open_mem", c.c_void_p, c.c_void_p, c.c_void_p, c.c_size_t, c.c_void_p)(
        renderer, source, len(source), None)
    if not instance:
        raise RuntimeError(f"Could not load subtitles in {path}")
    stack.callback(api("close", None, c.c_void_p), instance)
    if api("request_fmt", c.c_int, c.c_void_p, c.POINTER(Format))(instance, c.byref(Format(BGRX, w, h))):
        raise RuntimeError(f"Could not set BGRX output for {path}")
    draw = api("render", None, c.c_void_p, c.POINTER(Frame), c.c_double)
    pixels = c.create_string_buffer(w * h * 4)
    frame = Frame(BGRX)
    frame.planes[0], frame.strides[0] = c.addressof(pixels), 4 * w
    frame_ptr = c.pointer(frame)

    def measure(ms):
        c.memset(pixels, 0, len(pixels))
        seconds = math.nextafter(ms / 1000, math.inf)
        start = time.perf_counter_ns()
        draw(instance, frame_ptr, seconds)
        return time.perf_counter_ns() - start

    return measure, pixels


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("fixture", type=Path)
    parser.add_argument("--workers", required=True, type=int, choices=range(1, 9))
    parser.add_argument("--first-ms", type=int, default=1000)
    parser.add_argument("--last-ms", type=int, default=4999)
    parser.add_argument("--frames", type=int, default=60)
    parser.add_argument("--trials", type=int, default=3)
    args = parser.parse_args()
    if args.frames < 2 or args.trials < 1:
        parser.error("Use at least two frames and one trial")
    os.environ["OBORODZUKI_WORKERS"] = str(args.workers)
    memcmp = c.CDLL("ucrtbase").memcmp
    memcmp.argtypes, memcmp.restype = (c.c_void_p, c.c_void_p, c.c_size_t), c.c_int
    source = args.fixture.read_bytes()
    schedule = [args.first_ms + (args.last_ms - args.first_ms) * i // (args.frames - 1)
                for i in range(args.frames)]
    trials = []
    for trial in range(args.trials):
        with ExitStack() as stack:
            paths = [args.baseline, args.candidate]
            order = [trial % 2, 1 - trial % 2]
            renderers = {i: open_renderer(stack, paths[i], source, 1920, 1080) for i in order}
            for warm in range(6):
                for i in order:
                    renderers[i][0](schedule[warm * (args.frames - 1) // 5])
            samples = [[], []]
            rendered = False
            blank = c.create_string_buffer(1920 * 1080 * 4)
            for index, ms in enumerate(schedule):
                first = (index + trial) % 2
                for i in (first, 1 - first):
                    samples[i].append(renderers[i][0](ms))
                a, b = renderers[0][1], renderers[1][1]
                if memcmp(a, b, len(a)):
                    raise RuntimeError(f"Pixel difference with {args.workers} workers at {ms} ms")
                if not rendered:
                    rendered = bool(memcmp(a, blank, len(a)))
            if not rendered:
                raise RuntimeError("The entire schedule rendered a blank frame")
            trials.append(dict(baseline_ns=samples[0], candidate_ns=samples[1]))
    print(json.dumps(dict(workers=args.workers, fixture_sha256=hashlib.sha256(source).hexdigest(),
                          baseline_sha256=hashlib.sha256(args.baseline.read_bytes()).hexdigest(),
                          candidate_sha256=hashlib.sha256(args.candidate.read_bytes()).hexdigest(),
                          resolution=[1920, 1080], times_ms=schedule, trials=trials)))


if __name__ == "__main__":
    main()
