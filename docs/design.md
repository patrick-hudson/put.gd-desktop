# put.gd visual design

The native Qt interface uses a neutral charcoal canvas and surfaces with a restrained blue accent. Blue marks the primary action and keyboard focus; red marks destructive actions and errors. Text uses the desktop's installed sans-serif font. There are no downloadable fonts, web views or raster decoration assets.

Window headings name the task: Settings, Activity and History. Help describes an action or consequence in one short sentence. Routine status uses a neutral surface; errors retain their code and diagnostic text. Titles, section headings and controls use a consistent hierarchy, with 20-pixel window margins, 12-pixel gaps between sections and compact form rows.

Settings opens on editable S3 basics with an unsaved standard destination draft.
There is no Add prerequisite and no implicit development bucket. Bucket, searchable
editable region, optional public URL and masked credential inputs are visible.
Save settings, then explicitly Store credentials. Save locally selects local output
for all four actions in the draft; Save settings applies it. Advanced settings
expands the same validated draft into Capture, Destinations, Shortcuts and
Preferences. File-name modes and templates belong under Advanced. Every setting
has a detailed info tooltip and a keyboard-accessible help dialog created only on
request and deleted when closed. Labels name the setting; explanatory copy stays
in help. Footer actions are Cancel, Reload from file and Save settings. Cancel
discards the draft and clears unsaved credential inputs; Reload from file rereads
external configuration. An accepted save shows a distinct blue success banner,
while validation failures and revision conflicts remain visible.


Activity retains the operation/item hierarchy, operation IDs, URLs and independent outcomes. Its Details column fills the remaining table width. History retains the offline ledger, all filters, original receipts and destructive confirmations. Search and paging stay outside the table; the File details inspector scrolls independently and can show readable outcomes or exact receipt JSON. State badges and vector type icons use the same treatment in both windows. Long cells elide visually and retain their contents in tooltips or receipt details.

Editable controls keep their stable object names. Sidebar navigation, form inputs, table selection and buttons support keyboard focus. The layout targets a 1000×700 logical-pixel window. Qt handles display scaling in logical pixels, and icons are drawn at the display's device pixel ratio.

## Second-pass evidence and verification boundaries

The first-pass source and `work/lab-runs/kde-001/focus-obscured.png` establish the baseline: slogan headers, repeated introductory copy and tall stacked Capture cards. The witness is partly obscured by the synthetic fixture, so it does not establish full-window layout quality.

The installed second-pass Destinations witness (`ui-pass-two-destinations-head0.png` in the same lab directory) showed three full-width action rows consuming space above the destination fields. Add and Remove now share a compact toolbar, with explicit development-template import as its secondary action. Credential actions share a row as well. The Shortcuts witness showed duplicate introductory help, so the section now relies on the editor's binding explanation; desktop-support refresh uses a compact button. Preferences already used aligned fields and short checkbox labels, so its scrolling form remains unchanged. These follow-up changes require a fresh installed witness.

Existing Settings tests cover keyboard section navigation, compact-window sizing, validation and draft conflicts. The compact-window test now checks that all four capture buttons fit inside the Capture viewport without scrolling. Existing Activity outcome tests now also check that populated activity and its action buttons fit a 1000×700 window. Existing History tests cover the compact inspector with expanded filters/management, offline paging, removed-link behavior and cancellation of exact remote deletion.

Copy, colors and spacing do not get text/style snapshot tests: those would duplicate the implementation without proving usability. The replacement evidence is inspection of installed windows, including empty and populated Activity/History, all Settings sections, long destination labels, errors, conflicts, expanded disclosures, deletion confirmation and keyboard focus at supported display scales.

GUI tests prove widget behavior only. AE15 still requires real installed graphical inspection and retained screenshots; this document does not assert that the second pass has passed that gate or establish compositor support.

## Simple setup and readable sizes

The simple and Advanced views share the existing destination controls and validated draft. Expanding exposes endpoint/addressing, prefix, storage, ACL and timeouts, plus the existing Capture, Shortcuts and Preferences pages. Collapsing returns to the destination page without saving, reloading or discarding edits. Development-template import remains an explicit Advanced action; opening setup never creates or activates a development destination. Adding a standard S3 destination assigns only upload actions that have no profile, preserving existing upload assignments and local-only choices. Region changes update a standard regional endpoint while custom endpoints remain unchanged.

File-size inputs declare MiB (1,048,576 bytes). Exact terminating decimal values preserve every accepted byte on reload and save; fractional input is converted with integer arithmetic. Sub-byte, invalid and overflowing inputs reach the existing settings validator without replacing accepted settings. Activity and History use concise MiB/GiB summaries (GiB is 1,073,741,824 bytes), rounded for reading; exact receipt JSON retains the original byte counts. Transfer-rate controls retain their separately declared bytes/s unit.

Proof-first native KDE Qt 6.10.2 tests failed for missing Advanced disclosure and MiB size inputs before implementation. Added behavior tests cover simple setup and full configuration saving, keyboard expansion and draft/custom-endpoint preservation, fractional and odd-byte roundtrips, invalid size input, collapsed-view external conflicts, and readable Activity/History sizes. The root workflow reported all 25 host tests passing (40.47 s) and all three native KDE GUI suites passing (5.09 s) before the graphical follow-up. The installed 1280×800 simple-setup screenshot showed repeated destination headings and an empty profile selector pushing credentials below the viewport. The follow-up uses a compact destination toolbar, hides the selector for zero/one profile, and keeps it visible for multiple profiles or Advanced. Simple setup shows access/secret inputs and explicit Store credentials; storage mode, optional session token and secret-store reference are disclosed under Advanced. The same widgets and draft remain in use. New post-follow-up checks and installed screenshots remain root-owned. These changes still require installed graphical inspection and screenshots for AE15; no compositor acceptance is inferred from widget tests.


The selected history thumbnail is loaded lazily through one decoder at a time.
Rapid selections supersede pending work; stale results cannot paint another row.
Hiding History clears the pixmap and invalidates pending replies. Showing it again
reloads the selected preview. History pages retain metadata, never a decoded photo
per row. Operation payloads are released after disk persistence, before slow
uploads/local copies; retry ownership stays with the immutable artifact.

Region suggestions come from the [official AWS S3 endpoint list](https://docs.aws.amazon.com/general/latest/gr/s3.html) checked on 2026-10-08. Custom
regions/endpoints remain editable. The visual pass follows distinct surface and
primary roles described by [Material's color system](https://m3.material.io/styles/color/the-color-system), using the existing native Qt controls.
