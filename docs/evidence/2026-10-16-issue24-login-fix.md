# Cinnamon login startup fix — issue #24

The fixed candidate passed two real Cinnamon logout/login cycles with valid preserved settings: one newly autostarted resident, a registered tray, and no put.gd window or UI helper. The [structured receipt](2026-10-16-issue24-login-fix.json) records exact source, assets, process identities, native cases, memory results and retirement hashes. Broader acceptance remains [#44](https://github.com/patrick-hudson/put.gd-desktop/issues/44).

## Candidate and behavior

Frozen implementation commit: `09ce25b7851624e602c96dba855b4465e688fec9`; tree `5e0fc4ff4e89dc1dc9ec0f259a89f9e4f7516a2e`; tracked-source archive SHA-256 `48e1de5d9a43ee2a677aeeed5feeb7f77c4e43f033bd1ba2b0f75e6c0a96eba6`. All tested production packages identify this candidate. Later evidence-only commits do not relabel the binaries.

Cinnamon can start the configured `--background` resident before its StatusNotifier watcher or host is available. Main previously opened Settings on that initial absence. A precise one-shot timer now gives startup ten seconds, then checks tray availability before showing recovery. An explicit tray or IPC window request cancels that pending recovery. Explicit launches and missing/invalid settings still open immediately. A tray arriving after the ten-second ceiling leaves the recovery view visible; a regression pins this deliberate limit.

## Regression and host checks

The test launches the actual resident, CLI and UI helper under private D-Bus/Xvfb with isolated accepted settings. The initial late-watcher, late-host and absent-tray assertions failed before the production change. The final nine rows cover those cases, a ready tray, a tray arriving after the recovery ceiling, explicit activation cancellation, missing/invalid configuration, and explicit startup. Quiet rows are observed past the deadline after resident readiness, including transient-helper exit metadata. The explicit-activation row terminates only its verified positive helper PID and checks that recovery does not reopen it.

Final `cmake --preset dev` / `cmake --build --preset dev` and production Release builds passed with dynamic distro Qt 6.4.2 and the existing pinned official AWS SDK. Complete `ctest --preset dev`: **59/59**, 643.43 seconds. Affected ASan/UBSan targets built; tray, background startup and login contract checks: **3/3**, 85.82 seconds, including all nine startup rows. Private wire peers prove process/widget contracts, not compositor support.

## Real Cinnamon logins

Fresh overlay `i24c1`, run ID `473964e8-e303-4b2f-8d59-460d3573c34b`, Mint 22.3, Cinnamon 6.6.4/X11, distro Qt 6.4.2. Each cycle stopped every prior put.gd process, invoked real Cinnamon logout, and entered the existing protected local input through the visibly verified masked console field. Both unlocked desktops were verified visibly through VNC. Configuration SHA-256 stayed `b4f6686c3018d36579e98704854a4592ed1cf8cebd81769bc7e707f7bdf006d9` across both logins.

| Login | New resident PID | Start ticks | UI helper/window | Tray |
| --- | ---: | ---: | --- | --- |
| 1 | 4123 | 79059 | None | Registered |
| 2 | 6243 | 84910 | None | Registered |

Each cycle observed startup for at least sixteen seconds; a read-only 100 ms process observer spanned both logins. Accepted configuration, managed autostart bytes and installed/running resident hashes matched. Resident SHA-256: `9d724efbcd25feacf09da310b9a9e59f783f34bd90cb1071d556c410c4751f64`. Cinnamon DEB SHA-256: `a81f547bb103ad41ed179d2853b085cafaee16947a2f630a59f8fdbd262e75ac`. This proves actual console autostart; neither an SSH launch nor a surviving process is credited as login proof.

![First real login, no application window](issue24-login/login1.png)
![Second real login, no application window](issue24-login/login2.png)

## Required native regression gates

Cinnamon's functional and both memory windows passed before the GNOME/KDE package build or native runs. The shared Ubuntu 26 build uses its distro Qt 6.10.2 and the same frozen source archive. Each profile passed region/window/desktop capture; Qt and GTK clipboard text/image/files with independent paste; Escape/right-click/zero-area cancellation; two Settings/History/Activity reopen cycles with normal final-close helper exit; and graceful resident restart preserving accepted settings and exact history.

| Native profile | Qt | Action/paste cases | Peak RSS MiB, two post-restart 300-second windows |
| --- | --- | --- | --- |
| cinnamon-x11 | 6.4.2 | 12/12 | 25.42, 25.42 |
| gnome-wayland | 6.10.2 | 12/12 | 28.12, 28.12 |
| kde-wayland | 6.10.2 | 12/12 | 29.67, 29.67 |

Memory sampling followed the graceful restart and native History reopen/close. Capture cases ran on the preceding resident; these samples establish post-restart/post-view idle memory, not retained post-capture memory on that preceding process. Every window used the unchanged observer at five-second intervals for at least 300 seconds and 60 samples. All passed the 40 MiB RSS ceiling, <0.5% of one CPU core, zero process swap, one resident and no observed UI/escaped helpers. Builds and active GUI work finished before sampling. These are post-restart/post-History VM observations; PSS is recorded separately and does not replace RSS.

## Attempts, review and retention

The initial full Dev command ended with status 143 without a terminal CTest result; its partial output was not counted as a suite pass. The final complete run and focused sanitizer run passed. Initial configure/setup attempts and an apt version-order rejection are retained. The final amended development hash sorted below the initial same-count package; explicit guest apt downgrade allowance, then the unmodified lab install/hash checks, established the final candidate before both logins. The initial package is not credited as final native acceptance.

Focused correctness and independent Anthropic review led to the readiness-based test window and explicit-activation cancellation. The final positive-PID test guard was rebuilt and reverified. No outstanding defect was identified; the ten-second recovery ceiling remains explicit.

Private originals remain under `work/i24-fix/`: exact packages and audits, source archives, all attempts, native screenshots/oracles, process trace, test logs/JUnit, raw memory samples, run identities and guarded cleanup manifests. Every disposable overlay was stopped and retired only after verified export; immutable goldens and all host evidence remain. The public receipt contains payload-free summaries and hashes. No credentials or protected login input is included.

The historical local-only `copyStatus=suppressed` event in #24 remains unreproduced with unknown cause. Passing current synthetic paste cases does not identify that event. This fix does not complete the package lifecycle, live backend/GUI/CLI/MCP parity, fresh-agent walkthrough, physical performance, mixed-display or exact-release-asset gates tracked in #44.
