# Suppressed automatic link copy, before and after #477

Pictures for [#473](https://github.com/patrick-hudson/put.gd-desktop/issues/473) and
[#477](https://github.com/patrick-hudson/put.gd-desktop/pull/477): what a user sees
when another app copies something while put.gd is still uploading, so put.gd
leaves the clipboard alone (`copyStatus` `suppressed`).

## Environment

| Item | Value |
| --- | --- |
| Guest | Aurora `testing-45.20261022.1` (Fedora 45, `plasma-workspace-6.7.5`, `kwin-6.7.5`, Plasma Wayland), golden `aurora-testing-45.20261022.1-852d34e885c1`, overlay run `aurora-d473` on `f44`, profile `aurora-kde-wayland`, 1280×800 |
| Before | `putgd-26.1022.1019-nightly-x86_64.AppImage` (release `v26.1022.1019-nightly`, commit `5ba9de0e`), SHA-256 `e5b7ad2caecaa7f5737bdfb5d2f0fd4e3c74ce5d98a8a68142caf4efd43525bc` |
| After | `putgd-26.1023.1040-dev+gb247eaf0f111-x86_64.AppImage` (CI run 39889172448, artifact `release-appimage`, the `nightly` merge commit `b247eaf0` of #477), SHA-256 `6cb98eb2c02561f688363bca6f59ae908b9c361fb138deab58e190446ed6d848` |
| Destination | Live S3 lab prefix `acceptance/d473/{before,after}/`, region capture default output (upload, share), `uploadClipboardBehavior` `each_link` |
| Date | 2026-10-23 (UTC) |

Both builds ran in the same guest, one after the other, with the same settings.

## Steps (identical for both builds)

1. Throttle the guest's HTTPS uplink (`nft`, 6 KB/s for the lab user) so the
   upload of a 219 KiB region capture takes about 40 s.
2. Invoke **Capture region** and drag a region.
3. Ten seconds later, another app copies text: `wl-copy "Notes copied from another
   app during the upload"`.
4. Wait for the operation; take the frames of the completion notification.
5. Open Activity (`ui.show` `activity`), take the list, select the newest row,
   take the inspector.

Both operations ended `completed` with `copyStatus` `suppressed` and no error,
and the clipboard still held the other app's text: put.gd behaved the same, by
design. Only what it tells the user and support changed.

| Build | Operation | Result |
| --- | --- | --- |
| before | `ecbdb3f7-0b5c-4a9e-bdba-1313ccf93896` | `suppressed`, notification "Uploaded. Click to open in browser.", Activity "Uploaded", no log record |
| after | `cdbe20c1-d2e9-497f-9c97-8f4158a7a2b2` | `suppressed`, notification and Activity name the clipboard change, one `putgd.clipboard` record |

After step 5 on the after build, the inspector's **Copy link** was clicked:
`wl-paste` then returned the uploaded object's link (with put.gd's
`application/x-putgd-result-*` token type) instead of the other app's text.

## Pictures

| File | Shows |
| --- | --- |
| `notification-before.png` | Before: "Uploaded. Click to open in browser." and nothing about the clipboard (crop of the lower right of the desktop) |
| `notification-after.png` | After: "Uploaded. Link not copied: the clipboard changed during the upload. Copy it from Activity. Click to open in browser." |
| `activity-before.png` | Before: the row says "Region · 219 KiB · 1 min ago", the inspector just "Uploaded" |
| `activity-list-after.png` | After: "Link not copied (clipboard changed)" on each row in the theme's warning colour, **Copy link** as the row action. The second row is the before build's upload, which was suppressed too; the new build labels it from its stored status. |
| `activity-inspector-after.png` | After: "Uploaded · Link not copied (clipboard changed). Copy it again." and the **Copy link** button that was then clicked |

Each picture has a caption naming its build. The full-desktop frames come from the
lab VM (QMP screendump), which holds nothing but the lab session. Raw frames stay
in the run directory on `f44`:

| Raw frame | SHA-256 |
| --- | --- |
| `before-notify-1-head0.ppm` | `2d04a59741f9c197b00599bdfda73cea0ffa1b0f3be28fd1bb5df7cbd1c915c6` |
| `before-activity-selected-head0.ppm` | `5af7ece5a012d0a21acc22229ff339eaa7274d57286c411ae67db8ed68faa250` |
| `after-notify-1-head0.ppm` | `1a9616b13146449072f2a2494e77854a1f097c9e804931d28c950b2f2267a485` |
| `after-activity-head0.ppm` | `349b2602bf14b4f93f81a7aad6ef19d4655455bff3727cc8aa9bb8b6fe49767c` |
| `after-activity-selected-head0.ppm` | `8f7017304575a5e06d05e385d514ec8f3652bf027b51e5f6e00aa5759930fcb0` |

## Support-side evidence

- `after-clipboard-log-record.jsonl`: the after build's `Automatic copy finished`
  record (`putgd.clipboard`). `suppression.guard` is `clipboard_changed`, with
  `revisionAtInvocation` 1 and `revisionAtDispatch` 2; the epochs match. It holds
  neither the link nor the other app's text. The before build wrote no clipboard
  record for its operation.
- `after-bundle-preview.json`: `diagnostics.bundle` with `preview` and `since` `1h`
  on the after build. Both completed uploads are in `operationList`; before #477 a
  window package carried only failed, partial and interrupted operations.
