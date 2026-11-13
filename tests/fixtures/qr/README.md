# QR code fixtures (#1428)

`codes.json` holds known-good module matrices for `qr_code_contract`
(`tests/unit/test_qr_code.cpp`): `short` is a 34-byte link, `longest` the
2,331-byte link that is the most level M holds (both links are built in the
test). Each `rows` entry is one row of modules, `1` dark, without the quiet zone.

Both were checked on 2026-11-12 by reading put.gd's own exported PNGs
(`PUTGD_QR_WRITE_DIR=<dir> test_qr_code`) back with two independent decoders,
ZXingReader 2.3.0 and zbarimg 0.23.93 on Debian 13: each returned the exact link.
`longest` is also bit for bit what an independent encoder makes, segno 1.6.6
(`segno.make_qr(link, error='m', boost_error=False, mode='byte')`, mask 2).
For `short` segno chooses another, equally valid arrangement (both decode), so
its rows are put.gd's checked output.

A change to the generator, the scale or the quiet zone needs the same
read-back check before these rows change.
