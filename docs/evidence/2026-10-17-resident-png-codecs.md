# Resident PNG readers, and why the libpng writer was dropped — 2026-10-17

The kept change is commit `e9a4dac` on `feat/edit-and-upload` `7937ea6`. Its PNG
readers turn automatic format detection off after checking the signature; see
[resident PNG readers](../image-worker.md#resident-png-readers). This record also
covers a libpng writer that was built, tested and measured, then dropped.

## The writer question

Qt 6.4.2 and 6.8.2 instantiate every installed image plugin for any `QImageWriter`
or `QImage::save`, even one naming `png`. Qt 6.9 fixed this; the qtbase source
diff of `qimagewriter.cpp` and `qimagereader.cpp` between 6.8.2 and 6.9.3 shows the
change. Every capture and image import ends in a PNG encode in the resident, so on
Mint 22.3, Ubuntu 24.04 and Debian 13 the first stored image loads all plugins
whatever the readers do.

Three options were weighed:

- Accept the cost.
- Move the encodes into the image job: +0.1 s per 1080p capture with an offscreen
  child, +0.45-0.7 s with an inherited xcb child.
- Encode with libpng directly.

Patrick first chose libpng. The prototype `encodePng` followed Qt's PNG writer call
for call. Its output was byte-identical to `QImage::save` and `QImageWriter` for 23
formats and cases and four quality levels. It covered artifacts, previews and the
editor's staging copy, and passed the full Mint dev suite (69/69).

## Measurements

| Check | Result |
| --- | --- |
| Core probe, Mint 22.3 workstation, 3 plugins | +1.20 MiB RSS from the plugins |
| Core probe, Debian 13 builder, 4 plugins | +1.46 MiB RSS |
| Core probe, Debian 13 + `kimageformat6-plugins` and `qt6-image-formats-plugins` (32) | +13.37 MiB RSS, 12.04 MiB of it in plugin files |
| Mint 22.3 Cinnamon/X11 VM, 3 alternating pairs | 18/18 scenarios pass; baseline maps 3 plugins + `libjpeg` (220 KiB RSS), candidate none; median RSS 28.48 vs 28.43 MiB |
| Debian 13 physical Plasma/Wayland, 3 alternating native Release pairs | 18/18 pass; baseline maps 4 plugins + `libjpeg`/`libQt6Svg` (436 KiB RSS), candidate none; total RSS hidden by 3.3 MiB anonymous variation |

The A/B candidate was the prototype `ef48f67` (readers plus libpng writer) against
`d11940f`. The probe overstates the cost: it also counts Qt library pages that a
working resident already holds. On real residents the saving was only the plugin
files, 0.2-0.4 MiB. It would reach about 12 MiB only with large plugin sets on
Qt 6.8 or older, such as Debian's KDE set, which `gwenview` and `dolphin` pull in.
That case was probed but not measured in a real resident.

## Decision

After seeing these numbers, Patrick dropped the encoder. About 100 lines of
setjmp-based libpng code and a new `libpng-dev` build dependency were not worth
0.2-0.4 MiB, for a problem Qt 6.9 already fixes. The reader fixes and their tests
were kept. On Qt 6.4.2 and 6.8.2 the resident therefore still maps the image
plugins after its first stored image; on Qt 6.10 and later it maps none.

Retained failed attempts: a main-based first round, an apt downgrade blocked by
Mint's update lock, a guest lock-screen failure, and a first Debian attempt whose
input-agent start held its SSH session. The Cinnamon VM's 1.3 GB overlay is
retained on the Fedora builder because guarded cleanup refused (runner and golden
roots differ). Debian was restored to `95a0f9b` afterwards.

Private evidence, including the prototype commit as
`libpng-encoder-prototype-8173302.patch` (sha256 `424c1e5a…`), probe sources,
package identities, visuals and samples: `work/verification/2026-10-17-png-plugins/`
(`MANIFEST.sha256`
`c17747152dac851d54dc649ac6e5386036db93dd4517b65c4348624d306aa68f`).
