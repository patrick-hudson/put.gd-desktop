# Native storage activity tray checks — issue #33

Candidate **`061c2d9c569dc10b5c8681a7ad7beb3093e61b80`**, version
`0.1.1-dev.176+g061c2d9c569d`, was frozen before testing. Its tracked archive,
Git tree/count, per-distro package and binary hashes, process identities, commands,
versions, samples and selected console images are in the
[receipt](2026-10-14-storage-tray-native/receipt.json) and its
[file manifest](2026-10-14-storage-tray-native/manifest.json).
The product code did not change during the VM wave. Documentation commits do not
rename these measured binaries.

All bounded functional, visual and guest memory checks below passed.

| Desktop | Native cases | View opens/closes | Rendered color checks | Two post-use RSS peaks |
| --- | ---: | ---: | ---: | --- |
| Cinnamon 6.6.4 / X11 / Qt 6.4.2 | 12 | 6 | 5 | 29.96 / 29.94 MiB |
| GNOME 50.1 / Wayland / Qt 6.10.2 | 12 | 6 | 5 | 33.98 / 33.98 MiB |
| Plasma 6.6.6 / Wayland / Qt 6.10.2 | 12 | 6 | 5 | 31.05 / 31.05 MiB |

Cinnamon functional/visual checks and both memory windows passed before either
Wayland build started. Its package was built on Mint 22.3 with distro Qt 6.4.2,
GCC 13.3 and the pinned official AWS SDK 1.11.907. GNOME and KDE were built
independently inside fresh Ubuntu/Kubuntu 26.04.1 overlays with their distro Qt
6.10.2 and the same pinned SDK. Their development suites passed **60/60** and
**59/59**, respectively; GNOME's extra GJS ownership test is mocked and is not
native proof. After building, all 5,998 tracked source files in each archive build
still matched the frozen archive. Supplied archive provenance has `dirty: null`;
it is not represented as a Git-clean checkout.

The native cases cover region, window and desktop capture; text, image and ordered
files through independent Qt and GTK clipboard clients; and Escape, right-click
and zero-area region cancellation. They compare actual saved synthetic bytes or
pixels and independent native paste. Readiness verifies the actual compositor and
native Qt Wayland backend where applicable. All runs used KVM, one 1280×800 head,
and visible localhost VNC on the display wall. Golden/backing disks were not booted
or modified; test writes used `i33c1`, `i33g1`, `i33k1` and `i33k2` overlays.

Settings, History and Activity each opened and closed twice per desktop. Explicit
visible title-bar focus precedes Alt-F4 on Wayland; a CLI acknowledgement alone
does not establish foreground focus. The UI helper exited after every accepted
last-window close. These checks do not establish all draft/Cancel or full-ID GUI
flows.

Rendered tray checks prove blue during region selection and cancellation, green
during held HEAD/PUT storage requests, a temporary red/blue error flash with
notifications disabled, recovery to green while another upload is still running,
and a local-archive failure flash followed by blue. They use the production app
and official SDK against the isolated, signed offline S3 wire oracle. All payloads,
credentials and objects are synthetic; no user credential file or cloud object was
used. This is compositor evidence, not live-cloud or Proton-account acceptance.

The pixel oracle uses exact RGB on Cinnamon. The inspected Wayland panels resample
and blend small icons: their oracle fits the three declared colors against the
observed background within the visibly identified tray rectangle, with alpha at
least 0.35 and maximum RGB-channel residual 3. Unrelated blue panel/desktop icons
are excluded. Complete frame transitions remain available. Screenshots prove
visible flashing/recovery; they do not certify every 300 ms repaint. Precise timer
and latest-error restart behavior also have automated contracts.

Memory uses the unchanged `bench_ui_process.py`, SHA256
`a28c94b075036ea65f8eca8e068d480cff46ee959d4680593f5c4d55cce3fc40`, observing
an explicitly identified existing resident and every observed descendant. Each
desktop has two serial 300-second windows sampled every five seconds. Compilation
and graphical interaction finished before these observations. Commands, start
ticks, binary hashes, raw samples and evaluated gates are retained. The gate is
at most 40 MiB RSS, no helpers/escaped children or swap, and CPU below 0.5% of one
core. These are post-use VM observations; they do not certify cold physical memory,
physical startup latency, mixed displays or the complete release matrix.

Failed attempts remain under the private `work/issue33` tree and are described in
the receipt. They include missing GTK4 packages on fresh Cinnamon, overlapping
focus drivers, the recorder holding QMP exclusively, a transient classifier edit,
an inappropriate frame-count assertion, and incorrect exact-RGB/global-panel
matching on Wayland. Corrected runs preserve their original failures and did not
change the candidate or frozen memory observer.

The first KDE region attempt encountered SSH exit 255 with selection still
outstanding; it later recorded `capture_timeout`. Its same-timing repeat passed.
A second fresh overlay independently passed its first region gesture, six view
opens/closes and all five color checks using the identical package. Its memory
windows are the KDE entries above. The original interruption and timeout are
retained; this is not a causal fix or a claim that a rare startup race is impossible.
