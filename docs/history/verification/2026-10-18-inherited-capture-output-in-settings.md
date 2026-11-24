# Verification record

> **Historical record.** This is a dated run record, moved unchanged from [implementation evidence](../../testing/verification.md) on 2026-11-05 (#1222). It describes what that run found for the build it names, and its open or pending statements describe that time. "Above" and "below" refer to its neighbours on the old page, in the order of the [history index](../README.md#verification-run-records). For today, see the [current state](../../testing/verification.md#current-state). Do not use it as a work list.

## Inherited capture output in Settings — 2026-10-18

The [issue 58 receipt](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-18-issue58-inherited-capture-output.md)
records candidate `48c2b57`: `ctest --preset dev` 72/72, the MCP suite 3/3, and a
native check on the workstation's Cinnamon X11 session with an isolated instance
and synthetic destinations. Inherited per-action cards stayed visible with their
controls disabled, followed Default capture output live, and unchecking saved an
explicit exception; `ui.viewState` and MCP `get_view_state` reported the same
state. GNOME, KDE, Wayland and memory were not tested.
