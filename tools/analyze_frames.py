"""Count repeated (duplicate) and backward-jumping frames in a recording.

usage: python tools/analyze_frames.py tests/rec.mkv
needs: ffmpeg on PATH (any recent build), numpy

Each frame is decoded, downscaled to 480x270 luma, and compared with the previous one.
A frame whose mean absolute difference is < 5% of the median difference counts as a repeat.
"Backward" = a frame nearly identical to the frame two before while both neighbours differ
(A B A pattern; would indicate out-of-order frames, expected 0 - blinking UI can also trigger it).
"""
import subprocess, sys
import numpy as np

W, H = 480, 270


def decode(path):
    vf = ["-vf", f"scale={W}:{H}:flags=area,format=gray16le", "-f", "rawvideo", "-"]
    for sync in (["-fps_mode", "passthrough"], ["-vsync", "passthrough"]):  # new / old ffmpeg
        p = subprocess.run(["ffmpeg", "-v", "error", "-i", path] + sync + vf, capture_output=True)
        if p.returncode == 0:
            return p.stdout
    raise RuntimeError(p.stderr.decode(errors="replace"))


def main(path):
    raw = decode(path)
    f = np.frombuffer(raw, dtype="<u2").reshape(-1, H, W).astype(np.float32)
    d = np.abs(np.diff(f, axis=0)).mean(axis=(1, 2))
    med = float(np.median(d))
    dup = np.where(d < 0.05 * med)[0] + 1
    back = 0
    for k in range(2, len(f)):
        a, c = d[k - 1], d[k - 2]
        b = np.abs(f[k] - f[k - 2]).mean()
        if c > 0.5 * med and a > 0.5 * med and b < 0.1 * min(a, c):
            back += 1
    print(f"frames            : {len(f)}")
    print(f"repeated frames   : {len(dup)}  ({100.0 * len(dup) / max(1, len(f)):.1f}%)")
    print(f"backward frames   : {back}")
    if len(dup):
        print(f"first repeats at  : {dup[:30].tolist()}")


if __name__ == "__main__":
    main(sys.argv[1])
