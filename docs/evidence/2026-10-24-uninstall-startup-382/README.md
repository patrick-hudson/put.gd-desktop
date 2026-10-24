# Login-startup uninstall contract (#382)

Patrick corrected the decision during acceptance testing: preserve settings, but
remove the managed startup link. Active accounts remove it when the installation
disappears, even if the resident was quit; inactive accounts may remove it at
next login. Package scripts still do not access user homes. The
[recorded correction](https://github.com/patrick-hudson/put.gd-desktop/issues/382#issuecomment-6379948629)
supersedes the earlier retained-entry/TryExec proposal.

[Code PR #544](https://github.com/patrick-hudson/put.gd-desktop/pull/544) implements
the correction. [Configuration](../../configuration.md#launch-at-login-and-uninstall-linux)
documents the timing, preserved preference and explicit Settings/CLI/MCP route.
Both PRs remain drafts until current package and real-login acceptance is recorded.
#382 remains open until its acceptance and merge requirements are met.

## Revised implementation and bounded checks

Code commit: `b15f47d589e8f695fddcb93bc1b6130d42c57b43`, tree
`51a4ea8d1a403aaff9e5a3e2c641d0463672d8b1`.
The checks below used a dirty build based on `91864c41` on physical
`mint-builder` (`192.168.68.74`) through SSH. Its six changed code/test blobs
[exactly match the revised commit](revised/source-blobs.json). The embedded build
identity is still `26.1024.1182-dev+g91864c41bcc4.dirty`; it is not a package
built at the revised clean commit.

The build uses C++20, distro Qt **6.4.2**, GCC 13.3.0, CMake dev preset/Ninja and
the previously verified official AWS SDK 1.11.907. Heavy work stayed on Mint,
with `nice -n 10` and two build jobs. Private offscreen profiles contained no
provider or credential configuration. Real user-manager checks held the Mint
desktop lock; no graphical result is inferred from them.

- The [focused startup CTest](revised/ctest.log) and
  [Qt cases](revised/qt-cases.log) passed. They cover the common user-owned
  startup script, migration of older entries/scripts, managed-only changes,
  inaccessible native executables, AppImage identity and literal path text that
  resembles a template token.
- All **24** [shell contract tests](revised/script-tests.log) passed. They cover
  native and AppImage missing-install cleanup, preserved configuration,
  foreign/replaced entries and symlinks, and the existing AppImage channel,
  version, ownership, permission and replacement checks.
- The [production CLI/user-manager/parser check](revised/native-live-result.json)
  passed. With the resident quit, a real transient user-manager path watch
  removed the entry and script **2.103 s** after the copied native launcher was
  unlinked. Configuration bytes were unchanged. GLib launched the generated
  desktop entry through a script pathname containing spaces, apostrophes,
  quotes, backslashes, a tab, percent/dollar signs and template-like text.
  Replacing the executable before the settling check kept startup intact.
- Disarming that watch and launching the entry through GLib with the executable
  absent exercised the next-login cleanup path. This is an **inactive-account
  simulation**, not a real logout/login. The expected-revision production
  `settings.patch` setting `launchAtLogin: false` removed the managed files and
  disarmed the monitor; `ui.snapshot` confirmed `enabled:false, applied:true`.
  All three owned residents exited 0.
- The [AppImage-directory watch check](revised/appimage-live-result.json) used
  synthetic executable ELF fixtures with AppImage metadata. The real user
  manager watched a directory containing spaces, an apostrophe, percent and
  dollar signs. Removing the last image removed managed startup files and kept
  configuration. An eligible replacement kept the entry without starting the
  fixture; removing that final replacement then cleaned it. Both owned
  residents exited 0. **No actual AppImage, FUSE or desktop acceptance is claimed.**

The repeatable bounded drivers are
[native/CLI/parser](revised/native-live-check.py) and
[synthetic AppImage watch](revised/appimage-live-check.py).
[Socket preflight](revised/socket-preflight.log) checked both the isolated session
socket and real user bus before launch. Only exact owned process groups and
installation-ID-bound user units were stopped. Private profiles, failure logs
and raw evidence remain on Mint. [Checksums](revised/checksums.json) cover these
sanitized retained files.

## Exact package and real-login acceptance

The [full CI/package run](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/40063676983)
is building the revised clean commit. The required native rows are Mint,
Kubuntu, Debian, Fedora and the Ubuntu GNOME VM; AppImage additionally covers
the Aurora 45 KDE VM. Each needs current package identity, real login, active
cleanup with the resident quit, next-login cleanup without a running watch,
preserved settings/data, explicit disable, saved visuals and memory results.
No row is marked passed by the bounded checks above.

This work accepts the uninstall change only. It does not certify a stable
release or replace #384's exact-release-asset campaign.

## Superseded retained-link evidence

The files in this folder's root predate Patrick's correction. They document
source `91864c41`, whose native entry used `TryExec` and deliberately stayed
behind. Its focused startup tests, 18 AppImage script tests and isolated CLI/GIO
probe passed for that **superseded** behavior. They do not accept `b15f47d5`.
The original [build identity](build-identity.json),
[host/source](host-source.json), [commands](commands.json),
[CTest](ctest.log), [script output](appimage-script.log),
[CLI result](cli-result.json), [driver](cli-check.py) and
[cleanup](cli-cleanup.json) are retained as history.

A partial native-package/real-login pass at `91864c41` also remains privately
under the code worktree's `work/i382/acceptance/`. It stopped when the contract
changed. Its manual package run was cancelled; no AppImage was accepted. Some
Kubuntu VNC frames were uninitialized and are invalid visual evidence; portal
and wall recovery logs were kept. None of those rows is reused as a revised
contract pass.

Failed setup/probe attempts are retained. The original configure needed explicit
SDK runtime/source paths; the first GIO probe misinterpreted PyGObject's NULL
constructor exception. In revised live-check setup, the generated launcher
needed its installation execute bit and the copied build needed its UI helper.
The first live monitor probe then found that `systemctl show` supplies a default
Description even for an unknown unit. The implementation now checks
`LoadState=not-found` before ownership matching. The corrected live checks above
passed. Setup failures and the failed probe are not relabelled as passes.
