# Emoji template sources

`templates.bin` holds the pictures `putgd-ocr` names emoji by when it reads text from the
screen ([#1748](https://github.com/patrick-hudson/put.gd-desktop/issues/1748)). It is built
into the helper. No font ships with put.gd: each template is a small bitmap of one emoji
as a font draws it.

Made on 2026-11-20 with [`tools/text/emoji_templates.py`](../../../tools/text/emoji_templates.py)
on d69 (Debian 13, Pango 1.56.3, cairo 1.18.4, PyGObject 3.50.0), from Qt's drawings made on
f44 (Fedora 44, Qt 6.11.2) by [`tools/text/emoji_draw_qt`](../../../tools/text/emoji_draw_qt):

```sh
# f44, with the system's fonts:
tools/text/emoji_templates.py --emoji-test emoji-test.txt --list-sequences |
  putgd-emoji-draw-qt "Noto Color Emoji" 96 > noto-2.057-qt.records
# d69:
tools/text/emoji_templates.py --emoji-test emoji-test.txt \
  --style noto="Noto Color Emoji" --fonts noto=fonts.conf \
  --style twemoji="Twemoji Mozilla" --fonts twemoji=fonts.conf \
  --style noto-2.047="Noto Color Emoji" --fonts noto-2.047=noto-2.047.conf --changed noto-2.047=noto \
  --style noto-2.057="Noto Color Emoji" --fonts noto-2.057=noto-2.057.conf --changed noto-2.057=noto,noto-2.047 \
  --style noto-2.057-qt="Noto Color Emoji" --drawn noto-2.057-qt=noto-2.057-qt.records \
  --changed noto-2.057-qt=noto,noto-2.047,noto-2.057 --output templates.bin
```

`fonts.conf` includes `/etc/fonts/fonts.conf` and adds the directory holding
`TwemojiMozilla.ttf`. `noto-2.047.conf` and `noto-2.057.conf` include `/etc/fonts/fonts.conf`,
reject the system's `NotoColorEmoji.ttf` with `<rejectfont>`, and add a directory holding
that release's file, so every style is drawn with the same system settings. The defaults
give 20 by 20 and 10 by 10 pixel templates.

| Input | Version | SHA-256 |
| --- | --- | --- |
| [`emoji-test.txt`](https://unicode.org/Public/emoji/16.0/emoji-test.txt) | Unicode Emoji 16.0 | `24f0c534e86cf142e2496953e8f0e46a3e702392911eddcd29c6cced85139697` |
| `NotoColorEmoji.ttf` (Debian `fonts-noto-color-emoji` 2.051-0+deb13u1) | 2.051 | `dac5c27651082d6c53dab5081f50d2022ddf6877d730fa919cf6a4fc2af22de0` |
| `NotoColorEmoji.ttf` (Ubuntu 24.04 and Linux Mint 22 `fonts-noto-color-emoji` 2.047-0ubuntu0.24.04.1) | 2.047 | `93cdc4ee9aa40e2afceecc63da0ca05ec7aab4bec991ece51a6b52389f48a477` |
| `Noto-COLRv1.ttf` (Fedora 44 `google-noto-color-emoji-fonts` 20260921-1.fc44, the same file in Fedora 45 and Aurora) | 2.057 | `b8e25ea68db82f9e4d0aee921f4420be2be39887bd5c893a2ad98710531f9d0c` |
| `TwemojiMozilla.ttf` (bundled with Firefox, the same file in Firefox ESR 153 and Firefox 158 and 159) | 0.7.0 | `6d90152ee0d29e82fe2a87793af5aa4b7ad13e6538360889e141e81ed299ee8e` |
| `noto-2.057-qt.records` (Noto 2.057 as Qt 6.11.2 draws it, 148,023,912 bytes, not kept) | | `ed8d380385c9b6f7684245a9713e31d9f5757f2244da04b7c4a1617e85b1c8fb` |
| `templates.bin`: 3,781 Noto 2.051, 3,417 Twemoji, 445 Noto 2.047, 291 Noto 2.057 and 221 Noto 2.057 as Qt draws it; 315 of them plain shapes | | `99bcdbc104663455b1e706792858f93ca04fa27f718924243114f1f6b2d95990` |

Noto Color Emoji redraws some emoji in each release. Against 2.051, 445 of 2.047's emoji and
291 of 2.057's differ by more than `CHANGED` (0.03, a mean colour difference on white; the
🚀 of 2.057 differs by 0.16), so they are templates of their own. Ubuntu 26.04's 2.051 build
(`2.051-1build1`) is a different file of the same release; its emoji differ from Debian's by
0.02 at most, so they need none. Qt draws a COLRv1 font such as 2.057 itself, without some of
its shading (flags and 🍳 come out flat), so the 221 emoji Qt draws differently are templates
too: a Plasma desktop on Fedora or Aurora shows those.

Noto Color Emoji is the emoji font of Fedora, Debian, Ubuntu, Kubuntu, Linux Mint, Arch and
Aurora, so it is what most applications draw, Firefox from those distributions included.
Firefox's own builds draw emoji with Twemoji Mozilla, and Discord shows Twemoji pictures.

## Licences

- Noto Color Emoji: Copyright 2022 Google Inc. Licensed under the SIL Open Font License,
  Version 1.1 (<https://openfontlicense.org>).
- Twemoji Mozilla: graphics Copyright Twitter, Inc and other contributors, from
  [mozilla/twemoji-colr](https://github.com/mozilla/twemoji-colr). Licensed under CC-BY 4.0
  (<https://creativecommons.org/licenses/by/4.0/>).
- The emoji sequences come from Unicode's `emoji-test.txt` 16.0, Copyright Unicode, Inc.,
  under the Unicode License v3 (`UNICODE-LICENSE.txt`).
