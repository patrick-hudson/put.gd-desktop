# Gate C idle memory on the published nightly 26.1102.3797 (#1149)

Measured 2026-11-02 for [#1149](https://github.com/patrick-hudson/put.gd-desktop/issues/1149):
the resident of the published nightly, started with `--background` inside each builder's real
desktop session with a private profile (`XDG_CONFIG_HOME`, `XDG_STATE_HOME`, `XDG_CACHE_HOME`,
`XDG_RUNTIME_DIR`), no credentials, so the initial view's `putgd-ui` was open beside it. 30 s
settle, then RSS and Pss from `smaps_rollup` every second for 180 s, then a full `smaps` dump.
Hosts were quiet (load 0.00 to 0.19) and held under the desktop lock. Packages came from
pkg.put.gd; the AppImage from the GitHub release. All SHA-256 digests matched the repository
index and `SHA256SUMS`. `--version-json`: `26.1102.3797-nightly`, commit `858e017cc5dd`.

| Host | Session (`loginctl`) | Package, SHA-256 | Qt | RSS MiB (median; repeat) | Pss MiB | USS MiB | GTK / KDE theme | `libQt6Network` | `libssl` |
| --- | --- | --- | --- | ---: | ---: | ---: | --- | --- | --- |
| f44 Fedora 44, GNOME Shell 50.6 | wayland | RPM `0b2333a9…dab8ab` (repo-signed copy of release asset `b0990a6c…881507`) | 6.11.2 | 39.47; 39.59 | 16.52 | 8.93 | none | yes | yes (3.5.9) |
| k72 Kubuntu 26.04, Plasma / KWin 6.6.6 | wayland | DEB `6e936a42…562fb0` | 6.10.2 | 40.64; 40.56 | 11.70 | 9.70 | none | yes | yes (3.5.5) |
| m74 Mint 22.3, Cinnamon 6.6.9 | x11 | DEB `49907c05…53e241` | 6.4.2 | 36.09 | 15.23 | 8.86 | none | yes | no |
| f44 | wayland | AppImage `61634f2e…599e1` | bundled 6.8 (ICU 73) | 33.01 | 17.97 | 9.65 | none | yes | no |
| k72 | wayland | AppImage, same | bundled 6.8 | 33.72 | 19.33 | 11.35 | none | yes | no |
| m74 | x11 | AppImage, same | bundled 6.8 | 33.51 | 18.72 | 10.44 | none | yes | no |

The window was flat: RSS min to max moved 0.15 MiB or less on every run, CPU 0.05 to 0.36 s
over 210 s. No QPA platform plugin and no platform theme is mapped in the resident on any host.

## Where the RSS is (native, MiB)

| Group | f44 RPM | k72 DEB | m74 DEB | AppImage (f44) |
| --- | ---: | ---: | ---: | ---: |
| Qt libraries (Core, Gui, Widgets, DBus, Network, Sql, WaylandClient) | 13.12 | 11.98 | 10.72 | 12.42 |
| `putgd` binary itself (Pss = RSS, nobody shares it) | 3.93 | 4.46 | 3.97 | 3.72 |
| glibc, libstdc++, libgcc, libgomp | 4.78 | 5.00 | 4.51 | 4.50 |
| anonymous (heap, stack) | 1.75 | 2.09 | 1.79 | 1.54 |
| TLS and crypto (`libssl`, `libcrypto`, GnuTLS, nettle, p11-kit) | 1.79 | 4.23 | 3.38 | 0 |
| Kerberos, LDAP, SASL | 1.13 | 1.03 | 0.85 | 0.67 |
| systemd, dbus, compression | 2.37 | 2.19 | 2.54 | 1.99 |
| glib, gio, gobject | 1.81 | 1.87 | 1.56 | 1.58 |
| ICU | 1.70 | 1.82 | 1.69 | 1.58 |
| sqlite and its Qt plugin | 1.47 | 1.33 | 1.34 | 1.24 |
| everything else (curl, ssh, nghttp, proxy, xcb, wayland, fonts, Mesa) | 5.62 | 4.64 | 3.74 | 3.77 |
| **RSS** | **39.47** | **40.64** | **36.09** | **33.01** |
| RssFile / RssAnon (`/proc/pid/status`) | 36.6 / 5.0 | 35.8 / 4.6 | 32.6 / 4.4 | 30.5 / 4.0 |

## The `libQt6Network` chain

`readelf -d /usr/bin/putgd` lists `libQt6Network.so.6` directly (the session socket is a
`QLocalServer`). The distro's `libQt6Network` needs `libgssapi_krb5` and `libproxy`; `libproxy`'s
`libpxbackend-1.0.so` needs `libcurl`, and `libcurl` brings the distro's TLS stack. Walking the
`NEEDED` graph of every mapped library, these are mapped only through `libQt6Network`:

| Host | Libraries only reachable through `libQt6Network` | RSS MiB | Pss MiB |
| --- | --- | ---: | ---: |
| f44 | `libQt6Network`, `libproxy`, `libpxbackend`, `libcurl`, `libssl`, `libssh`, `libldap`, `liblber`, `libsasl2`, krb5 (5), `libnghttp2/3`, `libngtcp2` (2), `libpsl`, `libidn2`, `libunistring`, `libfido2`, `libcbor`, `libevent`, `libudev`, `libduktape`, `libresolv`, `libcrypt`, `libcom_err` | 4.18 | 0.97 |
| k72 | the same shape, with `libcurl-gnutls`, GnuTLS, nettle, hogweed, gmp, p11-kit, tasn1, rtmp, `libssh2`, plus `libssl` and `libcrypto` through Ubuntu 26.04's OpenSSL-linked `libldap` | 6.63 | 1.43 |
| m74 | the same as k72 without `libssl` (Mint's `libldap` uses GnuTLS); `libcrypto` through `libssh` and `libsasl2` | 5.36 | 1.29 |
| AppImage | `libQt6Network` and krb5 only: the bundled Qt has no `libproxy` | 0.7 | 0.5 |

`libssl` and `libcrypto` alone: f44 0.23 + 1.20 MiB RSS (0.06 + 0.49 Pss; `libcrypto` is also
needed by Fedora's `libQt6Core`, so it would stay), k72 0.27 + 1.66 (0.07 + 0.60), m74 0 + 1.38
(0.40). `libQt6Network` itself: 0.40 to 0.53 MiB RSS, 0.10 to 0.30 Pss. Qt's own TLS plugin
(`libqopensslbackend.so`) is not mapped on any host; on Fedora it would need `libssl`, on Ubuntu it
loads OpenSSL at run time. Only `libsqlite3` and `libqsqlite` are loaded at run time; everything
else comes from `NEEDED` entries.

## Raw data

`libraries.csv` here holds every mapping's RSS, Pss and private pages per run. The full `smaps`,
`maps`, `status`, `samples.jsonl`, `env.json` and `facts.json` of each run stay on the builders
under `~/putgd-lab/i1149/run-{native,native2,appimage}-*/`, with the measurement script
`i1149_measure.py` and the downloaded packages beside them.
