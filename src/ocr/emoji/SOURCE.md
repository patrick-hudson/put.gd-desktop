# Emoji template sources

`templates.bin` holds the pictures `putgd-ocr` names emoji by when it reads text from the
screen ([#1748](https://github.com/patrick-hudson/put.gd-desktop/issues/1748)). It is built
into the helper. No font ships with put.gd: each template is a small bitmap of one emoji
as a font draws it.

Made on d69 (Debian 13, Pango 1.56.3, cairo 1.18.4, PyGObject 3.50.0) on 2026-11-20 with
[`tools/text/emoji_templates.py`](../../../tools/text/emoji_templates.py):

```sh
FONTCONFIG_FILE=fonts.conf tools/text/emoji_templates.py --emoji-test emoji-test.txt \
  --style noto="Noto Color Emoji" --style twemoji="Twemoji Mozilla" --output templates.bin
```

`fonts.conf` includes `/etc/fonts/fonts.conf` and adds the directory holding
`TwemojiMozilla.ttf`. The defaults give 20 by 20 and 10 by 10 pixel templates.

| Input | Version | SHA-256 |
| --- | --- | --- |
| [`emoji-test.txt`](https://unicode.org/Public/emoji/16.0/emoji-test.txt) | Unicode Emoji 16.0 | `24f0c534e86cf142e2496953e8f0e46a3e702392911eddcd29c6cced85139697` |
| `NotoColorEmoji.ttf` (Debian `fonts-noto-color-emoji` 2.051-0+deb13u1) | 2.051 | `dac5c27651082d6c53dab5081f50d2022ddf6877d730fa919cf6a4fc2af22de0` |
| `TwemojiMozilla.ttf` (bundled with Firefox, the same file in Firefox ESR 153 and Firefox 158 and 159) | 0.7.0 | `6d90152ee0d29e82fe2a87793af5aa4b7ad13e6538360889e141e81ed299ee8e` |
| `templates.bin` | 3,781 Noto and 3,417 Twemoji templates, 173 of each style's emoji marked as plain shapes | `cd46302ce89727e990d1d233ffed960bf0ae71534c6ce912226d51e3fb00a126` |

Noto Color Emoji is the emoji font of Fedora, Debian, Ubuntu, Kubuntu, Linux Mint, Arch and
Aurora, so it is what most applications draw. Firefox draws emoji with its own Twemoji
Mozilla, and Discord shows Twemoji pictures.

## Licences

- Noto Color Emoji: Copyright 2022 Google Inc. Licensed under the SIL Open Font License,
  Version 1.1 (<https://openfontlicense.org>).
- Twemoji Mozilla: graphics Copyright Twitter, Inc and other contributors, from
  [mozilla/twemoji-colr](https://github.com/mozilla/twemoji-colr). Licensed under CC-BY 4.0
  (<https://creativecommons.org/licenses/by/4.0/>).
