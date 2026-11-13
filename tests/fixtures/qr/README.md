# QR code fixtures (#1428)

`codes.json` holds known-good module matrices for `qr_code_contract`
(`tests/unit/test_qr_code.cpp`): `short` is a 34-byte link, `longest` the
2,331-byte link that is the most level M holds (both links are built in the
test). Each `rows` entry is one row of modules, `1` dark, without the quiet zone.

They were made with an encoder independent of the vendored generator, segno
1.6.6 (`segno.make_qr(link, error='m', boost_error=False, mode='byte')`), and
put.gd's own exported PNGs of the same links (`PUTGD_QR_WRITE_DIR=<dir>
test_qr_code`) were read back to the exact links by `ZXingReader` and `zbarimg`.
A change to the generator, the scale or the quiet zone that still reads back
needs the same check before these rows change.
