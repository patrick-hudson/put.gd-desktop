# Region selection without the idle monitor outline (#888)

PR #888 removes the black/white ring that region selection drew around the
whole active monitor before any drag. The outline now appears only once a
region exists.

| Picture | Shows |
| --- | --- |
| [region-idle-outline-dark.png](pictures/region-idle-outline-dark.png) | Dark theme, idle region overlay: before vs after |
| [region-idle-outline-light.png](pictures/region-idle-outline-light.png) | Light theme, idle region overlay: before vs after |

Sources: the `46-region-entire-display` UI catalog state. Before is the
reference on `nightly` 3d23ddf3; after is CI's render of PR #888 at
b07d7124 (run 41060885944, `linux (26.04)`), the code the PR head f5a46ec3
carries unchanged. The catalog renders the overlay with a transparent
background; the pictures composite it on a stand-in grey desktop and add a
4x zoom of the bottom-left monitor corner, where the 3 px ring sat.
