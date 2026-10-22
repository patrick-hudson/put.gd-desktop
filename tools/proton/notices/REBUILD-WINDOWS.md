# Proton helper for Windows: corresponding source and rebuild

`putgd-proton-runtime.exe` is the same helper as the Linux one, compiled for
`bun-windows-x64`. Its sources are the six published packets in `SOURCES-WINDOWS.json`
(the Linux catalog's packets, unchanged) plus the `applicationSourceOverlay` files from
the current put.gd application source. `REBUILD.md` explains each packet and how to
assemble the helper source; this file covers what differs on Windows.

## Helper rebuild

On a Linux x86-64 host with Node and npm, from the put.gd source at the catalog's
`applicationSourceOverlay.sourceCommit` (or a later commit whose overlay files hash the same):

```sh
python3 tools/proton/provision_runtime.py --target windows-x64 --output NEW_DIR
```

It verifies the pinned SDK and crypto checkouts, installs the locked dependency graph,
runs the helper tests with the Linux Bun and cross-compiles with the locked Windows Bun
(`@oven/bun-windows-x64`, verified against `package-lock.json`'s sha512 integrity).
`NEW_DIR/manifest.json` records the helper's SHA256; it must equal the catalog's
`helperSha256`. With `--source` and `--crypto-source` pointing at extracted packet
checkouts and a populated npm cache, add `--offline`.

To build with another Bun runtime, as `REBUILD.md` describes for Linux, pass the
Windows executable to the compiler directly:

```sh
cd NEW_DIR/build-source
"$proton_bun" build --compile --target=bun-windows-x64 --compile-executable-path="$windows_bun_exe" \
  --minify runtime.ts --outfile=putgd-proton-runtime.new.exe
```

Keep the `.new.exe` basename: Bun embeds the output name in the executable.

## Using a changed helper

put.gd for Windows runs only the helper whose SHA256 was compiled in from the manifest
(`-DPUTGD_PROTON_MANIFEST=NEW_DIR/manifest.json` when configuring `src/windows`). After
changing the helper, rebuild `putgd.exe` with the new manifest and place both, with the
manifest and notices, as `src/windows/packaging/package.py --proton-runtime NEW_DIR` does:
`proton\putgd-proton-runtime.exe` next to `putgd.exe`. A helper that does not match
leaves the Proton destination unavailable (`proton_runtime_unavailable`); nothing else
changes.

## Windows Bun runtime

The Bun packet's sources build Windows as well as Linux: Bun's recipes select the
Windows inputs (`libuv`, TinyCC's `tccpe.c`, libarchive's Windows sources, BoringSSL's
NASM assembly), and the WebKit Git source holds `Dockerfile.windows`, which
cross-compiles JavaScriptCore and ICU 78.3 (`archives/icu4c-78.3-sources.tgz` in the Bun
packet) with LLVM `clang-cl`. Both builds need the Microsoft C runtime and Windows SDK,
which their recipes download with xwin (MSVC CRT 14.44.17.14, Windows SDK 10.0.26100)
under Microsoft's license; those are not part of the source packets and cannot be.
A modified Windows relink has not been performed; the Linux proof covers the same
sources (see `BUN-WINDOWS-ATTRIBUTION.md`).
