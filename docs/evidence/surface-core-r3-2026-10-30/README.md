# Surface driver round 3 fixtures and observations

Code: PR #855 into `feat/surface-driver-core`. Cleanup tooling: PR #846 into `nightly`. These are Boat development results, not physical desktop or release acceptance. No issue is closed and no lane branch was pushed or merged remotely.

Seven recipe lanes were merged only on the local replay branch: Settings, Destinations, Events, Review, Editor, Tray and Dialogs. The exact lane heads, controller archives and clean application source are recorded in `sources.json`; package and installed-binary hashes are in `builds.json`. The ordinary app under test was `88928c0b59f3938098067e88913bc077b1e46d89`, version `26.1030.2582-dev+g88928c0b59f3`, built separately for Mint's Qt 6.4.2 environment and Ubuntu 26.04. The signed update probe intentionally used genuine tagged AppImages `26.1026.1416-nightly` → `26.1029.2229-nightly`; it never relabelled the development package.

The two permitted snapshots were used: `putgd-lab-cinnamon-v3` (`bx_42saka4g`, run `0d66503c-ff03-4e8f-be4c-a72f4a140136`) and `putgd-lab-ubuntu-gnome-v4` (`bx_wzh5dxjk`, run `d925aa26-3ddd-4f49-818f-7f36cc58b963`). The GNOME compositor witness was staged per overlay and the installed app/witness were verified active. Both desktops remained visible on the wall during GUI work. Native frames, failed attempts, package identities, socket/capacity preflights and bootstrap/reboot evidence are retained. The same-source Qt 6.4 catalog was used as a widget reference on GNOME too; it is not GNOME visual acceptance.

## Raw row counts

Each cell is **passed / failed / blocked**, before → after. These are unchanged raw functional verdicts. They do not promote visually unreviewed rows. The compared sets contain 74 Cinnamon and 55 GNOME rows; the Tray sets are the original #670/#808/#809/#811 requests (42 Cinnamon, 23 GNOME). The four requested #671 variants are included separately for each guest. Variant keys include both recipe and interaction, so the two chooser backends cannot overwrite each other.

| Lane | Cinnamon X11 | GNOME Wayland |
| --- | --- | --- |
| Settings | 0/0/18 → 0/0/18 | 0/0/18 → 0/0/18 |
| Destinations | 0/0/4 → 0/0/4 | 0/0/4 → 0/0/4 |
| Events | 0/0/2 → 0/0/2 | 0/0/2 → 0/0/2 |
| Review | 0/2/0 → 1/0/1 | 0/2/0 → 0/2/0 |
| Editor | 0/0/2 → 0/2/0 | 0/0/2 → 1/0/1 |
| Tray | 0/2/40 → 0/3/39 | 0/2/21 → 0/2/21 |
| Dialogs | 0/0/4 → 0/0/4 | 0/0/4 → 0/0/4 |

`counts.json` preserves every compared receipt hash, transition and exact after-reason. Many Settings/Events/Tray rows are explicit blocked recipe declarations: new fixtures cannot silently turn those declarations into passes. The old destination fixture remains cached for compatibility; its explicit-unlock recipe must adopt `destinations-uncached` or `uncached=True`.

Cinnamon's MCP review row now passes captured moving/frozen controls, masked export and decoded output checks. The CLI row reaches those checks, then retains its explicit capture-lifecycle blocker. Its decoded moving/frozen metrics are approximately 47.68 / 0.000033; the MCP metrics are 47.64 / 0.0. GNOME's recording preparation still does not reach ready; its raw failures remain and no consent cause is inferred.

Both editor selectors resolve on actual Cinnamon Qt 6.4.2 in the focused probe and change the observed style (`lineWidth=2`, recent color `#123456`). The captured recent buttons have role `check box` and no exported objectName. The lane's Cinnamon rows now reach a downstream `IndexError` after the Insert/shape assumption; that is preserved rather than counted as a selector pass. GNOME's palette row passes; line-width remains blocked by a later observation. No application ID issue was needed for #810: the app has the IDs, while Qt 6.4 needs semantic matching.

## Fixture evidence

`fixtures.json` includes all attempts and the latest verdict per fixture, including failures. The latest focused probes have **17 passed / 2 failed** on Cinnamon and **6 passed** on GNOME. These are fixture integration results, not surface-map acceptance.

Provided and observed:

- Known loopback profiles and action-route add/remove; no-output refusal before source acquisition; native configuration write denial with accepted bytes preserved and successful writing after restore; uncached credentials false → production unlock → true; an owned collection lock/unlock without default-alias changes.
- Authenticated loopback update ledger, genuine production signature verification, installation and restart with different PID/start identities. Query/auth/cookie/body/agent privacy observations are booleans; two additional header names are counted without retaining names or values. No claim of a zero-header privacy audit is made.
- Genuine SDK multipart failure, independent server retention listing and refused abort requests, followed by the real 60-second startup automatic cleanup and notification event. The tray row now gets the event but still lacks a uniquely drawn native popup; its original missing-event failure and later drawn-popup failures are retained.
- Native X11 shortcut conflict and removal followed by private-resident re-registration; controlled native GNOME helper unavailability and restoration. The latter reads Shell's actual extension state through its CLI, preserving the separate witness and every other extension.
- Real QApplication/Qt tray fallback without changing desktop identity; real quit/relaunch; two independent active loopback uploads and cancellation; retained local-save failure and retry with the same durable operation identity. Local retry preserves the durable operation state while saving the local item; the superseded assertion expecting `completed` is retained.
- Fresh synthetic recording lifecycle, including prepare/select/start/pause/resume/overlay/stop and private review on Cinnamon; normal distro GTK sink and ffmpeg/ffprobe prerequisites. Consent and media implementation files were not changed.
- Real portal and forced-fallback chooser state with authenticated resident identities; no bus replacement or shared portal termination. Current native GNOME SNI enumeration succeeds with pinned owner/PID/start, skipping the real `bus@/path` watcher entries correctly.

All **664 surface tests pass** on the final controller head on Boat. The exact real notification and GNOME watcher captures are unit fixtures. One earlier whole-suite run exited with SIGSEGV after 449 completed cases and no Python traceback; its raw log and collection order are retained. Two later verbose runs passed all 664. No fault site is established, and no signal/release code was edited. The separate cleanup suite passes 31 tests.

## Remaining boundaries

- #681/#862: the shortcut status is visibly rendered but Qt 6.4 exports neither current text nor description. The new observer blocks precisely on #862. Input overlap/AltGr fixtures use the input owner's held-key guards; full live chord/focus assertions await the accessible status. The external focus/hide fixture is supplied, but the failed recording-status prerequisites are not claimed as delivered deactivation tests.
- #671: a production chooser `Failed` result cannot be induced safely by the current public boundary; portal Response 2 deliberately falls back. The two cross-backend recipes also explicitly block contradictory backend states. Open/save/folder reference catalogs remain absent. The fixture exposes real backend state, but does not fabricate those four row outcomes. GNOME proxy placement / fresh AT-SPI failures remain #842/#805.
- #666/#726: credential entry/reveal still needs the separate protected input/frame transport. No live S3, B2 or Proton was enabled; B2 stays a configuration-only template.
- #682/#670: protected lock/unlock, logout/re-login and ended-session replacement, controlled pending/unsupported desktop channels, a native other-client keyboard-grab witness, stable scheduled-update/browser/software-centre workflows, Proton busy, and delayed outstanding Notify replies are not proved here. Existing explicit blockers remain. The separate #817 consent work was not touched.
- Native notification/panel ownership and GNOME AT-SPI timeouts still stop several rows. SNI metadata or a bus notification is not proof that the desktop drew the surface. The GNOME uploading failure also retains its owned-directory cleanup refusal; disposable-guest shutdown is the outer boundary.

## Retirement and restore

Both custom completion manifests were independently verified before cleanup. Cinnamon: dry run **20.43 s**, actual retirement **5.38 s**; GNOME: **50.00 s / 4.34 s**. Both native cleanup receipts are completed, with zero owned running QEMU processes and no overlay disks remaining. Both read-only golden hashes match the hashes recorded before launch. All guards remained enabled. The original intermittent #844 timeout was not reproduced; the code identifies its sole subprocess timeout source (qemu-img backing-chain inspection), gives it a bounded 180-second budget and reports phase/elapsed time on refusal. This does not establish the original latency cause.

The Boat control API returned HTTP 525 at this worker’s polls at 10:13, 10:19 and 10:24Z; pinned SSH continued to work. The limits endpoint recovered at 10:29:49Z. No sandbox was created or attempted during the outage. Cinnamon deletion `bdop_d2bb7ad9d35941cdbd503b1abc120c3a` completed at 10:45:53Z; GNOME deletion `bdop_36316aa16bf64561a3a5000093c0567c` completed at 10:47:20Z. Both IDs subsequently returned not-found. No sandbox is retained. The two owned wall entries were removed while preserving seven other entries; the exact owned tunnel processes were already gone, and the private agent was stopped.

Raw archives retain exact packages, raw PPM/PNG frames, receipts, all superseded probes, command sources and logs. They were streamed to the workstation and every manifest member was independently hash-verified before guarded overlay retirement. Duplicate raw PPM pixels remain losslessly in the archive without expanding them on the workstation. `restore.json` lists archive paths, checksums, member counts and extracted evidence locations. Protected login inputs, SSH identities, generated credential files and VM disks are excluded.
