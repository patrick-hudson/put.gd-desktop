# History previews: WebP plugin and posters on selection (#189, #190)

Pictures for PR #222. Every frame was taken on m74 (Linux Mint 22.3) on a
private Xvfb display `:89` that held only the test's own windows: the put.gd
History window, grabbed by its X window ID, and a GStreamer `videotestsrc`
ball as the recorded motion. The profile was private (`XDG_*` under
`/tmp/pg189`, its own session bus); the physical Cinnamon desktop was not used.

| Picture | What it shows |
| --- | --- |
| `history-pre112-video-poster-190.png` | An MP4 recorded by nightly 588 (before #112) has no poster in 588; with the #222 ubuntu24.04 DEB, selecting it makes the poster from the retained file (inspector and list) |
| `history-webp-plugin-189.png` | An animated WebP recorded by 588 without Qt's WebP plugin has no preview; with the #222 DEB and the `qt6-image-formats-plugins` its `Depends` pulls in, selecting it previews the first frame |
| `history-webp-failsoft-189.png` | The #222 build before the plugin was installed: "Preview unavailable", no error banner; the resident logged `No preview` (`preview_failed`) and answered a repeat from memory |
| `deb-depends-webp-plugin-189.png` | `apt-get install -s` of the #222 DEB on m74 pulls `qt6-image-formats-plugins` and `libmng2` |

Builds: nightly 588 (`putgd-0.1.1-nightly.588-ubuntu24.04-x86_64.deb`,
SHA-256 `78ac5bd0…e626a`, commit 9c035bf) and the #222 CI package
`putgd_0.1.1~dev.660+g22c941e721b5_amd64.deb` (CI run 39452070045, head
13c5433 merged into nightly), both run extracted from the DEB, not installed.
The plugin packages were installed for the "after" frames and purged afterwards.
