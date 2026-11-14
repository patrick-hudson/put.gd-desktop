# put.gd visual language

This is put.gd's desktop visual language: warm charcoal surfaces, neutral
primary buttons, a restrained amber accent, and compact controls around the
user's content. Dark mode is the primary design target. Light mode uses the same
hierarchy and geometry with a complete warm off-white palette.

The implementation uses C++20 and Qt 6 Widgets, with a minimum Qt API of 6.4.2
and dynamically linked distribution libraries. Painting owns appearance; Qt
continues to own text entry, layout, focus, accessibility, model/view selection,
and window-system integration. There is no web view or replacement text-entry
engine.

## Shared implementation

Use the shared components before adding window-specific painting:

| Responsibility | Source |
| --- | --- |
| Semantic colors, theme notification and contrast calculation | `src/ui/Theme.h`, `Theme.cpp` |
| Control painting, geometry, type, surfaces, navigation, visual roles, help, icons and live scheme propagation (`putgd::Ui`) | `src/ui/Ui.h`, `Ui.cpp` |
| Window theme entry point, `Ui::styleWindow` (needs the bundled fonts, so it lives in `putgd_ui`) | `src/ui/UiWindow.cpp` |
| Bundled IBM Plex Sans and Mono registration | `src/ui/Fonts.h`, `Fonts.cpp` |
| Action glyphs | `src/ui/IconArt.h` |
| Synthetic screens of every window state, rendered in CI for inspection | `putgd-ui-catalog` (`tests/gui/ui_catalog.cpp`, [the UI catalog](ui-catalog.md)) |

`Ui::Style` is a `QProxyStyle` over Fusion. It paints standard controls
without an application stylesheet. Style hints set on widgets are dynamic
properties with a `ui` prefix (`uiFlush`, `uiDensity`, …); a bare name would
shadow real Qt properties such as `font`, `palette` and `margin`. Use
`Ui::apply` for the application and `Ui::styleWindow` for each window so
late-created windows and hidden controls follow theme changes. Use IconArt for actions and the shared chrome glyphs for window
controls. Do not introduce a second icon family or local copies of color tokens.

## Color and type

These core tokens describe the visual hierarchy. `Theme::Palette` remains the
source of truth for the full palette, including status surfaces and disabled
states.

| Token | Dark | Light |
| --- | --- | --- |
| Background | `#191919` | `#FFFFFF` |
| Navigation / inspector surface | `#202020` | `#F5F5F2` |
| Raised control | `#2A2A2A` | `#FFFFFF` |
| Selected row / segment | `#333230` | `#ECEBE6` |
| Border | white at alpha 18/255 | black at alpha 20/255 |
| Strong border | `#737373` | `#8E8E87` |
| Main text | `#F0F0EC` | `#292926` |
| Secondary text | `#C4C4BF` | `#484844` |
| Muted readable text | `#AAAAA6` | `#62625B` |
| Accent | `#E8B66D` | `#8F5B20` |
| Confirmed success | `#91BCA2` | `#386A4B` |
| Error / destructive action | `#E59B93` | `#A44139` |

A primary button uses the main-text token as its fill and the background token
as its foreground. Amber identifies keyboard focus and small active editing or
trim affordances. Success and error colors accompany words; color alone never
communicates an outcome. Ordinary settings do not need colored cards.

Use IBM Plex Sans for interface text and IBM Plex Mono for technical values that
benefit from it. Default logical sizes are 14 px body, 12 px metadata and
micro-labels, 16 px section headings and 22 px page headings (20 px compact headings). Weights are normal
and semibold. Reserve micro-labels for secondary context, not instructions or
errors that a user must read to continue.

Measure the same font and weight that will be painted. Center painted button
labels vertically using the visible text's ink bounds; center an icon, its gap
and its label as one group. Editable text retains Qt's caret and font metrics.
Elide long document names and bounded summaries, with the full value available
through a tooltip and accessibility text. Do not elide a failure's only recovery
instruction. Display-only wrapping must not change copied values: Copy diagnostics,
keyboard and context-menu copying, and X11 selections retain the original text,
including any original Unicode characters.

