The preference family's zero-delay sizing callback observes `LayoutRequest` on
its rows. The callback calls layout setters, which issue another layout request,
so an idle shown Settings window repeatedly queues the callback. `perf` puts the
preference-family callback on the hot UI stack; this reproduces offscreen without
an AT-SPI client, and History stays idle.

The final fix stops observing the sizing pass's layout output. The existing
combo-model and shortcut-text signals, plus font, visibility and geometry events,
still schedule sizing. A newly shown hint also receives a bounded inner-layout
refresh when its container has room but a child remains below its minimum height.
An explicit recording-prompt callback covers placeholder-only changes in an empty shortcut. No periodic timer is added and the normal sizing setters remain in place.

Earlier attempts guarded redundant setters. They stopped CPU usage but exposed
stale Qt height-for-width caches in the catalog. Those attempts were rejected;
no golden/reference images were changed. The final candidate matches every
compared light catalog state on Qt 6.10.2 (218 match, one existing exemption).
