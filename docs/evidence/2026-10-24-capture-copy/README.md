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

## Tests-first checkpoint

The tests-only commit was built on f44 with the normal Dev preset, official AWS
SDK 1.11.907 and dynamic distro Qt. The added History row test and production CLI
test reproduce the unchanged `suppressed` status after a successful link copy.
Two unit data rows reproduce `suppressed` and `failed` recovery; native-copy
refusal/suppression and batch-subset controls pass. The corrected private-bus
GNOME run has four expected failures: three absent endpoint errors and an endpoint
that disappears after discovery. Permission, malformed JSON and incompatible
protocol controls pass. These are contract results, not compositor evidence.

The first GNOME attempt incorrectly retained an `offscreen` override and reached
`session_unsupported`; it is preserved separately. Its corrected command uses
`QT_QPA_PLATFORM=wayland`, the synthetic GNOME Core session environment and a
private D-Bus session. No real Shell endpoint is replaced.

## Fedora native install blocker

f44 has a prepared DNF offline transaction (`download-complete`, no reboot
trigger). Its DNF 5.4.6 invalidates that queue after successful online installs.
The coordinator required preserving the queue and rejected both direct RPM
installation and backing up/restoring its state as bypasses. Scoped physical
checks may use a newly extracted, audited RPM with a temporary matching helper;
those are **extracted-RPM physical evidence**, never native installation proof.
Fedora's actual package-install/discovery gate remains blocked under #492.

## Pending gates

Builder tests, clean package-install discovery/re-login on Fedora GNOME 50 and
Ubuntu GNOME, disabled-helper recovery and enabled capture, and current-head
Cinnamon/GNOME/KDE functional and memory evidence are in progress. This report
and the companion PR remain draft until the required evidence is collected.
Offscreen and private-bus tests cannot establish compositor acceptance.

## Physical KDE checkpoint (in progress)

k72 runs Kubuntu 26.04, physical Plasma/KWin 6.6.6 on Wayland, distro Qt 6.10.2.
Its production DEB is `26.1024.1135~dev+g8a9415c540be`; the source is the code PR
head above. The production package audit passed. A first fixture-enabled lab
package was correctly rejected by the release audit and preserved separately;
the installed package has the fixture disabled and uses a separately built,
independent Qt witness.

The actual row **Copy link** action changed the persisted operation status to
`copied`. Both the row and inspector then show **Link copied**, and the synthetic
stored URL pasted into the independent native application. The same recovery
passed through the production CLI. The seeded link is deliberately
`https://example.invalid/copy-recovery`: these checks create no remote objects
and establish clipboard/history behavior, not upload or live-link availability.

![Before manual Copy link: the clipboard-changed hint](k72/copy-before-annotated.png)
![After manual Copy link: both row and inspector confirm success](k72/copy-after-annotated.png)

The empty native resident's bounded cold observation passed 61 samples over
300.000 seconds: 31.617 MiB peak RSS, zero swap, maximum one-core CPU 0.199%,
maximum sample gap 5.028 seconds, one exact resident and no UI/escaped child.
The raw sampler's diagnostic limitations remain intact; the separate evaluator
checks only these sampled numeric gates. Final native-matrix and post-use
observations remain pending, so this is not complete desktop acceptance.
