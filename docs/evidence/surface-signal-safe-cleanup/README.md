# Signal-safe surface cleanup (#815, #816, #762)

Evidence for the code PR that adds `tools/lab/surface_driver/critical.py`.
Core is `feat/surface-driver-core` at `97d335ec`; head is `0fdd2151`.

## Aimed-signal sweeps

`sweep.py TREE SCENARIO SIGNAL TRIALS` arms a POSIX timer for SIGNAL 0 to 150 us
before each cleanup's entry (timer slack 1 ns), on fake targets, with run.py's
handler behaviour: SIGALRM raises the recipe deadline, SIGTERM and SIGHUP record
`interrupted` and raise, SIGINT raises KeyboardInterrupt. A trial is bad when
the cleanup was skipped (for input: the press is unreleased and later input is
not refused), the handler did not run exactly once, its exception was neither
raised nor reviewed, a mask, handler or transaction leaked, a termination did
not stop the run, or a physical release was sent for a press that never went
out (#736). `runall.sh` and `runhead.sh` ran 2,000 trials per cell.

Bad trials per 2,000, as core (f44, Python 3.14.8) / head (f44, 3.14.8) / head (m74, 3.12.3):

| Section (entry aimed at) | SIGALRM | SIGTERM | SIGHUP | SIGINT |
| --- | --- | --- | --- | --- |
| click release, guest | 59 / 0 / 0 | 85 / 0 / 0 | 84 / 0 / 0 | 94 / 0 / 0 |
| click release, physical | 66 / 0 / 0 | 133 / 0 / 0 | 129 / 0 / 0 | 123 / 0 / 0 |
| drag release | 82 / 0 / 0 | 105 / 0 / 0 | 107 / 0 / 0 | 107 / 0 / 0 |
| held-key release | 99 / 0 / 0 | 126 / 0 / 0 | 134 / 0 / 0 | 126 / 0 / 0 |
| wheel release | 121 / 0 / 0 | 152 / 0 / 0 | 160 / 0 / 0 | 149 / 0 / 0 |
| default_browser exit | 0 / 0 / 0 | 1905 / 0 / 0 | 1910 / 0 / 0 | 1906 / 0 / 0 |
| default_browser setup recovery | 1944 / 0 / 0 | 1941 / 0 / 0 | 1943 / 0 / 0 | 1942 / 0 / 0 |
| fixture setup-failure cleanup | 386 / 0 / 0 | 389 / 0 / 0 | 386 / 0 / 0 | 380 / 0 / 0 |
| runner fixture cleanup | 424 / 0 / 0 | 474 / 0 / 0 | 517 / 0 / 0 | 419 / 0 / 0 |

Totals: core 19,107 bad of 72,000; head 0 of 72,000 on each host. Per-cell
tallies and up to six examples each are in `sweep-cells.json`. Core's
browser-exit SIGTERM/SIGHUP failures are mostly a termination swallowed by the
restore's retry; its setup-recovery failures are the dropped deadline (#816
finding 5) or a skipped recovery.

## Live boat trials

One sandbox `bx_s2bewrxc` (`putgd-lab-cinnamon-v2`, default, bare metal, KVM,
TTL 7200 s), fresh overlay `sig815a` of `cinnamon-x11-probes-v2`
(Mint 22.3 Cinnamon X11). Mint Update Manager stopped by exact PID 1871 first.
The sandbox was deleted afterwards (`boat info` 404).

- `live.py input`: 20 real QMP gestures with run.py's SIGALRM handler: 7
  clicks and 7 held-Shift drags with the alarm aimed 0 to 150 us before the
  pointer release, 6 held-Shift drags aimed at the key release. All 20
  raised the deadline after releasing; `xinput query-state` showed every
  button and key up after each (`live/input/`). `control.py` is the positive
  control: a QMP-held button and Shift read `down`, then `up` after release.
- `live.py browser`: 5 `default_browser` transactions with run.py's SIGTERM
  handler and the signal aimed 0 to 150 us after the body. All 5 restored, then
  raised `runner interrupted by SIGTERM`; the real `~/.config/mimeapps.list`
  (seeded, mode 640) matched byte for byte and mode after each, with no
  transaction left (`live/browser-results.json`). A first attempt's second
  setup timed out waiting for the private Firefox (60 s, VM under load) before
  any transaction opened; its recovery left the MIME files exact. The harness
  then retried such setups; the counted run had none.
- `sudo_live.py` (#762): a rule planted as a killed run would leave it was
  removed by the next staging (with a note), and after fixture cleanup
  `sudo -n -l` granted nothing and no rule file remained.
