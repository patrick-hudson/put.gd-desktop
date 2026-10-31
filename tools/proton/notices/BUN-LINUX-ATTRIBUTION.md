# Bun 1.4.4 Linux x64 attribution

The selected official Bun release is 1.4.4, source commit
`38afee6c84fcf48c0edb2466aa4b27d287a59c30`, executable SHA-256
`10702245766b225e70bbf6235fd6b47214fd94b4760d00ffaed8ab0df57ac235`.
The official release archive and executable hashes are in runtime-lock.json.
The npm executable was compared byte-for-byte by SHA-256 with the executable
from the checksum-verified official Linux x64 release archive.

The executable reports WebKit
`d6375c72affe0f70222cdcb45012b10f0bf3a9d5`, ICU 78.3 and BoringSSL
`41bf9b59c2ebf277a7aa427e1ecad5cc80dd4d4f`. The matching Bun and complete
WebKit Git source objects accompany the new Bun source packet. Its input
manifest preserves the current native recipe, Cargo and npm source/build
supersets, Node headers, ICU source and pinned Rust standard-library source.
Cargo and npm inputs pass their upstream lock integrity checks.

BUN-THIRD-PARTY-NOTICES.txt.gz retains the earlier attribution corpus and appends
a Bun 1.4.4 preferred-source supplement: standalone legal files and original
legal comments from Bun, WebKit and the archived native/Cargo/npm/Rust sources.
BUN-NOTICE-PROVENANCE.json.gz identifies each new text by source path, source
SHA-256 and text SHA-256. Earlier records remain explicitly historical.
The source packet includes the collector and all preferred sources.

This is a conservative source/build/platform attribution superset. Inclusion
does not assert a component is linked into the Linux executable. Bun's upstream
MIT declarations, JavaScriptCore/WebKit's per-file licenses, TinyCC's LGPL
materials and imported-source attributions are preserved without inventing
copyright holders. The exact ICU 78.3 notice remains LICENSE.icu4c-78.3.

The new packet verifies offline source assembly and native patch application.
The existing native rebuild supplement documents the earlier Bun 1.4.2
modification/relink proof; that proof is historical and does not establish a
Bun 1.4.4 native rebuild. A new native modification/relink check remains open.
The production helper uses the verified official Bun executable, and its exact
helper-source rebuild is checked separately. These materials are redistribution
engineering evidence, without legal certification or final desktop acceptance.
