# README image sources

The README shows the application icon straight from `packaging/linux/putgd.svg`, so it
always matches the app. Everything else it shows is in `shots/`.

## Screenshots

`shots/` holds six screenshots copied unchanged from the put.gd website
([put.gd-site](https://github.com/patrick-hudson/put.gd-site) `public/_site/shots/`, commit
`5e3aca5`, the `@2x` WebP files). They are real windows of put.gd 26.1111.4633 nightly (commit
`4f80bcfffc047087d69a5e469a08d9f6397249f3`) on GNOME 50.6 (Wayland), Fedora Linux 44, taken by
the site's `tools/shots/` and recorded in its `shots.json`. Each comes in a dark and a light
version, and the README picks one with `<picture>` and `prefers-color-scheme`, as GitHub's
theme does. Uploads in them went to `demo.put.gd`, the site's demo destination.

| README file | Site file | What it shows |
| --- | --- | --- |
| `desktop-dark.webp`, `desktop-light.webp` | `desktop-*@2x.webp` (3840 × 2160) | History on the Fedora GNOME desktop |
| `edit-dark.webp`, `edit-light.webp` | `edit-*@2x.webp` (3218 × 2040) | The editor with a box, steps, an arrow, a label, a highlight and a redaction on the Apollo 11 plaque |
| `record-bar-dark.webp`, `record-bar-light.webp` | `record-bar-*@2x.webp` (2556 × 1494) | A region over a playing video and the recording bar |

To refresh them, copy the same files from a newer put.gd-site commit and update the version
and commit above and the credit line at the end of the README.

Demo media, credited at the end of the README as on the website:
[Sintel](https://commons.wikimedia.org/wiki/File:Sintel_trailer-1080p.ogv) © copyright
Blender Foundation | durian.blender.org, [CC BY 3.0](https://creativecommons.org/licenses/by/3.0/)
(the recording bar); NASA photos, public domain (the Apollo 11 lunar plaque `S69-38749` in the
editor, and the History thumbnails).

## Image checksums

- `desktop-dark.webp`: `b42eed199e4c4974762d9fd2d5072d22ba3fec6a15a671258650b8ec60a86ab8`
- `desktop-light.webp`: `9d1766826caf852d48cf77a6ffc33e2b274ea069ba3e8097e401adefdebfdcb2`
- `edit-dark.webp`: `7953fcb69170d495323b147257be813004e45aac149e5aaee9aad70b3be1366b`
- `edit-light.webp`: `fea2fd04300cdde30cbffafc483a9e302a373fbb1301c7a2a0d8a432257a4727`
- `record-bar-dark.webp`: `2214c1aeb96e7ae2dda3b3f8ca5122045654f8ec5914c6ea0f40aa9393965bbc`
- `record-bar-light.webp`: `2ff44364dbca9f74addccf25b7641ded0204ce54f3063578bc9cc68ae2606d57`

Everything under `docs/readme/` exists only for the repository README. Nothing here is
packaged, exported in the curated source archive or used by a test. The README claims no
acceptance result from these images; acceptance evidence lives in
[put.gd-evidence](https://github.com/patrick-hudson/put.gd-evidence) and is recorded in
`docs/testing/verification.md`.
