# Verification record

> **Historical record.** This is a dated run record, moved unchanged from [implementation evidence](../../testing/verification.md) on 2026-11-05 (#1222). It describes what that run found for the build it names, and its open or pending statements describe that time. "Above" and "below" refer to its neighbours on the old page, in the order of the [history index](../README.md#verification-run-records). For today, see the [current state](../../testing/verification.md#current-state). Do not use it as a work list.

## Native package full acceptance (#375, #377, #382) — 2026-10-23

[Evidence](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-23-native-full-acceptance/README.md): the
[native package profile](../../testing/acceptance-matrix.md#native-package-full-acceptance-profile)
(A–Q) on nightly `v26.1023.1058-nightly` (`05991d9d`). It covered the `ubuntu24.04` DEB on m74
(Mint Cinnamon X11), the `ubuntu26.04` DEB on k72 (Kubuntu Plasma Wayland, plus its X11
session for recording) and on an Ubuntu 26.04 GNOME VM, the `debian13` DEB on d69 and the
`fedora44` RPM on f44.
- **Passes everywhere:**
  - fresh install from pkg.put.gd;
  - the never-run dev.133 → nightly upgrade with a 10,000-entry history (#56 layout move,
    schema 5→7, rows, counts and config preserved);
  - 0.1.1-nightly.947 → CalVer through apt and dnf;
  - recording with the distro media stack, motion against a static control;
  - a real browser loading a live upload;
  - the read-only repository channel;
  - start at login and removal.
- **Fails:** item P on m74, because of [#490](https://github.com/patrick-hudson/put.gd-desktop/issues/490):
  the login-started resident survives logout on Cinnamon.
- **Other findings:** #491–#495.
- **#382:** the per-desktop behaviour of the retained login entry, with and without
  `TryExec`, is recorded for Patrick's decision.
- **Scope:** a historical nightly pass. Release acceptance requires fresh evidence
  on the exact release bytes with reviewed drivers; use the #495 follow-up below
  for the retired recipes and maintained replacement checks.