Desktop text size and display pixel ratio are separate concerns. Use
`Ui::font` and `Ui::scaled` for painted type and scalable geometry;
let Qt handle DPR. Installation must preserve an already enlarged application
font. `PUTGD_TEXT_SCALE` is a test override, not a saved appearance preference.

## Geometry and controls

The baseline grid is 4 logical pixels. Prefer deliberate 8, 12, 16, 20 and 24 px
gaps. Controls start at 36 px high and grow when text requires it. Action icons
are normally 20 px, with 32–36 px pointer targets. The normal control radius is
6 px; compact controls use 5 px, selected segments use 6 px inside a 10 px
track, and swatches and small details use 4 px. A split button gives its arrow a
separate 32 px target; combo-box and menu chevrons use a 26 px zone.

The main header starts at 56 px, the command band at about 60 px including
padding, and the status band at 40 px. These are component baselines, not reasons
to break a supported small-screen or large-text layout. History uses 76 px base
rows with 96 × 60 thumbnails and 28 px row actions, expanding for complete
failure text when needed. Reserve space for row actions before laying out text.
When an inline action would leave too little room for the item name, place
actions in a stable footer below the text. Hover and selection must not change
the row height.

Use one hairline between adjacent regions. Prefer flat sections and aligned form
rows to nested cards, oversized pills, gradients or ornamental shadows. Give
ordinary choices sensible widths; let paths use available space. Forms on one
page share a label gutter. When space requires a different layout, switch the
whole relevant group consistently rather than wrapping unrelated rows one at a
time. A value and its unit form one joined control with aligned top and bottom
edges and a shared text baseline, including at enlarged text sizes. Put effective
limit hints below that control. Scroll long forms and contextual properties while
keeping primary actions reachable.

Buttons have primary, secondary, quiet, destructive and disabled states. Hover
and press changes stay subtle. Keyboard focus is visible and distinct from
mouse hover. Text inputs keep their focused editing frame. Checkboxes and radio
buttons must remain visibly checked when disabled. Menus preserve shortcuts,
submenus, checkable state, disabled items and keyboard navigation. Literal
ampersands must survive Qt's mnemonic escaping.

## Navigation and window behavior

The main header has the application menu and name on the left, one centered
History–Destinations–Settings segmented control, and window controls on the
right. Each page can have a sidebar for its own categories. The command band
keeps capture and upload actions available across pages.

At narrower widths, Capture window and Capture desktop can fold into the
Capture region split menu. Remaining tools can use icons with complete tooltips
and accessible names. A compact layout must retain every action.

The image editor uses the shared document header with its title and truthful
upload state. Recording review keeps the desktop title bar to preserve frame
area and fit at enlarged text sizes; its Not uploaded state lives in the footer.
Both use shared controls around the working area and the app's window identity.
The editor helper receives the resident's resolved light/dark scheme so opening
it does not unexpectedly change appearance.

The About window uses the compact header with only a close button: it fits its
content, so it has no resize edges, maximize or window menu. Its top is a painted
scene, a dimmed desktop with put.gd's identity in a lit region selection, and it
follows the theme tokens.

Custom Linux chrome uses `QWindow` system move and resize rather than
compositor-specific positioning. Verify dragging, all resize edges, minimize,
maximize/restore, double-click, close and keyboard window management on both X11
and native Wayland. The `PUTGD_SYSTEM_DECORATIONS=1` fallback uses desktop
decorations for windows that otherwise use custom chrome.

Appearance remains in Settings → General → Appearance. There is one persisted
write path for the preference; a window-specific theme control must not create a
second source of truth.

## Words and actions

Use the same action names in the command band, menus, help and hotkey settings:

| Action ID | Visible action |
| --- | --- |
| `region` | Capture region |
| `window` | Capture window |
| `desktop` | Capture desktop |
| `edit` | Capture region & edit |
| `clipboard` | Upload clipboard |
| `record` | Record screen |

Other image sources are **Edit clipboard image** and **Edit an image file…**.
Choosing either must not start a region capture. Keep drag-and-drop, tray,
keyboard, session, CLI and MCP paths working. A visual change does not create
a new upload-file pipeline or change an action ID.

