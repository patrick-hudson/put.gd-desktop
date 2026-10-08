# put.gd visual design

The native Qt interface uses a graphite canvas, deep green surfaces and a mint accent. The mint indicates a primary action or keyboard focus; red identifies a destructive action. Text uses the desktop's installed sans-serif font. There are no downloadable fonts, web views or raster decoration assets.

Settings is organized around four keyboard-selectable sections: Capture, Destinations, Shortcuts and Preferences. Each section scrolls independently while Save and Reload stay visible. Cards group related decisions, and action cards keep destination/profile choices beside the corresponding capture action. The configuration path and accepted/observed revisions remain available as selectable text in a collapsed panel. The setup and validation message stays outside the scrolled content. Shortcut editors show the actual desktop binding and a readable status for each action, separately from the editable draft. Conflicts and unavailable support remain clear; exact native diagnostics are available through an advanced disclosure.

Activity keeps the existing operation/item hierarchy, IDs, URLs and independent outcomes. History keeps the local offline ledger, all filters, exact receipt details and existing destructive confirmations. Both use the same title treatment, spacious rows, hand-drawn vector type icons, state badges and clear empty/loading text. Long table cells elide visually and keep their full contents in tooltips or receipt details.

All editable controls retain their stable object names. Sidebar navigation, form inputs, table selection and action buttons support keyboard focus. Settings scroll areas and the independently scrolling history inspector preserve a 1000×700 logical-pixel window. Display scaling is handled by Qt using logical pixels; vector icons are drawn at the display's device pixel ratio.

## Verification boundaries

Qt GUI tests exercise navigation, sizing, settings behavior, operation outcomes and history behavior. They cannot establish cosmetic completeness or compositor support. AE15 requires inspection of the installed application in a real graphical session at the supported scale factors, including populated and empty views, errors, exact remote deletion confirmation and keyboard focus. Record actual witness results in the acceptance evidence; do not infer them from offscreen tests.
