# Recording frame rate — 2026-10-31

Receipt transcribed from [merged code PR #1040](https://github.com/patrick-hudson/put.gd-desktop/pull/1040),
merge `3d7d3334651e8d560a4719cd8e11d5336dea90c6`. These are the PR's
bounded measurements, not a new campaign or current release acceptance.
The development-tree rows do not identify an immutable build; retain that
limitation when comparing them with the named nightly and AppImage.

## Settings picture

![Settings → Capture → Screen recording → Frame rate, annotated](https://github.com/patrick-hudson/put.gd-desktop/blob/fd139d3349b0dddb6a24bb889b2f0b1a55ceba78/docs/evidence/2026-10-31-recording-rate/settings-frame-rate-annotated.png?raw=true)

Copied unchanged from `evidence/recording-rate-984`, commit
`fd139d3349b0dddb6a24bb889b2f0b1a55ceba78` (Git blob
`279a95f48b65903d863827d1d3dede399f84b658`). `putgd-ui-catalog` rendered
`settings-recording-frame-rate`, dark, on CI run `41212607076` at code head
`50635249`. This synthetic Settings render proves the control's appearance;
it is not graphical desktop acceptance.

## Reported measurements

**KDE (aurora-amd, Plasma 6.7.5 Wayland, Radeon 740M, kde-screencast, `vah264enc`, 1080p, 10 s):**

| Build | fps | distinct/s | fluid | desktop | machine busy | recorder | lowered |
|---|---|---|---|---|---|---|---|
| nightly 26.1031.3384 | 60 | 59.72 / 59.81 | 0.992 / 0.994 | 59.8 / 59.9 fps | – | 0.53–0.54 cores | – |
| branch (dev tree, toolbox) | 60 | 59.72 / 59.81 | 0.993 / 0.994 | 59.8 / 59.9 fps | 16–24% | 0.54–0.58 cores | no |
| nightly / branch | 30 | 30.0 / 30.0 | – | 59.9 / 59.9 fps | 20% | 0.28 / 0.29 cores | no |

**m74 (Mint 22.3 Cinnamon X11, i5 4 cores, HD 530), 1080p60, 20 s.** m74 alone is not starved: nightly 60 fps holds 57–58 desktop fps with `vah264lpenc` and with x264. Two busy-loop hogs leave it about 2 cores; the fixture alone with them still runs at 59.6 fps. Desktop pace is shown before and after the step (or the same windows without one). CI AppImage `26.1031.3422-dev+gae547a435984` (`c57ef10d…`).

| Run | Rate lowered | Desktop, 0.5–5.4 s | Desktop, 6.5–19.5 s | Recorder | Distinct/s |
|---|---|---|---|---|---|
| x264, adaptive **on**, r1 / r2 | 60→30 (cpu) at 5.2 / 5.3 s | 47.5 / 53.2 fps | 49.6 / 55.5 fps | 0.93 / 0.96 cores | 32.7 / 35.6 |
| x264, adaptive **off**, r1 / r2 | – | 48.9 / 54.4 | 51.5 / 54.6 | 1.13 cores | 50.5 / 53.9 |
| x264, nightly | – | 49.3 | 49.3 | 1.22 cores | 48.4 |
| `vah264lpenc`, adaptive **on** | 60→30 (cpu) at 11.4 s | 54.9 | **57.7** | 0.49 cores | 43.2 |
| `vah264lpenc`, adaptive **off** | – | 54.9 | 54.7 | 0.61 cores | 49.9 |
| x264 at 30 fps, adaptive on | none (cpu stops at 30) | 57.1 | 57.1 | 0.69 cores | 30.0 |
| `vah264lpenc` 60, no hogs, adaptive on | none (machine 57% busy) | 58.6 | 59.2 | 0.77 cores | 58.9 |

What these runs show:

- With VA, the back-off gives the desktop back about 3 fps and saves 20% of the recorder's CPU.
- With software x264, the gain is **within run-to-run noise**: the gate halves conversion, but x264 still encodes 60 frames a second, half of them repeats. #1045 tracks encoding at the lowered rate.
- No back-off fired in the measured healthy-machine runs: aurora at 16–24% busy, m74 without hogs at 57%.
- Choosing 30 fps (the default) gave 57.1 fps desktop on 0.69 cores in this m74 workload.
- f44 (the issue's GNOME case) was not measured: it was held by `codex-stable-ui` and loaded by another session's podman builds the whole time.

Raw results: `~/putgd-lab/rr984/` on aurora-amd and m74 (`results.json`, `logs/record-*.log` with the `rate window` lines, `logs/ticks-*.log`).