Use sentence case and the actual action instead of generic Yes, OK or Proceed.
An ellipsis means another choice, picker or confirmation is required. Show units
beside values. Confirmation text names the target and consequence; destructive
actions have a named action button, with Cancel as the initial and Escape choice.
Do not display an optimistic success before the operation is accepted, or leave
completed feedback on an unrelated page.

## Feature composition and boundaries

**History.** All items, Images, Recordings and Other files are content categories.
Other files includes clipboard text and arbitrary files; its aggregate filter
uses the new `history.list` category value `text+other`, matching stored `text`
and `other` rows. All other category values, including `files`, retain exact
matching; no stored category is renamed. Item, Output and Created
describe list columns. Capture output in the sidebar describes saved routing for
future captures. It is not a destination filter or an active-transfer indicator.

Retain list and gallery views, search, filters, paging, multi-selection,
multi-file inspection, cancelled-item visibility, progress, cancellation,
diagnostics and maintenance.

The page heading is the category at the 22 px page size (20 px in a narrow
header) with a muted count note beside it: `236 items`, or `12 of 168 items`
while a search or filter narrows the category. The note counts stored files,
never grouped rows or live activity without a file; it is blank while a query
loads and falls back to `12 matching items` when the category total is unknown.
The sidebar shows each category's unfiltered total; Failed uploads uses the error
colour and hides a zero. Both come from one `history.counts` request per change of
the page's files, never per progress update.

Rows sit in day sections: a 16 px semibold heading in sentence case (`Today`,
`Yesterday`, `Saturday, November 7`, with the year only when it is not the
current one) and a muted count of that day's stored files within the query.
Every day after the first has a hairline and 12 px of extra space above it. Work
in progress sorts first under `In progress`; a day that began on the previous
page says `· continued`. The heading of the day at the top of the viewport stays
pinned over its rows, takes no clicks, and keyboard movement never leaves the
current row under it. Failed uploads has no day sections; its time column shows
the date as well.

