# hdrrec

**4K / 60 fps / HDR10 game recorder for Windows with frame-accurate pacing.**

[日本語版 README](README.ja.md)

hdrrec records a borderless-fullscreen game at 3840x2160, 60 fps constant frame rate, HDR10
(BT.2020 PQ, 10-bit) with NVIDIA NVENC (HEVC or AV1), together with system audio or the audio of a
single application.

It was written to get rid of the **interval stutter** — periodic duplicated / skipped frames in
otherwise smooth 60 fps recordings — that OBS and NVIDIA App recordings can show
([OBS forum thread](https://obsproject.com/forum/threads/interval-stutter-jitter-duplicated-frames-bug-workarounds.179196/)).
In 20-second test recordings of a game running at a steady 60 fps, hdrrec writes 1200 of 1200
frames as unique game frames, with no repeats and no skips.

## How it avoids interval stutter

Typical recorders sample the screen on their own fixed timer. When that timer and the game's
presents drift against each other, a frame is periodically captured twice and the next one is
lost. hdrrec does not sample on a timer:

- **Desktop Duplication** delivers every desktop update with its `LastPresentTime`
  (the moment the game presented the frame).
- Each frame is placed into the 60 fps output timeline by that **present timestamp**, with a
  small phase-locked loop that keeps presents centred in their output slots.
- Only slots in which the game really produced no new frame are filled with a repeat, and the
  recorder reports why (`src` = the game did not present, `miss` = the capture was too late).

Other details:

- Capture as FP16 scRGB, conversion to BT.2020 PQ 10-bit with a GPU shader (no clipping of
  highlights above SDR white), HDR10 mastering-display / content-light-level metadata in the
  bitstream and in the MKV container.
- Separate D3D11 devices for capture and encoding, so NVENC never blocks the capture loop.
- Audio placed on the same clock (QPC timestamps), stays in sync over long recordings
  (38 minutes: audio and video length differ by 0.01 s).

## Requirements

- Windows 11 (tested). Recent Windows 10 versions may work but are untested.
- NVIDIA GPU with NVENC, driver 570 or newer. AV1 needs a GeForce RTX 40 series or newer.
- Windows HDR turned on for the recorded monitor
- The game in borderless fullscreen or windowed mode

## Usage (GUI)

Download the release zip, extract it, and run `hdrrec_gui.exe` (`hdrrec.exe` must stay in the
same folder).

- **Alt+F9** (changeable) starts / stops a recording, as do the window button and the
  notification-area (tray) menu. The tray icon is gray while idle, red while recording.
- Minimize hides the window to the tray; the close button exits (a running recording is stopped
  and finalized first). `hdrrec_gui.exe /tray` starts hidden.
- Settings: hotkey, output folder, monitor, encoder (HEVC / AV1), preset (p1–p4), quality
  (CQ or bitrate), audio source (whole system / none / one application) with gain,
  GPU priority. They are stored in `hdrrec_gui.ini` next to the executable.
- Every recording also writes `logs\<name>.log` and `logs\<name>_times.csv`
  (per-frame timing; see *Checking a recording*).

> The window is captured too if it is shown on the recorded monitor (Desktop Duplication records
> the composed desktop). Minimize it to the tray or move it to another monitor.

## Usage (command line)

```
hdrrec.exe [-o out.mkv] [-t sec] [-monitor N] [-fps 60] [-codec hevc|av1]
           [-cq 18 | -bitrate Mbps] [-preset p4] [-maxrate Mbps]
           [-maxcll 1000] [-maxfall 400] [-mdl 1000]
           [-audio 0|1] [-acodec aac|pcm] [-abitrate 256] [-again dB]
           [-audio-process NAME.exe|PID] [-gpuprio 0..3]
           [-timelog times.csv] [-list-monitors]
```

| Option | Meaning |
|---|---|
| `-o` | Output file (default `rec_YYYYmmdd_HHMMSS.mkv`) |
| `-t` | Stop after N seconds (default: until Enter, Ctrl+C or end of stdin) |
| `-monitor` | Monitor index, see `-list-monitors` |
| `-codec` | `hevc` (Main10) or `av1` |
| `-cq` / `-bitrate` | Constant quality (HEVC 1–51, AV1 1–63, lower = better) or average bitrate in Mbit/s |
| `-preset` | NVENC preset `p1`–`p7`. **p5 and above cannot sustain 4K60 while a game loads the GPU** |
| `-mdl`, `-maxcll`, `-maxfall` | HDR10 static metadata in nits (defaults suit a 1000-nit display) |
| `-audio-process` | Record only this application's audio (and its child processes) |
| `-again` | Audio gain in dB (clipped at full scale) |
| `-gpuprio` | GPU priority of the recorder: 0 unchanged (default), 1 above normal, 2 high, 3 realtime |
| `-timelog` | Write per-frame present time / latency / slot decisions as CSV |

The summary printed at the end shows `unique captures`, `repeated frames (source did not update /
capture missed)`, `missed desktop frames` and `encoder queue overflow`.

## Checking a recording

```
python tools/analyze_timelog.py logs/rec_..._times.csv   # present intervals, where frames were lost
python tools/analyze_frames.py  rec_....mkv               # decode and count repeated frames (needs ffmpeg, numpy)
```

`scripts/test_record.bat [options]` records 20 seconds after a 5-second countdown into `tests/`
and prints the summary.

## Building

Use the MSYS2 **MINGW64** environment (not UCRT64: the bundled FFmpeg libraries use msvcrt).

```
pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-pkgconf tar
bash scripts/build_hdrrec.sh          # -> build/hdrrec.exe, build/hdrrec_gui.exe
```

The build uses the prebuilt minimal static FFmpeg in `third_party/`. To rebuild FFmpeg itself
(other version or components), see `scripts/build_ffmpeg.sh`. `scripts/package_release.sh VERSION`
creates the release zip and fetches the matching FFmpeg source archive.

## Limitations

- NVIDIA GPUs only (NVENC).
- The mouse cursor is not recorded.
- Changing the resolution or the HDR mode during a recording is not supported (the recording
  stops, or the picture freezes while HDR is off).
- If the default audio device changes during a recording, system audio continues as silence
  (per-application audio is not affected).
- In very GPU-heavy scenes, where the game itself drops below 60 fps, a few frames can still be
  missed (about 0.07–0.08 % in long test recordings). `-gpuprio` can help; test it per game.

## License

hdrrec is released under the [zlib license](LICENSE).

The executables statically link FFmpeg, which is licensed under the LGPL 2.1 or later, and other
third-party components. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and
[LICENSES/](LICENSES/).
