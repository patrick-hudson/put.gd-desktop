# Narrow layout: left column

At widths of 900px and below the map is one column. Before, the left column stayed sticky under the toolbar and covered the surfaces as you scrolled (red box). Now it scrolls away with the page, and the Map, Gaps and Review views stay inside the window (green box). The Matrix view is a wide table and scrolls sideways at every width, as it does on nightly. Wider layouts keep the sticky column under the measured toolbar.

Code: e4857ba46bf110b8046ad1b7b9b9d9840d3ab450 (PR #667). Rendered at 760x900, scrolled to y=3000, in headless Chromium.

| Width | Column position | Page width | Column top vs toolbar bottom (wide only) |
| --- | --- | --- | --- |
| 520 | static | 520 | n/a |
| 760 | static | 760 | n/a |
| 1600 | sticky | 1600 | 105 = 105 |