A wide row reads, left to right: the thumbnail (with a hairline ring in the dark
scheme), the name in Medium weight, and one detail line in the order kind · size
· destination · state. State words are never cut; the destination elides first
and goes rather than shrink below its first word, then the size and file count
go, and last the kind by the destination's rule, so no stub such as `Re…` is
left. A failure line is body size and names where the upload was going (`Upload to
Proton Drive failed · Connection interrupted`); it wraps rather than elides.
Right of the text come the row's labelled recovery action (Retry upload, Finish
share link, Retry local save, Retry deletion), a right-aligned time column (relative for the last
hour, else the locale's short time; full timestamp in its tooltip) measured from
the widest text it can show, and a slot reserved for copy and more. A row with a
recovery action keeps more visible; hover never moves anything. The keyboard-
current selected row carries the sidebar's 2 px accent bar. Below 480 px of row
body the time moves into the detail line and the actions collapse to one menu.

The gallery tile caption is the same three lines everywhere: the name on up to
two lines, then the detail line with the time at its right. A failed tile gives
one line to the name and two to the failure, ending with `Details in ⋯` when the
text cannot fit; a row of tiles grows together only when neither fits. Copy link
and more sit on the picture's top-right corner over a scrim, a recovery action on
its lower-left; day sections start new rows of tiles under the same headings.

The pager is one row under the list, a hairline above, grouped at the right 24 px
inside the list's edge: `Page 2 of 5 · items 51–100 of 236`, Previous and Next
(Alt+Left and Alt+Right). It is absent on a single page, blank and disabled while
a page loads, and a deletion that empties the last page steps back to it.

The details pane always has a title row with the file name (or `3 selected
files`) and a close button; Esc closes it. In the wide list it shows whenever
details are wanted and something is selected; once closed it stays closed
until Enter, a double-click or the header's details toggle asks for it. That
choice lives in the view state: it survives closing and reopening the window
while put.gd runs and starts as shown after put.gd restarts. The gallery and the compact window open it on request over the current
selection. With the pane closed and two or more rows selected, the selection bar
replaces the header's heading and tools: `3 selected` at the page size, a note
with size and upload states (and an active search), Copy N links, Delete from
destination…, more and Clear selection, then the view switch and details toggle;
in a narrow header Delete folds into more first and Copy links last.

Active filters are 36 px chips under the header, aligned with the heading, at
most two lines: each opens its filter's own editor and has its own remove
button; `+N more` opens the panel of every filter; Clear filters ends the row.
Filter opens the add-filter menu directly. The view switch is the list and grid
glyphs in the shared segmented control; Manage holds history maintenance and
becomes `⋯` in a narrow header, as does Filter's word when the heading would
otherwise need its own row.

Show actions from actual capabilities and receipts:
Copy link requires a valid stored link; editing requires supported retained
image bytes; retry requires the corresponding retained artifact. Remote deletion
and clearing local history are separate operations with exact effects. An empty
history and a search with no matches are different states.

**Destinations and Settings.** Use aligned, scrollable forms and restrained
section dividers. S3, native Backblaze B2 and Proton Drive retain their distinct
fields and capabilities. Fresh configuration does not create a destination.
An editable destination name must remain shrinkable; never set its minimum width
from the entire value. Show its beginning when unfocused, while preserving normal
selection, context-menu copying and editing. Keep help and trailing actions
reachable by moving the action group below the name when needed. Stack credential
actions at narrow widths or enlarged text sizes without adding horizontal scroll.
Settings continue to autosave through the existing revision-checked path;
credentials require explicit Store and never enter configuration, autosave or
view checkpoints. Keep errors, conflicts, draft preservation and reload behavior.

**Image editor.** The image owns the working area. Tool properties live in a
fixed-width, scrollable panel so changing tools or selection does not refit the
canvas under the pointer. Crop, redaction, blur, arrows, shapes, highlights, text,
undo/redo and sanitized export keep their existing behavior. Document information
and actions stay visible at supported sizes. Plain Enter never uploads. See
[the image editor contract](image-editor.md).

**Recording.** The review remains frame-dominant with a compact transport,
timeline, trim and mask controls. Keep every mask lane and the existing timing
model. Labels and placement instructions over video need a contrasting backdrop;
mask outlines must remain visible over both pale and dark frame content,
independently of the app theme. The preview is silent; video export retains
recorded system audio where the format supports it. Save a copy… exports locally;
only explicit Upload enters the shared pipeline. The recording bar names Start
recording and Stop recording and exposes its actual state. See
[the recording contract](recording.md).

## Verification and evidence

Test shared control behavior and actual workflows rather than snapshotting copy
or duplicating token declarations. Preserve the existing minimum-size,
frame-share, canvas-stability, popup, hotkey, autosave and privacy regression
gates. Button centering checks must isolate label/icon ink rather than count
rounded-corner background pixels. A large-text check must verify that the text
is still enlarged after window construction.

Review both themes, narrow windows, long labels, 150% text and fractional DPR.
Include keyboard focus, menus, confirmations, disabled checked controls and live
theme changes. Check recording review's fit with its actual desktop decorations
on each native session, while retaining the existing image-area share gates.
Offscreen geometry alone cannot establish decorated desktop fit.

The synthetic UI catalog and widget tests establish design and widget behavior only.
Production acceptance needs the exact source/build on the declared desktops,
saved visuals, functional and memory evidence, and cleanup receipts under the
[acceptance matrix](../testing/acceptance-matrix.md) and [remote builder rules](../lab/remote-builders.md).
Prototype evidence never establishes production or release acceptance. Retain
failures and the corrected attempts under the [evidence retention rules](../testing/evidence-retention.md).
The [2026-10-26 feasibility receipt](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-26-graphite-design-feasibility.md)
records the prototype's exact scope, identities, results and preserved failures.
The separate [production review](https://github.com/patrick-hudson/put.gd-evidence/blob/874703b35562c732268c6fe0bc9b42614ffda928/records/2026-10-26-graphite-production/README.md)
records the implemented screens, package identities, desktop workflows, memory
measurements and remaining limitations for code PR #596.

The previous blue design notes remain in [design.md](../history/design.md); the earlier
redesign experiment remains [a historical record](../history/ui-redesign-experiment.md).
Those records, the 2026-10-26 receipts and other dated evidence keep the names
they were written with: Graphite, `Graphite.h`, `VisualStyle`, `RedesignTheme`
and `putgd-redesign-preview` became `putgd::Ui` (`Ui.h`), `Ui::styleWindow` and
`putgd-ui-catalog` on 2026-10-29.
