# S3 and Proton destination controls — native continuation, 2026-10-16

Saved S3 Show/Hide and visible destination-test feedback passed current-source
native checks on Cinnamon first, then GNOME and Plasma. Proton verification was
limited to regression tests and signed-out UI smoke because the user reported its
live E2E already complete. This campaign used no live cloud account.

## Candidate and build

- Fresh main base: `d7b17bb03e39a40fe8a5f957265d73563e1220d8`.
- Tested source: `3a4fa5fb02c80d90689253e792c59edd92660c32`; tree `2291d7849416a3b8dcb61eca40db04381474bd2f`;
  version `0.1.1-dev.237+g3a4fa5fb02c8`.
- Cinnamon: host-built Mint package tested in the Mint VM with shared distro Qt 6.4.2.
  GNOME and Plasma: separate native guest builds with shared distro Qt 6.10.2.
  All use the verified official AWS C++ SDK 1.11.907 and pinned Proton runtime.
- Lab archive SHA-256: `746810042ade6f0f8b4b3f3c9b25f312f2ad7ea98c15c71e3733ecdbaa96925d`.
  It contains committed production build inputs plus the independent desktop
  fixture. Lab packages include that fixture and are not release-distribution packages.
- This receipt is a documentation-only follow-up and does not relabel the tested
  binaries. Private identities retain each exact source/package/binary hash.

## Checks

`cmake --preset dev`, `cmake --build --preset dev`, and
`ctest --preset dev --parallel 2`: **59/59 passed**, 349.44 seconds, including
saved-secret undo, CLI rejection, setup jobs, S3 permission guidance, Proton
account/folder checks and GUI result handling. Socket templates were checked in
private `/tmp/ct.djxe59ar`; the longest normal template was 82 UTF-8 bytes.

Each native desktop passed nine Qt cases (region, window, desktop, three clipboard
formats and three region-cancellation paths), three independent GTK clipboard
cases, six Settings/History/Activity routes, and history preservation through a
resident restart. The production lab runner and memory harness were unchanged.
All graphical interactions stayed visible through localhost VNC and the existing
`http://127.0.0.1:8765/` display wall.

Native S3 UI checks entered only fixed synthetic credentials, stored them for the
isolated session, revealed and hid the saved value, and verified repeated Undo
could not restore it. The actual production SDK contacted a private loopback S3
oracle. Test connection displayed progress, successful upload/public GET/cleanup,
an invalid-key diagnosis, and `s3:PutObject` IAM/bucket-policy guidance. Successful
cleanup was `removed`; all three attempts had distinct recorded test identities.
Proton's signed-out Test connection displayed the account/folder setup guidance.
These prove native UI behavior with a synthetic backend, not cloud service E2E.

## Short memory observations

The user requested short tests. Each unchanged `bench_ui_process.py` observation
used `--seconds 30 --interval 5` after native use, with seven samples, one matching
resident, no live UI or escaped child, RSS below 40 MiB, and no swap. CPU is an
independent numeric `/proc` observation over the same interval.

| Desktop | Peak RSS MiB | CPU, one core | Swap KiB |
|---|---:|---:|---:|
| Cinnamon/X11 | 28.70 | 0.000% | 0 |
| GNOME/Wayland | 31.67 | 0.000% | 0 |
| Plasma/Wayland | 31.98 | 0.000% | 0 |

These are 30-second post-use VM observations. They establish neither cold-idle,
long-duration/leak, physical-machine nor complete U9 fresh-agent acceptance.

## Review, failures and retention

Review found and resolved saved-secret resurrection through Qt undo history.
`setText(QString{})` clears that history; the exact repeated-Undo regression and
native checks passed. Review also moved Proton result interpretation into its
provider and added versioning/exact-version cleanup permission assertions. The
final source review has no outstanding actionable finding.

Retained setup failures: the initial Cinnamon SSH pin lacked its new port; an early
capacity query preceded bootstrap-key installation; the private UI controller
requested an unsupported destination view before switching to the documented
Settings route and native navigation; GNOME's runtime tar was initially extracted
one directory too deep. The corrected commands and original attempts remain
private. A two-guest memory preflight rejected parallel GNOME/Plasma launch, so
those desktops ran serially with unchanged six-GiB guest allocations.

GNOME acceptance followed visible logout/login into a new Shell process with the
exact installed candidate extension. Protected login input was checked as an owned
private regular file, read only in memory and entered into the visible masked
field. Its value was never recorded. No user cloud credentials, real payloads,
bucket controls or existing user objects were used or changed.

Private evidence is retained under `work/sc1/`: host CTest reports, per-desktop
native/UI/memory records, original failures, reproducible controllers and source,
package identities and completion manifests. All three disposable overlays were
retired only after evidence export and checksum verification; golden disks remain
unchanged. Original checkout staged/unstaged work and unrelated qemu-grid files
were preserved.

| Desktop | Run identity | Completion manifest SHA-256 |
|---|---|---|
| Cinnamon/X11 | `fe0c762d-ed2d-4a6c-a8cf-3102e89c58b3` | `67867a26866d23ab9dc4752f0e1bc7dbf2373677e5fb87f91049b4316a1c1505` |
| GNOME/Wayland | `e19f10a2-5c0e-4226-99e1-d01415f446c3` | `01a414f9882ad28a2b2625813540f8b71f68f03ac9d264fb8c2b993132b04787` |
| Plasma/Wayland | `f3441399-0d44-42c0-b7dc-cb8fc97767c9` | `0a4efced078681ee45d958176230350a6732c1247b5c1f0f625fc4477e05b9be` |
