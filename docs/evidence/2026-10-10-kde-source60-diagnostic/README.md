# KDE source60 diagnostic checkpoint

This candidate is unaccepted. Four Qt local native cases passed; post-action RSS was 40.195 MiB, above the 40 MiB target. No full twelve-case suite, five-minute confirmation, UI acceptance, or current-version sibling regression has been completed. See summary.json for version, hashes and limits.

The bounded standalone SVG probe confirms that the installed Qt6.10.2 codec aborts on a text-bearing SVG under QCoreApplication while the same bytes render under QGuiApplication. The product remains unchanged during this dependency probe. This proves the dependency constraint; it does not prove a production action or worker fix. No new dependency was installed.

To reproduce the dependency probe on a Linux distro with its normal Qt SVG image plugin installed, copy svg-probe into a fresh private directory. Its wrapper creates private output and disables core dumps, limits output to64KiB per stream and each child to15seconds. Existing output is rejected. Build and run with the normal tools:

```sh
cmake --preset dev
cmake --build --preset dev
python3 -B run_probe.py
```

Read actual codec inventory and both exit statuses. A missing SVG plugin, failed GUI initialization, or timeout is inconclusive. Offscreen rendering is dependency diagnosis only. Preserve failed results; do not overwrite or report this as desktop acceptance. No credentials, user payloads or graphical input are required.
