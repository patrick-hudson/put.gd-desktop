# Live-S3 regression: source114

Measured commit: `0258600b752dae6304ba61d48b67d56883dfb6b2`; tree:
`7dd03c352aac2a21ad1723bedd42e0c4a4774d9a`; runtime:
`0.1.1-dev.114+g0258600b752d`. Documentation/CI commits do not relabel it.

| Desktop | Peak five-minute RSS + swap | Swap | Production objects removed |
| --- | --- | --- | --- |
| Cinnamon/X11 | 29.059 MiB | 0 | 31 |
| GNOME/Wayland | 36.078 MiB | 0 | 31 |
| KDE/Wayland | 31.285 MiB | 0 | 31 |

All three actual desktop runs passed 12 native and 8 GUI checks, 30 live operations
and 31 objects across all 6 sources. Public content/SHA, native URL paste, all 15
80-by-60 region pixel checks and independent window/desktop frame checks passed.
The fixed five-minute windows had one resident, no active jobs/helpers, no swap,
unchanged identity and CPU below 0.0201% of one core. Exact production deletion,
local archive/spool cleanup and normal runtime drain passed. No SDK error,
automatic retry or emergency cleanup occurred.

The additional KDE target passed 12 uploads/deletions before full regressions:
105 source114 objects were published and production-deleted. The failed
source113 target published 3 additional objects, also production-deleted; its
fourth failed operation/artifact remains private. Campaign reservations total
125 objects/28.4375 MiB against 128/32 MiB, including failed/unexecuted slots.

## Replay and reconstruction

```sh
python3 docs/evidence/live-s3-memory/source114/replay.py \
  --manifest docs/evidence/live-s3-memory/source114/manifest.json --require-target
```

System Python is sufficient; no desktop, AWS access, credentials or Node is
needed. Each archive contains metadata only, exact evaluator inputs, transformed
private paths, numeric samples and production receipts. The joined manifest
bounds members and verifies archive/member/helper hashes. Replay independently
recomputes each result and requires exact equality with its original saved JSON.
It validates historical evidence, not a new compositor or cloud run.

The frozen evaluator/observer bytes remain separate per desktop. The exporter
records original hashes before removing raw configuration snapshots and payload
bodies; protected values were checked only in memory and were never written.
Witness image hashes reference private graphical evidence; images are excluded.
The helper-path adapter and its negative checks are recorded in
`adapter-validation.json`; the original Cinnamon exporter is preserved separately.

[Original controller sources](controllers/README.md) retain exact recorded bytes,
hashes, sequencing and timing. These are historical drivers requiring their
recorded lab/private inputs; the guide explains reconstruction rather than
claiming a portable new acceptance command. Never blindly invoke their old paths
or namespace. A new run needs fresh private profiles, socket/capacity checks,
versioned builds, a bounded ledger and exact receipt-guarded cleanup.

## Qualifications

This is one current-source full regression per desktop. Two source106 Cinnamon
profiles remain separately recorded; no new fresh-repeat optimization decision,
full release/fresh-agent acceptance or universal 40 MiB active-work ceiling is
claimed. All original source106/108 TLS failures and the failed source113 target
remain failures. Cinnamon's two earlier native attempts also remain qualified:
one lacked timely independent paste evidence; another hit the clipboard-change
guard with a late producer acknowledgment. The later unchanged controlled matrix
passed. Its numeric XFixes trace does not establish either historical cause.

[Transport diagnosis](../../../testing-live-s3-transport.md) records the controlled
TLS comparison, policy guard and missing-optional-include correction. The
resetting peer was not identified; successful SDK tests do not prove a particular
firewall or packet-fragmentation cause. [Memory strategy](../../../memory-strategy.md)
explains the cumulative worker/cleanup design.
