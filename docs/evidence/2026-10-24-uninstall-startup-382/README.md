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
Targeted development-package and real-login acceptance is complete on five native
and six AppImage desktop rows. The separate PRs retain the legacy-account rollout limitation explicitly.
#382 remains open until its acceptance and merge requirements are met.

## Earlier link-removing implementation and bounded checks

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

## Race fix and source-collection correction

Superseded race-fix commit: `18bf9f340f772490877c8a1eafc39fba4bc5708b`, tree
`87225148c48c7d1cc932be5f7853e611cd08b6fe`.
All five native desktop lifecycle rows completed at `b15f47d5`, then an
additional live probe found a settings-change/uninstall race. If the executable
vanished before startup integration was rewritten, the old watch could be
disarmed without rearming cleanup. That [failed probe](race-fix/before.json)
left the entry/script behind. The fixed path re-arms cleanup on apply failure;
a newly armed watch also starts one settling check for an already-missing file.
The [same probe after the fix](race-fix/after.json) removed both managed files,
preserved `launchAtLogin:true`, and retained the original `autostart_io` diagnostic.
Both owned residents exited 0. These are bounded offscreen production-CLI/real
user-manager checks, not graphical acceptance.

The updated code passed the focused [Qt startup CTest](race-fix/ctest.log),
[25 shell tests](race-fix/script-tests.log) and
[14 complete-source tests](race-fix/source-tests.log) on Mint. This bounded
build's code bytes match the current commit, but its embedded Git identity is
the older dirty development checkout; exact package identities are recorded
separately below. [Before](race-fix/before-probe.py) and
[after](race-fix/after-probe.py) drivers and
[checksums](race-fix/checksums.json) preserve the regression.

The previous [full package run](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/40063676983)
failed AppImage complete-source verification on a zlib response; it was not a
successful CI run or accepted AppImage. The pinned SHA-512 was independently
verified against upstream and Alpine's pinned mirror. A declared HTTPS mirror
now provides a fallback that must match the unchanged digest; tests reject
incorrect primary/mirror bytes and unsafe mirror URLs.
[Verified mirror identity](race-fix/verified-zlib-mirror.json).
The failed CI response bytes were not retained, so their cause is unknown.

## GNOME next-login cleanup regression

The next exact-package pass used GitHub's PR test merge `69d277ed28d0`
(`26.1024.1189-dev+g69d277ed28d0`). Its application/packaging code matched
`18bf9f34`; the merge added only Neon lab tooling and documentation. Fedora,
Debian, Kubuntu and the Ubuntu VM completed lifecycle checks, but Fedora's
next-login journal exposed a [failed GNOME scope](login-grace/before-finding.json):
the missing-installation cleanup exited before GNOME attached its process.
The former aggregate checked failed units but missed this transient journal
failure. Those cycles are **not accepted**. Their raw logs, identities, hashes
and visuals remain private; Debian had an incomplete RFB frame and Ubuntu had
a login-transition frame, both excluded as visual proof.

Commit `bca1ff98c9dfb3ab7ec11efd8427659adcf7d3d4`, tree
`76f01e544acf07b40d6d0273318b66580c41b4f2`, gives missing-installation login
cleanup the same two-second replacement grace as the monitor. The
[26 script tests](login-grace/script-tests.log), including a native replacement
arriving during that grace, and [startup CTest](login-grace/ctest.log) pass.
A [real Fedora GNOME login](login-grace/result.json) with that exact script
template removed the files, preserved the preference, and recorded a successful
scope with no startup journal error. This script-only regression is **not an
exact-package pass**. The [renderer](login-grace/render.py),
[driver](login-grace/check.py), [visual](login-grace/cleaned-login.png),
[cleanup receipt](login-grace/release-lock.txt) and
[checksums](login-grace/checksums.json) are retained.

## Monitor lifecycle regressions

The next pass at `bca1ff98` completed the Fedora, Kubuntu and Mint native
cycles, but the stricter journal gate rejected Debian and Ubuntu at enabled
login. [Debian's receipt](monitor-lifecycle/d69-before.json) records transient
unit files disappearing while an unchanged watch was stopped and recreated.
[Ubuntu's receipt](monitor-lifecycle/u382a-before.json) records a deliberately
stopped initial cleanup check reported as a signal failure. Resident startup
succeeded in both cases; their journal errors still block acceptance. All five
rows are superseded and their profiles/packages were cleaned up.

Current code: `d5b561ddb94f8255b8bc754c1382552d983a6fa9`, tree
`797721fa0672581562a8675d0d2010eced7c6bd4`. It preserves a watch when reapplying
the same installation identity, stops the prior watch when disabling or changing
identity, and declares SIGTERM successful for the deliberately stopped oneshot
check. Other failures remain errors. A focused Qt regression checks unchanged
native identity (including inherited AppImage environment) versus a changed
launcher; [startup CTest](monitor-lifecycle/ctest.log) and
[26 shell tests](monitor-lifecycle/script-tests.log) pass.

The [real Debian user-manager regression](monitor-lifecycle/debian-direct.json)
passed ten rapid watch start/stop cycles with no journal errors. The
[Mint production-CLI regression](monitor-lifecycle/mint-cli.json) passed ten
settings changes without replacing the watch, three disable/enable cycles,
a launcher-location change that retired the old watch, and uninstall racing
with another settings change. Both owned residents exited 0; no unit journal
error appeared. These are **bounded/offscreen checks**, not exact-package or
desktop acceptance. [Source hashes](monitor-lifecycle/source-identity.json),
[unit driver](monitor-lifecycle/unit-lifecycle.py),
[CLI driver](monitor-lifecycle/cli-lifecycle.py) and
[checksums](monitor-lifecycle/checksums.json) preserve the evidence.

## Exact package and real-login acceptance

All eleven targeted lifecycle rows completed at clean `d5b561dd` using actual
CI DEB/RPM/AppImage development artifacts, with real logout/login, active cleanup
with the resident quit, unchanged settings/data, explicit disable, reviewed
visuals and 180-second memory observations. Both the full branch CI and the PR
integration CI (attempt 2) passed. Fedora native's pre-existing Qt/OpenSSL strict
map failure is preserved explicitly. See the [complete matrix, exact artifact
hashes, annotated pictures, lab corrections and retirement receipts](package-acceptance/README.md).

Legacy entries in accounts that never ran the updated app cannot gain cleanup
retroactively; the [rollout limit](package-acceptance/README.md#legacy-account-rollout-limitation)
remains open with #382.

The published-nightly check (including AppImage replacement with real update
metadata) remains after code/docs merge. These targeted rows do not certify a
stable release or replace #384's exact-release-asset campaign.

## Superseded retained-link evidence

The files in this folder's root predate Patrick's correction. They document
source `91864c41`, whose native entry used `TryExec` and deliberately stayed
behind. Its focused startup tests, 18 AppImage script tests and isolated CLI/GIO
probe passed for that **superseded** behavior. They do not accept the current implementation.
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
