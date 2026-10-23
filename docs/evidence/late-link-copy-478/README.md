# #478: each_link copies a link that arrives after its file settled

Evidence for [PR #479](https://github.com/patrick-hudson/put.gd-desktop/pull/479) (fix) and
[issue #478](https://github.com/patrick-hudson/put.gd-desktop/issues/478). The lab host is
`f44`, run `~/work/put.gd/work/lab-runs/aurora-i478` (an overlay of the Aurora 44 v3 golden
rebased with `bootc switch ghcr.io/ublue-os/aurora:testing` to `testing-45.20261022.1`:
Fedora 45, KWin 6.7.5-1.fc45, plasma-workspace 6.7.5-2.fc45, qt6-qtwayland 6.11.2, kernel
7.2.11), shown on the wall as "Aurora KDE VM aurora-i478 (f44) - #478 link copy".
Raw frames, the guest `putgd.*` log and `results-i478.json` are under that run's `evidence/`.

| Picture | What it shows |
| --- | --- |
| `478-before-aurora-f45-proton-notification.png` | nightly 26.1022.996 (1887c4c9), Proton Drive, `each_link`: "Uploaded. Click to open in browser." only; `copyStatus: no_result`, no error, clipboard unchanged (lab run `aurora-kc45`, 2 of 2) |
| `478-after-aurora-f45-proton-notification.png` | PR build `26.1022.1021-dev+g26303cce` (CI AppImage, SHA-256 `77544aa5…`), same scenario: "Uploaded. Link copied to the clipboard. Click to open in browser."; `copyStatus: copied` (ops `90b24aa6`, `ac4b0b6a`, 2 of 2) |
| `478-after-aurora-f45-proton-paste-kwrite.png` | Ctrl+V sent through QEMU into KWrite pastes the copied `drive.proton.me` link; the saved file equals the link |
| `478-after-aurora-f45-s3-notification.png` | S3 (unchanged path) still `copied` with the same notification (ops `acb60ca9`, `873d9d63`); the S3 link was pasted into KWrite the same way |

Synthetic proof is in `tests/unit/test_operations.cpp` (`linkThatFollowsTheTransferIsCopied`,
`earlierFileLinkFromSharingStillCopiesFirst`): both fail on `origin/nightly` 5ba9de0e with the
reported `no_result` and pass with the fix; `operations_contract` passed on d69.
