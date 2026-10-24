# Install, capture and copied-link recovery — work in progress

Companion code PR: [#534](https://github.com/patrick-hudson/put.gd-desktop/pull/534),
source `8a9415c540bee2ac5a8caa0e7606681bafa528a7` from nightly
`50053b01f1bbd7f3776a53513bb8c43151ce0de8`.
Tests-first source: `c777787deb7b708d9bd70f8ba01da7bcafdef480`.

## Published metadata check (#466)

On 2026-10-24, the README's previous `-nightly\.` filter selected
`v0.1.1-nightly.947`. The corrected `endswith("-nightly")` filter selected
`v26.1023.1058-nightly`, published 2026-10-23 at 04:10:17 UTC. The
[payload-free GitHub release listing](published-releases.json) records the input.
No package was downloaded or installed on the workstation. The README badge
now uses the same current tag suffix.

## Pending gates

Builder tests, clean package-install discovery/re-login on Fedora GNOME 50 and
Ubuntu GNOME, disabled-helper recovery and enabled capture, and current-head
Cinnamon/GNOME/KDE functional and memory evidence are in progress. This report
and the companion PR remain draft until the required evidence is collected.
Offscreen and private-bus tests cannot establish compositor acceptance.
