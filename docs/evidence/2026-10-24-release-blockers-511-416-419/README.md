# Remaining AppImage findings and distribution decisions

This supplements the existing [#511 dispositions](../../../packaging/sbom/dispositions/511.json),
code PR #538 and documentation PR #539. It adds no policy ignore, waiver,
component upgrade or release acceptance. The original scan stays intact.

## Exact package and scope

The assessed development AppImage was built by
[CI run 40045151634](https://github.com/patrick-hudson/put.gd-desktop/actions/runs/40045151634)
from clean commit `50053b01f1bbd7f3776a53513bb8c43151ce0de8`, tree
`add0e9919319fed3a465838a4ea34d5de56938ea`, version
`26.1024.1133-dev+g50053b01f1bb`. Its SHA-256 is
`cea2f5f6fe98b7782c1eae090e4a0398b620989052a3d340450474f5eb9af426`;
size 116,472,312 bytes. This is the same package the original dispositions
assessed, not a later stable draft. `assessment.json` retains the exact relevant
file hashes, pinned source, source digests and arithmetic checks.

## libtiff CVE-2026-4775: fixed source already shipped

The scanner's missing fixed-version metadata is not evidence that the pinned
source lacks the fix. The vulnerability concerns overflowing pointer arithmetic
in `putcontig8bitYCbCr44tile`. Upstream fix
[`782a11d6`](https://gitlab.com/libtiff/libtiff/-/commit/782a11d6b5b61c6dc21e714950a4af5bf89f023c)
widens the operands before multiplication in that function and the related
42/22/12 tile functions. The [4.7.2 release notes](https://libtiff.gitlab.io/libtiff/releases/v4.7.2.html)
record the corresponding issue #787 fix and widened pointer arithmetic.

All four corrected expressions are present in the actual
[Qt imageformats pin](https://github.com/qt/qtimageformats/blob/dc13f2fbff1d579cfb2eb2d9092e3563b4d05b8e/src/3rdparty/libtiff/libtiff/tif_getimage.c).
The receipt identifies that module and embedded libtiff 4.7.2. The image contains
`libqtiff.so`; Qt's TIFF handler calls `TIFFReadRGBAImageOriented`, and put.gd has
generic Qt image-loading paths. This conclusion rests on the corrected source,
not on claiming that TIFF is absent or universally unreachable. No Qt rebuild
or new exception is justified by this finding on these pinned bytes.

## GStreamer CVE-2025-3887: affected library, no current product input path

The [upstream advisory](https://gstreamer.freedesktop.org/security/sa-2025-0001.html)
identifies H.265 slice-header parsing and fixes it in 1.26.1. Upstream commits
[`5463f0e0`](https://gitlab.freedesktop.org/gstreamer/gstreamer/-/commit/5463f0e09768ca90aa8c58357c1f4c645db580db)
and [`bcaab360`](https://gitlab.freedesktop.org/gstreamer/gstreamer/-/commit/bcaab3609805ea10fb3d9ac0c9d947b4c3563948)
bound decoded-picture-buffer size and the total number of long-term references.
The latter explicitly fixes ZDI-CAN-26596.

Both matching Ubuntu binary packages come from the same exact source,
`gst-plugins-bad1.0=1.24.2-1ubuntu4`. The source packet was retrieved through
signed Ubuntu archive metadata on the physical Debian builder. Its original
parser still uses the old bounds, and none of its Debian patches changes that
parser. `gstreamer-distro-source.json` records all packet hashes, parser hash,
the relevant statements and patch inventory. The bundled
`libgstvideoparsersbad.so` and `libgstcodecparsers-1.0.so.0` contain the affected
functionality. The library is not fixed, and the vulnerable component is not
omitted.

The product's only `gst_parse_launch` sites build explicit recording and encoder
test graphs. Recording takes raw X11/PipeWire frames through an allowlisted
H.264 encoder and `h264parse`, and raw audio through AAC and `aacparse`.
The test graph likewise uses H.264. `MediaProbe` inspects named factories and
constructs the WebP encoder to inspect properties; it does not parse a media stream.
H.265 MP4 export and working-clip preview/decoding use libavcodec/libavformat
in `ClipExporter.cpp`, not GStreamer's H.265 parser. There is no product
`h265parse`, generic `decodebin`/`playbin` or `QMediaPlayer` input route in this
commit. `gstreamer-entry-points.txt` and the source hashes preserve this review's
scope; this is static call-path evidence, not an exploit test or a proof about
arbitrary third-party plugin hosts.

Disposition: retain the raw finding and existing policy. No current product
input path to the affected parser was identified, so this assessment does not
justify a whole-stack upgrade or a new ignore. Reassess before introducing H.265
parsing, generic playback/autoplugging or a new media import route, and prefer
the distro/upstream fixed package when available. A separately invoked plugin
host can exercise the bundled vulnerable code; these libraries must not be
described as security-fixed.

The exact stable draft still requires a current scan under the unchanged policy,
current asset/source identities and its native desktop, functional and memory
gates. The older scanner summaries' “no fixed version” wording describes their
metadata at the time; the source assessment above supersedes that conclusion
for this libtiff pin without rewriting the historical scan.

## Complete source assets (#416 / #541)

[Code PR #541](https://github.com/patrick-hudson/put.gd-desktop/pull/541) replaces
the media-only asset with automated complete bundled-component source delivery.
It validates coverage against every component in the exact image receipt, all
Qt module pins and the runtime's static-source lock. put.gd and Proton remain
separately verified source assets on the same release. The curated application
archive includes the AppImage build scripts, packaging inputs and their local
Python import dependencies; private work, credentials and lab evidence remain
excluded by the existing committed-source selection.

The full collector ran on physical Debian 13 (`patrick@192.168.68.69`, hostname
`debian`) inside Ubuntu 24.04, after Fedora became unreachable. Container setup
installed standard distro tools as root; collection itself ran as UID 1000.
Private collector commit `2815dcbe18da4321255dfa24c5129896b00645d3` produced and
verified the complete sources for the exact development AppImage above:

- 137 inventory components, with put.gd and Proton explicitly mapped to their
  separate release source assets.
- 94 distinct exact Ubuntu source packages, recursive upstream Git trees,
  six static runtime source archives and four Alpine packaging recipes.
- 364 checksum-listed files in the source catalog.
- Archive size 658,780,913 bytes; SHA-256
  `77de399818e852aad3e897a19096211d4311220617efa15182eb6bea6553b1d2`.

The full archive remains at
`~/work/pgsrc-541-d69-20261024/output-2815/` on Debian. `source-proof/` retains
its source receipt, complete internal catalog, unprivileged verification and
the live mimalloc checkout proof. Source collection never executes downloaded
sources. The disposable container used `--rm` and finished; no desktop or VM
acceptance is claimed.

Before the single branch update, final commit
`49966f830d04ff1ec99d5c171b43f5c6652c010d` passed all 35 release contract suites
on Debian. The nine complete-source tests execute under CI's direct-file runner,
and the curated source test checks the real assembly script's local import
closure. A real curated source export includes those inputs, and the full
658,780,913-byte archive was verified again on the final commit. The collector
code is unchanged from the collection commit. `verification-final.json`,
`curated-source-final-proof.json` and `release-contracts.json` preserve those
results in `source-proof/`; they do not relabel the earlier collection run.
`appimage-build.json.gz` preserves the full original image receipt byte for byte
after decompression; its uncompressed SHA-256 is in `assessment.json`.

Reproduce collection from the final code commit in an Ubuntu 24.04 environment
with standard Git, Python 3.11+, dpkg-dev, Ubuntu's archive keyring and signed
`noble`, `noble-updates`, `noble-security` source indexes. Use a fresh private
output directory and ample temporary disk space; the sources are substantially
larger unpacked. The original receipt's source commit time is 1792801711:

```sh
gzip -dc /path/to/source-proof/appimage-build.json.gz > work/appimage-build.json
python3 tools/releases/appimage_complete_sources.py \
  --receipt work/appimage-build.json --output-dir work/complete-source-repro \
  --cache work/complete-source-cache --timestamp 1792801711
python3 tools/ci/contracts.py release --output work/complete-source-contracts
```

The normal AppImage build script invokes this collection automatically; the
release collector calls `appimage_complete_sources.verify` before staging.

The first real collection attempt failed because Ubuntu had retired the exact
AppArmor source version `4.0.1really4.0.1-0ubuntu0.24.04.8`. The corrected run
used signed Ubuntu snapshot `20261003T232432Z`, chosen from that exact version's
Launchpad publication interval. Historical apt configuration, indexes and
caches are private to the unprivileged collector; signature checks stay on,
and no newer source version substitutes for the bundled version. Original
failure logs/cache remain on Fedora under `~/work/pgsrc-20261024/`; corrected
logs/cache and source packets remain on Debian under the path above.

The previous public #541 CI failed because mimalloc's annotated release tag
object was recorded as a commit. The pin now names actual commit
`8c532c32c3c96e5ba1f2283e032f69ead8add00f`; the existing exact-checkout guard
is unchanged. A real checkout verified commit object type, version 2.1.7 and
the upstream MIT notice hash before the branch update. The full source
collector and its final branch verification are separate from the still
required CI AppImage assembly and from exact stable-asset acceptance.

## Codec distribution decision (#419)

Retain bundled software H.264, H.265 encoding and AAC for the free direct
AppImage distribution, preserving the previously approved recording fallback
and export behavior. [The release policy](../../release.md#codec-distribution-decision-419)
records the delegated choice, AAC explicitly included, primary licensing
references and the distinction between software-source compliance and patent
clearance. No agreement, payment or universal patent clearance is asserted.
