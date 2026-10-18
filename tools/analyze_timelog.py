"""Summarize the per-capture CSV written by `hdrrec -timelog file.csv`.

usage: python tools/analyze_timelog.py tests/times.csv
needs: numpy

columns: present_us, acquire_latency_ms, accumulated, slot_float, slot, action, phase[, ring, copy_ms, release_ms]
  accumulated > 1  -> desktop frames were lost by the capture loop (first row = startup backlog, ignored)
  action           -> new / early (placed one slot later) / drop (slot already used) /
                      fill (repeat fed while the source was frozen; not a capture)
  ring             -> shared buffer used (-1 = none free); copy_ms = keyed mutex + CopyResource,
                      release_ms = texture release + ReleaseFrame (CPU time in the capture loop)
"""
import collections, csv, sys
import numpy as np


def main(path):
    rows = list(csv.DictReader(open(path, newline="")))
    fills = [r for r in rows if r["action"] == "fill"]
    rows = [r for r in rows if r["action"] != "fill"]   # repeats fed during a frozen source
    t = np.array([int(r["present_us"]) for r in rows])
    lat = np.array([float(r["acquire_latency_ms"]) for r in rows])
    acc = np.array([int(r["accumulated"]) for r in rows])
    slots = np.array([int(r["slot"]) for r in rows])
    acts = collections.Counter(r["action"] for r in rows)
    used = slots[[r["action"] != "drop" for r in rows]]
    gaps = np.diff(used) - 1
    dt = np.diff(t) / 1000.0
    print(f"captures          : {len(rows)}  {dict(acts)}")
    print(f"missed (acc>1)    : {int((acc[1:] - 1).clip(min=0).sum())}")
    print(f"repeat fills      : {int(gaps[gaps > 0].sum()) - len(fills)} at the next capture, "
          f"{len(fills)} fed during freezes")
    print(f"present interval  : p0.5={np.percentile(dt, 0.5):.2f} median={np.median(dt):.2f} "
          f"p99.5={np.percentile(dt, 99.5):.2f} max={dt.max():.2f} ms")
    print(f"intervals > 25 ms : {int((dt > 25).sum())}  (source / game hitches when acc == 1)")
    print(f"acquire latency   : median={np.median(lat):.2f} p99={np.percentile(lat, 99):.2f} max={lat.max():.2f} ms")
    # video vs real time: the audio is placed on real time, so this is the A/V offset of each frame
    placed = [r for r in rows if r["action"] != "drop"]
    dev = np.array([int(r["present_us"]) / 1e6 - int(r["slot"]) / 60.0 for r in placed]) * 1000
    ph = np.array([float(r["phase"]) for r in rows])
    print(f"video - real time : max |offset| {np.abs(dev).max():.1f} ms (at {int(placed[int(np.abs(dev).argmax())]['present_us']) / 1e6:.1f} s), "
          f"phase {ph.min():+.2f}..{ph.max():+.2f} slots (assumes 60 fps)")
    timed = [r for r in rows if r.get("copy_ms")]
    if timed:
        cp = np.array([float(r["copy_ms"]) for r in timed])
        rl = np.array([float(r["release_ms"]) for r in timed])
        print(f"copy (CPU)        : median={np.median(cp):.2f} p99={np.percentile(cp, 99):.2f} max={cp.max():.2f} ms")
        print(f"release (CPU)     : median={np.median(rl):.2f} p99={np.percentile(rl, 99):.2f} max={rl.max():.2f} ms")
        print(f"ring usage        : {dict(sorted(collections.Counter(int(r['ring']) for r in timed).items()))}")
    for i in np.where(acc[1:] > 1)[0] + 1:
        print(f"  lost {acc[i] - 1} frame(s) before t={t[i] / 1e6:.3f}s (latency {lat[i]:.2f} ms)")


if __name__ == "__main__":
    main(sys.argv[1])
