"""Re-sync the audio of a recording made with hdrrec 1.0.0, whose video can drift ahead of the
audio in long recordings (fixed in later versions). The video stream is copied unchanged; only the
audio is re-timed and re-encoded.

usage: python tools/fix_av_drift.py in.mkv logs/<name>_times.csv out.mkv [--abitrate 256k]
needs: numpy, ffmpeg / ffprobe in PATH

The timelog (-timelog / logs\\<name>_times.csv of the same recording) gives, for every video frame,
the real time at which that image was presented. The audio was placed on real time, so the output
audio at video time v takes the input audio at v + offset(v), offset = present time - video time
(smoothed over ~2 s). The offset changes slowly, so the time-varying shift is inaudible.
"""
import csv, json, subprocess, sys
import numpy as np


def offsets(csv_path):
    rows = [r for r in csv.DictReader(open(csv_path, newline=""))
            if r["action"] in ("new", "early")]   # frames placed in the video (not drop / fill)
    v = np.array([int(r["slot"]) / 60.0 for r in rows])
    off = np.array([int(r["present_us"]) / 1e6 for r in rows]) - v
    w = 121                                       # ~2 s rolling median: keep the drift, drop the jitter
    pad = np.pad(off, (w // 2, w // 2), mode="edge")
    smooth = np.median(np.lib.stride_tricks.sliding_window_view(pad, w), axis=1)
    return v, smooth


def main(src, csv_path, dst, abitrate="256k"):
    probe = json.loads(subprocess.check_output(
        ["ffprobe", "-v", "error", "-select_streams", "a:0", "-show_entries", "stream=sample_rate,channels",
         "-show_entries", "format=duration", "-of", "json", src]))
    rate = int(probe["streams"][0]["sample_rate"])
    ch = int(probe["streams"][0]["channels"])
    total = int(round(float(probe["format"]["duration"]) * rate))
    v, off = offsets(csv_path)
    print(f"audio {rate} Hz {ch} ch, {total / rate:.1f} s; offset 0 -> max {off.max() * 1000:.0f} ms "
          f"(at {v[off.argmax()]:.0f} s), end {off[-1] * 1000:.0f} ms")

    reader = subprocess.Popen(["ffmpeg", "-v", "error", "-i", src, "-map", "0:a:0", "-f", "f32le", "-ac", str(ch),
                               "-ar", str(rate), "-"], stdout=subprocess.PIPE)
    writer = subprocess.Popen(["ffmpeg", "-v", "error", "-y", "-i", src,
                               "-f", "f32le", "-ar", str(rate), "-ac", str(ch), "-i", "-",
                               "-map", "0:v:0", "-map", "1:a:0", "-c:v", "copy", "-c:a", "aac", "-b:a", abitrate,
                               dst], stdin=subprocess.PIPE)

    buf = np.zeros((0, ch), np.float32)   # input samples [buf_start, buf_start + len(buf))
    buf_start, eof = 0, False
    chunk = rate * 5
    for n0 in range(0, total, chunk):
        n = np.arange(n0, min(n0 + chunk, total))
        s = (n / rate + np.interp(n / rate, v, off)) * rate   # source position of each output sample
        i0 = np.floor(s).astype(np.int64)
        frac = (s - i0).astype(np.float32)[:, None]
        need = int(i0.max()) + 2
        while not eof and buf_start + len(buf) < need:
            data = reader.stdout.read(rate * ch * 4)
            if not data:
                eof = True
                break
            buf = np.concatenate([buf, np.frombuffer(data, "<f4").reshape(-1, ch)])
        def at(idx):
            k = idx - buf_start
            ok = (k >= 0) & (k < len(buf))
            out = np.zeros((len(idx), ch), np.float32)
            out[ok] = buf[k[ok]]
            return out
        out = at(i0) * (1 - frac) + at(i0 + 1) * frac
        writer.stdin.write(out.astype("<f4").tobytes())
        keep = int(i0.min()) - rate               # drop input that is no longer needed (1 s margin)
        if keep > buf_start:
            buf = buf[keep - buf_start:]
            buf_start = keep
        print(f"\r{n[-1] / rate:7.1f} / {total / rate:.1f} s", end="", flush=True)
    writer.stdin.close()
    reader.stdout.close()
    reader.wait()
    print("\ndone" if writer.wait() == 0 else "\nffmpeg failed")


if __name__ == "__main__":
    args = sys.argv[1:]
    br = "256k"
    if "--abitrate" in args:
        k = args.index("--abitrate")
        br = args[k + 1]
        del args[k:k + 2]
    if len(args) != 3:
        sys.exit(__doc__)
    main(*args, abitrate=br)
