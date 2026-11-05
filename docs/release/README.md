# Versions, CI and releases

How a commit becomes a version, what CI runs and how packages ship.

## Guarantees

- Supported packages are DEB, RPM and AppImage.
- Every feature, fix, documentation and tooling pull request targets `nightly`. `main` changes only through a promotion pull request from `nightly` or a hotfix pull request. Nobody pushes to either branch directly.
- Published tags are never moved or deleted. Never create, move or delete `v*` tags by hand.
- Versions come from the commit itself, never from the clock, and no file is bumped by hand.
- The `CI` summary is the only required check; a cancelled or skipped-by-failure job is not a pass.
- Each version gets exactly one tag, and every set of bytes that ships needs its own acceptance receipt. Stable publication stays manual after exact-asset acceptance.
- Nightlies never replace GitHub's Latest stable release.

## Documents

- [Software versions](versioning.md): CalVer `YY.MDD.N` and build identity.
- [CI coverage and runtime](ci.md): workflows, job selection, caches and the builder stress lane.
- [Linux releases](release.md): branches, channels, assets and the acceptance boundary.
- [Release runbook](release-runbook.md): the agent procedure for releasing a version.
- [Package repositories](package-repositories.md): the signed apt and dnf repositories at pkg.put.gd, the signer and its keys.
