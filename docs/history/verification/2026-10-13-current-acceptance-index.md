# Verification record

> **Historical record.** This is a dated run record, moved unchanged from [implementation evidence](../../testing/verification.md) on 2026-11-05 (#1222). It describes what that run found for the build it names, and its open or pending statements describe that time. "Above" and "below" refer to its neighbours on the old page, in the order of the [history index](../README.md#verification-run-records). For today, see the [current state](../../testing/verification.md#current-state). Do not use it as a work list.

## Current acceptance index — 2026-10-13

The historical source108 GNOME/KDE TLS failures were superseded by the passing
[historical transport retest](../testing-live-s3-transport.md). Keep their failure
receipts as diagnostic history; they are not current release blockers.

| Scope | Latest linked evidence | Limit |
| --- | --- | --- |
| Build and automated contracts | [Main b1ed4ae Build run](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/38438946727), all four jobs passed | The retired cross-platform jobs compiled shared code only; CI is not compositor proof |
| Cold first capture (#3) | [Unable-to-reproduce closure](https://github.com/patrick-hudson/put.gd-desktop/issues/3#issuecomment-6149342985): b75c0dd, 3/3 cold runs each on GNOME/KDE, exact image/native-paste comparisons | Does not prove the S3 fix caused the historical timeout to disappear or rule out a rare race |
| Live S3 and retained memory (#4) | [Closure and exact scope](https://github.com/patrick-hudson/put.gd-desktop/issues/4#issuecomment-6152173663): the historical full workload plus bounded b1ed4ae-tree Cinnamon/GNOME/KDE checks | The newer check covers region/text publication, restart/history/deletion and two idle windows per desktop; it is not a new full geometry matrix |
| Release and fresh-agent acceptance (#5) | [Dated audit](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-13-issue5-acceptance.md): clean SDK/dev/release builds, production CLI/MCP/live S3, Cinnamon upgrade with 10,000 records | Initial unexplained auto-copy suppression blocks sibling expansion; physical hardware and full geometry/performance acceptance remain separate gates. Remaining findings and acceptance work: [#24](https://github.com/patrick-hudson/put.gd-desktop/issues/24) |
