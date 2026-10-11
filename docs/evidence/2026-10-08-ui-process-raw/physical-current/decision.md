# U11 measured decision — retain the UI split

Tested product: `0.1.1-dev.41+g97ca3a521714`, full source commit
`97ca3a521714e2717cedc1139976130627bfb8a8`; historical retained baseline
`bcbe63f0d1e1ad00f36ac110fb596d0245869617`. Evidence-only later commits do not
change these measured identities. Source archives, executables, fixtures, native
receipts, commands, host conditions and observer hashes accompany the raw results.

Two serial runs per build used the identical empty local fixture, three cycles
of Settings/History/Activity, five-second startup warmup, two seconds visible per
mapped view, thirty seconds quiet after final closure, five-second records. The
order was retained1/split1/retained2/split2. All four runs passed, with no operations,
changed resident or escaped descendants. Normal window-manager Close released
all observed helpers; no signal was used as proof of UI closure.

| Whole observed family | Retained | Split | Saving |
|---|---:|---:|---:|
| Median final closed RSS |65.0879 MiB|45.0410 MiB|20.0469 MiB /30.80%|
| Median final closed PSS |40.3989 MiB|25.4023 MiB|14.9966 MiB /37.12%|
| Median final closed private |37.5859 MiB|22.8867 MiB|14.6992 MiB /39.11%|
| Maximum sampled open RSS |73.1914 MiB|120.8359 MiB|47.6445 MiB higher|
| Maximum sampled open PSS |44.4893 MiB|55.0088 MiB|10.5195 MiB higher|
| Maximum sampled open private |37.6680 MiB|32.2969 MiB|5.3711 MiB lower|

Summed RSS double-counts shared pages. The open PSS cost is material too, so the
saving applies between view use rather than promising lower memory in every state.
The median of four repeated Settings reopens per build is 100.92 ms retained versus
332.68 ms split; the six final closes per build are 123.81 ms versus183.66 ms. Initial
Settings activation is 432.14 ms versus379.51 ms (two samples each). Additional views
are 103.76 ms versus145.42 ms (twelve each). These are request-to-mapped-window times,
including observer work, not finished painting or p95 certification. Full event
values and counts are retained in latency-breakdown.json.

The separate physical empty cold run lasted 300.605 seconds: 44.390625 MiB peak RSS
and 0.0099799% of one CPU core. Original 100 MiB/<0.5% idle budgets pass; the 40 MiB
cold target fails. Do not subtract a historical resident-only cold CPU run with
different observer controls to claim a CPU improvement.

Current-source native capture/tray/clipboard, safe drafts/conflicts/Cancel, native
folder dialogs, helper normal close and real 512 MiB + 23-byte local-copy continuity
passed on Cinnamon/X11, GNOME/Wayland and KDE/Wayland. Their separately guarded
120-second post-use VM observations are not matched old/new savings. Cinnamon's
user manual region contributes metadata only and its pixels/payload remain local.
GNOME's post-measurement host wrapper error is preserved; valid timed samples were
not repeated, and a corrected future replay driver is separate.

Retain the split for the substantial closed-state total-process reduction while
keeping native fonts/dialogs and the tray. Still open: 40 MiB cold goal, populated
history/repeated-capture/post-S3 plateaus, real S3 publication/deletion and slow
remote continuity, native crash/version/typed-credential cases, historical GNOME
readiness/focus, mixed displays and remaining release/fresh-agent gates. Ranked
read-only attribution records the next locating probes; it predicts no removable
bytes. No native-font override, integer packing, toolchain substitution, library
unloading or bucket policy change was introduced.

A subsequent four-run standard Qt locating probe measured only 0.629 MiB settled
full-versus-native-style RSS difference and 0.1875 MiB immediate style allocation.
It is a five-second diagnostic with different menu appearance, not a product
variant or a 300-second saving. Preserve the current product styling; do not claim
it can close the 4.39 MiB gap. Source/binary/commands/stages and diagnostic warnings
are explicitly exported under style-cost-probe/.
