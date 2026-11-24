# Recording encoder records

> **Historical record.** These are dated run records, moved from [capture and export engine](../design/recording/capture-and-export.md) on 2026-11-09 (#1230). They describe what those runs found at the time, and their open statements describe that time. For the current encoder order, see that page. Do not use them as a work list.

## Skylake GPU hangs, 2026-10-17 to 2026-10-20 (#89)

The lab's Intel HD 530 hosts logged seven i915 hangs between 2026-10-17 and
2026-10-20: six on k72 and one on d69. All of them were `Resetting rcs0 for
preemption time out` with `ecode 9:1:85dffffb`, in the compositor's
context (Xorg or `kwin_wayland`) on the render engine. The video engine that
`vah264lpenc` uses (`vcs0`) never reset. Four of the seven came with no
VA-API encode running. One happened during a CPU GIF export, one at idle with
builds running, and two before any recording work. The other three, in two
events, overlapped a `vah264lpenc` recording at host load 5–9. m74 (the same GPU, Mint X11)
recorded with `vah264lpenc` without a hang, and k72 was taken out of the
lab for a hardware check on 2026-10-20. Kernel versions, drivers and
the per-host table are in
[the closeout receipt](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-20-recording-closeout.md#89-gpu-first-encoder-order-kept).

## Export speeds on the workstation, 2026-10-18

On the workstation (i9-9880H, UHD 630, Quadro T2000, 60 s 2238×1318 clip
with audio, `putgd-record --export`): H.264 and H.265 on NVENC export at
about 3.9× real time (15.6 s), H.264 on the Intel iGPU through VA-API at
2.6× (23.5 s), `libx264` at 2.1×, VP9 at 1.7×, a 10 s GIF at 0.9×; `libx265`
stays below real time (0.68× at `faster`).

Live capture on the same machine: `vaapih264enc` captures 720p at half the CPU
of `x264enc veryfast`.

## AMD hardware encoding, recorded 2026-11-04 (#138)

On an AMD Radeon 740M (Aurora Fedora 45, Plasma 6.8.2 Wayland, Mesa 26.2.6 from
negativo17), live H.264 used `vah264enc`, and H.264 and H.265 exports used
`h264_vaapi` and `hevc_vaapi`, with the GPU's encode engine counters confirming
the work. Fedora's stock Mesa on AMD has not been tested.
