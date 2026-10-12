# Historical source114 controllers

These are byte-identical copies of the controller code used for the recorded
Cinnamon, GNOME and KDE full workloads. Their separate directories preserve
desktop-specific imports and filenames; no combined runnable wrapper exists.
`manifest.json` records original project-relative paths, byte hashes and the
parameter/receipt that pins each file. Copies are read-only. No controller was
run while preparing this export.

Use these sources to inspect and reconstruct a new, separately reviewed test
recipe. Do not run the copies against the historical VM directories or live
bucket. They contain hard-coded private paths, import `tools/lab/lab.py` from
recorded checkouts, require protected lab identity/GO/pin files, and some load an
old private baseline document only to prepare fresh configuration. Those
configuration documents, credentials and lab private identity files are not
exported. A fresh recipe needs actual current source/package hashes, new private
profiles/namespaces and a new nonrefundable budget. It must validate real VNC
desktops, socket path limits, helper ownership, protected credentials and durable
publication/deletion receipts. Recorded sources are not a fresh acceptance CLI.

The source114 measured workload is 30 operations/31 objects over six sources,
with 20 seconds between batches and 15 region pixel checks. The observer records
at 1 Hz; the evaluator excludes the first five quiet seconds and scores 300 seconds.
Inspect each desktop's recorded parameters for its exact command order and
preparation/finish steps. Preserve timings, oracles and failure qualifications
when reconstructing. Live tests require explicit live-bucket authorization and
exact receipt-guarded cleanup; offline archive replay requires neither.

The shared observer is copied independently for each desktop. Its hash matches
each actual run's `parameters.json`. KDE's budget module matches the byte-identical
budget pinned by Cinnamon/GNOME receipts; KDE's own parameter receipt independently
records the 32-object/10 MiB cap. This is identified as a cross-receipt hash anchor,
not an invented KDE pre-run hash field. No placeholder audit files or aliases
were manufactured.

The controllers also depend on the source114 tracked `tools/lab/lab.py`, socket
preflight utility, production CLI and packaged desktop fixtures. Live graphical
oracles use host Python/Pillow, SSH and the existing QEMU/VNC lab metadata. These
dependencies are recorded project/lab inputs, not duplicated substitutes in this
export. Reconstruct through the standard build and immutable-overlay lab workflow.

Build/deployment scripts and unrelated prior experiments are excluded. GNOME's
deployment quoting correction is outside these workload sources and remains in
its original qualified evidence. GUI images, payload bodies, raw configuration,
credential values and lab login inputs are absent. Code legitimately names
credential fields and the protected input filename; the actual values were read
only in memory for exclusion scanning of every copied file.
