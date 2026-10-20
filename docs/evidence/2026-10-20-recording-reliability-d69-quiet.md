# Recording reliability: d69 quiet re-run of the #130 pause A/V (PR #143)

This addendum to PR #143's receipt (`docs/evidence/2026-10-20-recording-reliability.md`) covers one re-run. The receipt's d69 PR row came from runs at host load 3.7–6.9: another session's build was running on d69 at the same time. The same three runs were repeated on a quiet host.

- **Host:** d69, Debian 13, Plasma 6.3.6 Wayland, `kde-screencast`.
- **Build:** AppImage `putgd-0.1.1-dev.533+gcc4568f248ce`, sha256 `23cee22c58d12b35…`. This is PR #143 head `cf397e1` merged into nightly.
- **When:** 2026-10-20, 03:25–03:28Z.
- **Load during the runs (1-min):** 0.55, 1.40, 1.97.
- **Each run:** the `--av-marker` clip played full screen through the default sink. Record 8 s, pause 3 s, resume, record 8 s, stop, then Save copy.
- **Measurement:** the build's own `putgd-record --av-sync`, with offsets split at the pause by `pairTimesS`. Offsets are audio minus video in ms; positive means the sound is late.

| Run | Format | Capture step at the pause | Export − working after the pause |
| --- | --- | --- | --- |
| p1 | MP4 | −0.3 | 0.0 |
| p2 | WebM | +0.4 | −1.2 |
| p3 | MP4 | −1.3 | −0.7 |

The loaded run gave −0.3/0.0/0.0 and −0.3/+0.5/+1.4. Nightly `134a9be` on the same path gave capture steps of +17.0/+16.7/0.0 and an export 10 ms earlier than the working clip in every run.
