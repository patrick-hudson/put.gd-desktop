# Bun 1.4.5 Linux x64 attribution

The selected official Bun release is 1.4.5, source commit
`a7733435cf40429e3b35c36072769df2052b80f1`, executable SHA-256
`f05d28a3862f4b84395c7fa32480b835dd47482e80b7e7a49a115227f0c06083`.
The official release archive and executable hashes are in runtime-lock.json.
The archive matches Bun's SHASUMS256.txt, whose signature verifies with Bun's
release key `F3DCC08A8572C0749B3E18888EAB4D40A7B22B59`. The npm executable was
compared byte-for-byte with the executable from that archive.

The executable reports WebKit `d6375c72affe0f70222cdcb45012b10f0bf3a9d5`
(unchanged from Bun 1.4.4), ICU 78.3, BoringSSL
`fe703e9faa3bfd22b04eb192066856557b7e015c` and c-ares
`0b88066f79ac9745d6819899f1794df91b73c873`. The matching Bun and complete
WebKit Git source objects accompany the Bun 1.4.5 source packet. Its input
manifest preserves the current native recipe, Cargo and npm source/build
supersets, Node headers, ICU source and pinned Rust standard-library source.
Cargo and npm inputs pass their upstream lock integrity checks.

BUN-THIRD-PARTY-NOTICES.txt.gz retains the earlier attribution corpus, the Bun
1.4.4 preferred-source supplement and a Bun 1.4.5 supplement. Bun 1.4.5 moves
only BoringSSL, c-ares and LSQUIC among its sources; every other input and the
WebKit commit are byte-identical, so their Bun 1.4.4 records stay current. The
1.4.5 supplement scans Bun 1.4.5's source and the three changed archives, and
adds only texts not already in the corpus. BUN-NOTICE-PROVENANCE.json.gz
identifies each record by source path, source SHA-256 and text SHA-256.
The source packet includes the collector and all preferred sources.

This is a conservative source/build/platform attribution superset. Inclusion
does not assert a component is linked into the Linux executable. Bun's upstream
MIT declarations, JavaScriptCore/WebKit's per-file licenses, TinyCC's LGPL
materials and imported-source attributions are preserved without inventing
copyright holders. The exact ICU 78.3 notice remains LICENSE.icu4c-78.3.

The Bun 1.4.5 packet verifies offline source assembly, native patch application
and Bun's own native fetch identities. The Bun 1.4.4 native modification/relink
proof is historical and does not establish a Bun 1.4.5 native rebuild; that
check remains open in SOURCES.json. The production helper uses the verified
official Bun executable, and its exact helper-source rebuild is checked
separately. These materials are redistribution engineering evidence, without
legal certification or final desktop acceptance.
