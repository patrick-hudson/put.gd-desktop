# Current-source native UI split evidence

Frozen software: 0.1.1-dev.41+g97ca3a521714,
commit 97ca3a521714e2717cedc1139976130627bfb8a8,
tree 888460466ff1d240bd8e3c61189f36f407fb3485,
Git archive SHA-256 cf2a94b8594b6c0a5295b6a9e87884447a330b3ceed726ec2fb499cca7ca95c1.
Later evidence-only commits do not relabel these binaries or results.

The host directory preserves the successful standard Release/Dev/MCP build/test
receipts and logs. Cinnamon/GNOME/KDE each have a native receipt, an explicitly
selected native evidence directory with its original manifest, and a separately
allowlisted memory export. Memory source-relative paths are preserved inside
`desktop/memory/`; the unit receipt is at `desktop/memory/memory/unit-receipt.json`.
This retains command/guard/preflight separation rather than exporting whole VM
run directories. Historical wrappers use recorded work paths; read their replay
instructions and docs/testing-ui-process.md before using fresh labels/identities.

All three graphical functional subsets use their existing overlay and visible
localhost VNC console. Each covers the 12-case native matrix and four real tray
actions, four normal helper view lifecycles, clipboard paste after helper exit,
drafts/conflicts/Cancel/native dialogs, own-view/modal capture exclusion and a
real two-file local copy active after helper exit. The 512 MiB fixture is streamed
and excluded from exports. Cinnamon's user-confirmed manual region is metadata
only, excluded from scripted acceptance; its pixels/payload were not read or
exported. Old app residents were identity-checked and stopped, their terminal
operations/profile preserved, and golden disks untouched.

These are functional subsets, not full AE17/compositor or S3 certification. The
GNOME selector readiness/focus history, native crash/version/credential cases,
remote publication/deletion, mixed displays and release/fresh-agent gates remain
explicit in the top-level current-source record.

Each VM quiet sample has 25 records over 120 seconds, helper absent/no escaped child, exact
resident/build/socket ownership and 26 stable operation/UI guards. These post-use
samples follow views/captures/clipboard and a 512 MiB local copy, not a matched
historical saving or cold 40 MiB budget. Keep RSS/PSS/private together. GNOME's
post-measurement Popen-method defect and corrected future driver are both retained;
there was no timed rerun. No credentials/private keys/configuration contents or
payload bytes are part of the allowlists.
