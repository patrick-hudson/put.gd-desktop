# put.gd user guide

Open put.gd to reach one window with **History**, **Destinations**, and **Settings**. The tray's **Open put.gd** command and a click on the tray icon open it on History, which also shows current work; the tray's **Settings** opens it on Settings. Closing the window keeps the tray and accepted work running. The launcher reopens it.

The action bar across the top of the window has the same commands as the tray, in the same order and groups:

- **Capture region** (the lead button), then **Window**, **Monitor**, **Desktop** and **Scrolling region**: Capture window, Capture monitor, Capture desktop and Capture scrolling region. The arrow beside Capture region, or a right-click on it, has **Repeat last region**.
- **Capture region & edit** and **Record screen**. The arrow beside Capture region & edit, or a right-click on it, has **Edit clipboard image** and **Edit an image file…**; you can also drop a PNG or JPEG on that button.
- At the far end, **Copy text** and **Upload clipboard**.

The labels never shorten. When the window is too narrow for one row, the second group moves to a second row in the same order. From the keyboard, Alt+Down (or F4, the Menu key or Shift+F10) opens a button's arrow menu.

The tray menu lists **Capture region**, **Repeat last region**, **Capture window**, **Capture monitor**, **Capture desktop** and **Capture scrolling region**; then **Capture region & edit**, **Edit image** (from the clipboard or a file) and **Record screen**; then **Copy text from region** and **Upload clipboard**; then the **Capture delay** and **Capture output** submenus; then **Open put.gd**, **Settings**, **Update available** (when there is one) and **Quit put.gd**. While something is happening, rows at the top say what: a recording's state and **Show recording**, an upload and **Cancel upload**, or a failure to look at. **About put.gd** is in the window's menu (☰).

Each command starts after finished settings edits are saved. Screen captures close this window to keep it out of the capture. Clipboard imports keep it open. A save failure keeps the window open; retry saving and start the action again. Store or clear unfinished credential entry before a screen capture. Region capture supports Escape to cancel. Window capture targets the focused external app where supported; monitor capture takes the one monitor the pointer is on, at its full size; desktop capture includes enabled monitors and panels. See [One monitor, a delay, or the same region again](#one-monitor-a-delay-or-the-same-region-again). Clipboard import prefers copied local files, then an image, then plain text. Text URLs are imported as text. **Capture region & edit** selects a region and opens a small editor first; see [Edit before uploading](#edit-before-uploading). Each button's tooltip names its hotkey and its output, and **How capture actions work** in the window's menu explains them all. Capture is also available through hotkeys and the CLI.

## GNOME 50: log out once

On GNOME 50, put.gd's capture and hotkeys need a Shell extension that put.gd sets
up for you. After installing, whether by the one-line installer, a DEB or RPM, or an
AppImage, start put.gd and log out and back in once when it asks, then start put.gd
again unless you turned on Launch at login. There is nothing to run or download. If
something else is in the way, put.gd says what in plain words.

Copy text needs the extension at version 23 or later; these packages carry version 24, which also lets auto-scroll in a scrolling capture pause the moment you move the mouse. After an update, log out and back in once. Until you do, Copy text from a region reports the same log-out step, and every other way of using it keeps working: History's **Copy text**, the editor's tools and the CLI's `text.recognize` from a window, a monitor, the desktop, the clipboard or a file.

To remove the extension copy put.gd installed for an AppImage, quit put.gd and run
the AppImage with `--remove-gnome-extension` (the installer puts it at
`~/.local/bin/putgd.AppImage`; use your own path for a hand-downloaded one).

## Choose outputs

In **Settings → Capture**, select the default upload destination or **No destination (uploads off)**. Expand **Per-action exceptions** to choose an output for a particular action. **Use default** follows future default changes; an explicit exception keeps its own destination and sharing choice. Hotkeys remain independent.

In **Settings → General → Local copies**, **Keep a local copy** controls local saving independently of the upload destination. Set the main folder and optional image, video, text, and other-file folders there. Recording is not included; video files must already be finalized.

| Destination | Keep a local copy | Result |
| --- | --- | --- |
| Selected | Off | Upload only |
| Selected | On | Save locally and upload |
| None | On | Save locally only |
| None | Off | Report an output error before capturing or importing |

Local image results copy image MIME, text results copy text, and file results use local file URIs. Private uploads create no share link and do not change the clipboard. An upload failure does not turn an upload-only action into local saving; local-save and upload recovery remain separate. Existing files, accepted operations, and historical receipts are preserved when outputs change.

## One monitor, a delay, or the same region again

**Capture monitor** takes the monitor your pointer is on, at its full size. On a desk
with several monitors, Capture desktop gives one very wide picture of all of them;
this is the one in between. It has its own output and its own hotkey, both set like
the other captures' (the hotkey starts unassigned).

**Capture delay** waits 3, 5 or 10 seconds before a region, window, monitor or desktop
capture, and before Repeat last region, Capture region & edit and Copy text from region,
so you can open a menu, hover a button or hold a key first. Scrolling capture and
recording never wait. Choose it in **Capture delay** at the bottom of the window (on
every page), in the tray's **Capture delay** submenu, or in **Settings → Capture →
Capture delay**; all three show the same choice, and it stays until you set it back to
None. While a delay is set, the window's control is highlighted and the buttons that
wait say "Waits 5 seconds first" in their tooltip. A capture you start with a hotkey is immediate unless you
turn on **Also delay captures started by a hotkey** in the Settings section; the delay
menus then say **Hotkeys capture at once** under the choices. A small pill at the top of the
screen counts down. It doesn't take the keyboard and you can keep working around it;
click it to cancel, and nothing is captured. It is never in the picture.

- For a region you choose the area first; the countdown runs afterwards. With
  **Screenshot first** region selection, the countdown runs first and the screen is
  frozen when it ends.
- A delayed window or monitor capture takes the window that is active, or the monitor
  under the pointer, when the countdown ends.
- A capture started with a hotkey is immediate unless **Also delay captures started
  by a hotkey** is on.
- If the screen is locked when the countdown ends, nothing is captured.

**Repeat last region** captures the same rectangle as your last region capture, with
no selection step: handy for a series of the same area. It's in the tray, under the
arrow (or right-click) of **Capture region** in the window, and has its own hotkey (unassigned to begin
with). It uses Capture region's output. put.gd forgets the rectangle when it quits,
and says so instead of capturing when there is nothing to repeat yet or the monitors
have changed since.

## Upload from your file manager

Right-click files or folders in your file manager and choose **Upload with put.gd** (in Thunar it is under **Send To**; with the AppImage, Files and Caja list it under **Scripts**). They upload right away to your capture destination (or your only destination; with neither, Settings opens on Destinations), shared and copied as a capture is. Folders upload as one `.zip` by default; **Settings → General → File manager → Folders** can upload each file instead. The AppImage's entries switch off under **File manager menu**. After installing the DEB or RPM, Files, Caja and Peony show the entry after a restart; put.gd's first notice offers one. See [file-manager-upload.md](../design/file-manager-upload.md).

## Edit before uploading

Choose **Capture region & edit** from the action bar or the tray, or assign it a key in **Settings → Hotkeys** (it has no default key). Drag the region to edit; Escape cancels. To edit something you already have, use **Edit image** at the top of History (the clipboard image or a PNG/JPEG file), use the arrow beside the action-bar button (or right-click it), or drop a PNG or JPEG on that button. **Edit again** on a History item reopens its local copy when **Keep a local copy** was on; the edited result is uploaded as a new item. In the editor:

- **Redact** (X) covers an area with opaque black and removes those pixels from the uploaded image. Use it for passwords, tokens, names and other private information.
- **Blur** (B) is only a visual effect; it can leave enough detail to guess text. Its **Pixelate** style draws square blocks instead, and pixelation may be reversible too. Prefer Redact for anything private.
- **Crop** (C): drag the area to keep, then **Apply crop** or Enter; Escape cancels.
- **Arrow** (A), **Line** (L), **Rectangle** (R), **Ellipse** (E), **Pen** (P), **Highlight** (H) and **Text** (T) add simple marks; Shift keeps a line at 0°, 45° or 90° and makes squares and circles.
- **Step number** (N) drops numbered discs: each click places the next number. Change **Next number** to start elsewhere; deleting one leaves the others as they are.
- **Copy text** (G), the last tool, reads the text in the image so you can select and copy it. See [Copy text in the editor](#copy-text-in-the-editor).
- Any drawing tool picks up a mark you can see under the pointer; hold Ctrl to draw over it. Select (V) moves, resizes or deletes them. Highlight looks like a marker on light and dark pages alike (text stays readable) and snaps to text lines: click a word, double-click a line, hold Alt for a free box.
- Ctrl+Z / Ctrl+Shift+Z undo and redo; Ctrl+1 shows 100% to check edges; Ctrl+0 fits.

**Upload** (Ctrl+Enter) sends only the edited image to the output shown at the bottom of the window, with the usual local copy, history and link copying. Plain Enter never uploads. **Save a copy…** writes the same edited PNG to a file, and **Copy image** (Ctrl+Shift+C) puts it on the clipboard to paste elsewhere; neither uploads or adds anything to History. **Cancel** or closing the window discards the edit: nothing is saved or uploaded. Inputs above 24 megapixels are refused; output is PNG. An edit interrupted by quitting put.gd is listed as interrupted and is never uploaded automatically.

## Capture a scrolling region

Use **Capture scrolling region** to get one tall image of something longer than the screen: a web page, a document, a chat, a settings list. Choose it from the tray or the main window (its button reads **Scrolling region**), or give **Capture scrolling region** a hotkey in **Settings → Hotkeys** (it has none by default). Drag a rectangle over the scrollable content, and include its scrollbar. There is no Start button: put.gd begins as soon as you let go.

1. **Scroll the page yourself.** The bar says **Scroll the page**. Use the mouse wheel, the touchpad, the keys or the scrollbar, and stop or go back up as you like: the image follows the page and grows as it moves. put.gd sends nothing and does not touch your pointer.
2. **Or let put.gd scroll.** Press **Auto-scroll** on the bar, and put.gd turns the wheel for you, in small steps, and finishes by itself at the end of the page. **Moving the mouse pauses it**: the bar says **Paused: you moved the mouse**, and the image keeps following whatever you do. Press **Auto-scroll** again to let it go on. To start every capture this way, turn on **Settings → Capture → Scrolling capture → Start with auto-scroll**.
3. **Finish.** Press **Done** when you reach the end, or press the scrolling capture hotkey again. On X11 (Cinnamon) **Esc** finishes it too; the bar names the key you can use. **Cancel** throws everything away: nothing is saved, uploaded or added to History.

The bar sits beside your rectangle. When there is no room (the rectangle fills the screen), it sits inside the rectangle's bottom-right corner, and none of it ends up in the image: the page behind it is taken from moments when it was clear. Only at the very end of the page, the corner behind the bar was never clear, and that patch is plain.

The result is one PNG that goes to the output chosen for this action (it starts out as **Capture region**'s; change it under **Per-action exceptions**): local copy, upload, History and link copy. To edit it first, an agent or script can run `editor.open {"source":"scroll"}`, which opens the image in the editor and uses **Capture region & edit**'s output.

**What the bar says.** **Following** while the page moves. **Lost the page** with **Scroll back a little** when the page moved somewhere put.gd cannot place it (you jumped further than a screen, or the page changed under you): scroll back to where you were and it finds its place again. **Auto-scrolling** while put.gd turns the page, **Paused: you scrolled** when you scrolled up during it, and **Nothing scrolled here** when its wheel moved nothing. **Auto-scroll lost the page** means auto-scroll went back five times and could not find its place, which happens when a page rearranges itself. Scroll back a little. When the bar says **Found the page again**, scroll on yourself or press **Auto-scroll** to carry on. The height so far is beside it.

**Scroll to the top first.** With **Settings → Capture → Scrolling capture → Auto-scroll goes to the top first** on (it is off by default), auto-scroll takes the page to its top before it starts, when you start it before scrolling yourself, so the image begins at the top. If it cannot reach the top (the page keeps loading more above, never holds still, or takes longer than a minute and a half), the image starts where the page was, and a warning says "Could not reach the top of the page. The image starts partway down." History carries the same note.

**Height cap.** An image stops at the **Maximum height** in **Settings → Capture → Scrolling capture** (2,000 to 60,000 px, 30,000 by default), or earlier when the image would pass the size limits. The image so far is kept and shared as usual, and a warning says "Stopped at 30,000 px. The page continues below." History carries the same note. If the upload or the local save failed, History shows that failure first and keeps the note after it.

When put.gd loses the page while you scroll and finds it again, the capture says so in a notice and History; check the image there because rows may be missing.

**Lost at the end.** If you press Done while put.gd has lost the page, the image ends where it last found it, and a warning says "Finished while put.gd had lost its place. The image ends where it last found the page."

**Left alone.** A capture with nothing to follow and no button pressed for 10 minutes finishes by itself. If the page was scrolled during the capture, it keeps the image, and History notes "Finished by itself after 10 minutes without scrolling." If nothing was ever scrolled, it ends as if you had pressed Cancel: no picture is saved, copied or uploaded, and no History row is added. The capture's view says "Nothing was scrolled for 10 minutes, so the capture ended without an image." The way to the top does not count as scrolling.

Tips:

- A sticky header, a footer, a side bar or a floating box is in the image once, as a browser's own full-page picture has it, not beside every screenful.
- Things that change in place (a video, a carousel, a hover effect) are in the image as they were first seen.
- A blank stretch taller than your rectangle shows nothing that moves: auto-scroll ends the capture there and keeps the image so far, and put.gd says in a notice and in History that it stopped where the page showed nothing. To get the rest, start another capture below the blank part. In a capture you scroll yourself, put.gd loses its place when you scroll past such a stretch: scroll back until the bar follows again, or press Done to keep the image so far.
- With the pointer over the page while you scroll, a link's address or a tooltip can end up in the image. Auto-scroll keeps the pointer outside except for each wheel step.
- put.gd closes its own notifications (including the file-manager notice and the fallback balloon) when a capture starts and holds new ones until it ends. If one cannot be closed the capture does not start (`notification_close_failed`): dismiss it and try again. A notification from another application over the rectangle can end up in the image, so close it first.
- The region has to lie on one monitor and be at least 64 px each way.
- GNOME shows its screen-sharing indicator while a capture runs. Clicking it finishes the capture like Done. Scrolling capture needs the put.gd Shell extension at version 22 or later: after an update, log out and back in once, or it reports an error until then.
- On Wayland (Plasma, GNOME, Hyprland, Budgie) there is no Esc for a capture: bind a hotkey to **Capture scrolling region** if you want a key that finishes it.
- On the Ubuntu 24.04 package for KDE Wayland there is no region selector, so the window's button says scrolling capture is unavailable (`region_unavailable`); use the AppImage. A script cannot capture there either: with no selector, scrolling capture is unavailable for every caller, including a rectangle named through `putgd-cli` or MCP.
- A new capture can start as soon as the image is finished, while it is still waiting to upload.
- If the size of the captured area changes during a capture, it ends with `display_changed` and keeps nothing.

## Copy text from the screen

Use **Copy text** to copy the text out of something you can see but cannot select: a screenshot of a terminal, an error in a dialog, a slide, a video frame, a PDF that is only an image. Choose **Copy text from region** in the tray, **Copy text** in the main window, or give **Copy text from region** a hotkey in **Settings → Hotkeys** (it has none by default). Drag a rectangle over the text. Escape cancels. A capture delay works as it does for a region. The selector looks the same as for any other capture. It works on Cinnamon, GNOME, Plasma, Budgie and Hyprland, and on GNOME and Plasma sessions that run on X11.

put.gd reads the region on your computer, in a short-lived helper that has no network access and cannot write a file or start a program. It then puts the text on your clipboard as plain text and tells you with a notification:

| What it found | On the clipboard | The notification says |
| --- | --- | --- |
| Text | The text | "Copied 214 characters" |
| Text and QR codes | The text, with what each code holds on a line of its own | "Copied 214 characters and 1 code" |
| Only QR codes | What the codes hold, one per line | "Copied 1 code" |
| Nothing | Unchanged | "No text found" |
| You copied something else meanwhile | Unchanged | "Text not copied: the clipboard changed" |

Codes that put.gd reads are QR, Micro QR, Data Matrix and Aztec codes. A code that holds binary data rather than text is neither copied nor counted. A very small rotated Micro QR code (about 3 pixels a module), and a code with no white border around it, may not be read. put.gd never opens a link from this action.

- **Nothing is saved or uploaded.** The action makes no file, no upload and no History item. The text is not written to put.gd's logs, settings or diagnostics, and the notification never shows it. A failed attempt is listed in History as "Text recognition failed" with its reason. Your clipboard manager is another matter: put.gd does not mark the copy as private, so a clipboard manager such as Klipper may keep the text. Clear it there if the text is sensitive.
- **It reads what is on the screen.** Text a person could not read, such as near-white text on white or glyphs under about 5 pixels tall, is left out. Stray symbols that icons produce are left out. Check what you pasted: recognition can make mistakes, especially with unusual fonts, small type and mixed columns.
- **Notifications off.** The copy still happens. You just get no message.
- **One at a time.** Pressing it again while it is still reading says "Text recognition is still running" and changes nothing.
- **How long it takes.** Reading costs roughly a millisecond a character on each processor core. A paragraph takes about half a second. A region packed with text takes several seconds, and a whole 4K screen takes more. On a 2015 low-power CPU (the m74 test machine), the final build took these times at the 95th percentile over 30 runs: a paragraph 0.66 seconds with the native package and 0.52 with the AppImage, 8,739 characters of terminal text 3.2 and 2.0 seconds, and 4K screens 6.4 to 7.9 seconds and 2.7 to 4.1 seconds. A faster processor with more cores is quicker. A 4K screen is read window by window, each at its own scale. These times are recorded, not promised.
- **Very long text.** An image over 24 megapixels is refused. A capture with more text than put.gd can read in about 16 seconds, such as a full 4K screen of small dense type, returns what it read so far, in reading order, and the notification adds "The text was cut at a size or time limit." It does not fail. Select a smaller region to read the rest. A copy holds at most 1 MiB of text.
- **If it will not run.** The button and the tray entry say why. The usual reasons are missing English data, a missing `putgd-ocr`, or a kernel too old to confine the helper. See [text recognition failures](errors.md#text-recognition-and-qr-codes) for each one and what to do.

### Languages

English comes with put.gd. By default put.gd reads the language of your desktop when its data is installed, then English. To read others, add the language data and choose up to three languages in **Settings → Capture → Text recognition**: **Language**, **Second language** and **Third language**. **Automatic** names what it will use. A language whose data was removed stays chosen and says "(not installed)", and put.gd skips it when another loads, which the notification mentions, for example "(German data is missing)".

You can add data in two ways:

- Install your distribution's Tesseract language package. For German that is `tesseract-ocr-deu` on Debian, Ubuntu and Mint, `tesseract-langpack-deu` on Fedora and `tesseract-data-deu` on Arch. Some of these are larger models than English needs, and put.gd loads only the part it uses.
- Or put the language's `.traineddata` file, such as `deu.traineddata` from the `tessdata_fast` repository, in `~/.local/share/putgd/tessdata/`. Every package kind reads that folder, and a file you put there wins over the system's copy of the same language.

put.gd does not download language data. Reading several languages at once takes more memory, and put.gd refuses more than three or more than 64 MiB of data.

### Copy text in the editor

Choose the **Copy text** tool (G) in the editor, the last one, after Blur. put.gd then reads the image, and the panel says "Recognizing…" and then how many words it found, for example "214 words", or "No text found". Nothing is read until you choose the tool.

- Press on a word and drag to another to select every word between them, in reading order. The words are tinted. Shift+click extends the selection, double-click takes a line and Ctrl+A takes everything. Ctrl+C copies the selection while the canvas has focus. Escape clears the selection, and a second Escape returns to Select.
- **Copy selection** and **Copy all text** are in the panel. Copy all text includes what any QR codes hold, as the action does.
- The panel lists each QR or other code it found, with **Copy**. A link in a code shows in full, with its host spelled the way a browser will use it, so you can check it before you press **Open link**, which opens it at once in your browser. Only http and https links open. put.gd got the link from the screen, so read it first.
- It reads the original image inside your crop with every Redact already applied. Marks you added, such as arrows and text, are not read, and **Blur does not hide text from it**. A word under a Redact is never copied, and neither is a code.
- If you add, move or remove a Redact, or change the crop, the old result is thrown away and the tool reads again.

**Quick redact** is a menu in the Redact tool's panel: **Email addresses**, **Phone numbers**, **IP addresses**, **Card numbers**, **Keys and tokens**, **My username and name**, **My redaction list**, **All of these** and **Remove suggestions**. It adds ordinary Redact marks over matching text, as one step you can undo, and says "Added 3 suggested redactions" or "No matches found. Text can still be there." Hover **Keys and tokens** to see the shapes it knows: AWS access key IDs, GitHub, GitLab, Slack and Stripe tokens, JWTs, private-key blocks, and long unbroken hex or base64 runs. Recognition often splits or misreads a long token, and Quick redact copes with that. A mark reaches about a character and a half beyond the text it covers on each side, and it can take in an ordinary word that is joined to a token, such as a label like "sha256" or the words after a token. A version number such as 26.1112.4660 is not suggested as a phone number, and neither is a long run of digits unless it starts with "+". It also marks a long run of one or two repeated letters, such as a masked secret shown as a row of x's.

**My username and name** covers your own login name and real name, which a screenshot of a terminal, a file manager or a chat window often shows: a prompt like `patrick@d69:~$`, a path like `/home/patrick/Pictures`, "Signed in as Patrick Hudson". put.gd reads them from your account on this computer and keeps them in the editor's memory. If the lookup is slow, the other kinds still run after about a second and the result says "My username and name was not checked: the account lookup did not answer in time." Your patterns see the screen text, which can include your name, and run in a separate small process. It ignores case and accents (accents only on Latin, Greek and Cyrillic letters). It looks the account up in the background, only when you choose it. For the name `Patrick Hudson` it covers `Patrick Hudson`, `patrick-hudson`, `patrick_hudson`, `PatrickHudson`, `Hudson, Patrick`, `phudson`, `p.hudson` and `patrick.hudson` before an `@`, each part alone (`Patrick`, `Hudson`), and your login with up to four digits after it (`phudson2`). It does not match `Patricks`, `pattern` or `pat`, and it skips a name part shorter than three letters on its own. A middle name is ignored in `Hudson, Patrick`, and your login follows the same rules as a name, so `jane.doe` also covers `jane-doe`. If your account has no real name, only the login is matched. **Hudson** in "Hudson Bay" is covered too, since it is your surname.

**My redaction list** covers words and patterns you keep in **Settings → Capture → Text recognition**. It is off, and its tooltip says where to add words, until you save one.

- **Always suggest redacting** takes one word or phrase per line, up to 200 lines of up to 100 characters, each with a letter or digit. A hostname, a client's name or a project code name are the usual entries. It ignores case, and accents on Latin, Greek and Cyrillic letters, and matches inside longer text such as a path, so `acme` covers `/srv/acme/data`, but not `acmes`. A phrase matches with nothing between its words, with up to two of space, `-`, `_` or `.` between them, or with a space, one of `-`, `_` or `.` and a space, so `Project Aurora` covers `project-aurora`, `project--aurora`, `project__aurora` and `Project - Aurora` but not `Project -- Aurora` or `Project - - Aurora`. Punctuation at the start or end of an entry stays part of it, so `.env` does not cover `env`. `Straße` matches `STRASSE`, but `kır` and `kir` stay apart. An entry matches only whole: `Patrick Hudson` does not cover `Patrick` alone, so add `Patrick` on its own line if you want that. An entry is not a regular expression, so `*`, `?` and brackets mean themselves. A `.`, `-` or `_` inside an entry (or a space, or a change from lower to upper case) separates its parts, so `srv.data` also covers `srvdata`, `srv_data`, `srv-data` and `srv data`. Only punctuation at the start or end of an entry stays literal.
- **Patterns (regular expressions)** takes one pattern per line, up to 50 lines of up to 500 characters as you type them (spaces count), for shapes a word cannot say: `\bJIRA-\d+\b` for ticket numbers, or `(?i)\b[a-z0-9-]+\.corp\.example\b` for internal hostnames. Case matters unless the pattern starts with `(?i)`. A pattern runs with its meaning unchanged: put.gd only puts the regular-expression engine's own match and depth limits in front of it, which can stop a search but never change a match. It sees the recognized text as it was read, with no case, accent or digit folding: each visual line with its words one space apart, and each pair of neighbouring lines joined with a line break. So a pattern that has to match across a wrapped line uses `\s` (for example `quick\sredact` or `JIRA-\s*\d+`); a plain space does not match the break, and nothing spans three lines. The checks that run when you save compile the pattern as you wrote it. A pattern that does not compile, or that would match empty text, is shown with its reason and position beside the box, and Settings does not save until it is fixed. Your patterns run in a separate small process that put.gd stops after about 2 seconds, so a pattern that takes too long can never freeze the editor. A pattern that does not finish, in time or because it ran into the regular-expression engine's own limits, is skipped, and the result says "Pattern 3 didn't finish and was skipped." (the first pattern is number 1). The marks your words, your name, the other kinds and the other patterns found stay. That process cannot write files, and it ends when you close the editor.
- The list stays in your configuration file on this computer. put.gd never uploads it and never writes it to its logs, and a diagnostics package holds only how many entries there are.

**A mark covers the whole word box**, not only the letters that matched. `/home/patrick/project` is covered from end to end, and the words next to a match can be covered when recognition joins them. This keeps part of a name from showing in a proportional font.

**Editing suggestions.** Quick redact adds ordinary Redact marks. Select one with the Select tool (V) to move it, resize it or delete it with Delete. The panel says "Suggested: My username and name" (or the kind) for a mark Quick redact added. Run Quick redact again and it does not add back a suggestion you deleted: a new suggestion of the same kind is skipped when its box overlaps the deleted one by at least half of their combined area, while a different kind, or a clearly different box, can give a new mark. Undo the delete and it returns. **Remove suggestions** takes away every suggestion that is still there, as one undo step, and never touches a mark you drew yourself. Undo after Quick redact removes all of that run's marks at once.

**Quick redact only suggests. Recognition can miss text, and a page with no matches can still hold some.** A misread name, such as one letter wrong in a short name, can be missed too: names and words are matched as spelled, not by closeness. Look at the result yourself before you upload. Redact is what removes the pixels, and Quick redact only places Redacts for you. A number made of repeated digits can be counted wrong, so a card number like that can be missed. If you submit or close the edit while Quick redact is still working, it adds nothing and says "Quick redact added nothing: the edit was already submitted."

### Copy text and show a QR code from History

- **Copy text** in an image item's inspector and ⋯ menu does the same as the action, reading the item's local copy. It is off, and says why, when the item has no local copy (turn on **Keep a local copy** before capturing) or is not a PNG or JPEG image. put.gd never downloads an upload to read it.
- **QR code** in an uploaded item's inspector and **Show QR code** in its ⋯ menu open a window with a QR code of the link **Copy link** would copy, so a phone can scan it. An expired link that can be refreshed is refreshed first. **Copy image** puts the code on the clipboard and **Save…** writes it as a 1,024 pixel PNG. It is off, and says why, for a private, removed or revoked link and for an expired link that cannot be refreshed. A link over 2,331 bytes is too long for a QR code, and the window says so. **Save…** never replaces a file without asking.
- History search is still by name only. put.gd does not read the text in your captures unless you ask.

## Record the screen

Choose **Record screen** in the action bar or the tray, or assign the record hotkey in **Settings → Hotkeys**. Drag the area to record on X11, Plasma, GNOME (with the put.gd Shell extension), Budgie and Hyprland; other Wayland desktops ask once which screen or window to share. The floating bar chooses MP4, WebM, GIF or WebP, system audio and the microphone, then records, pauses and stops. Stop always opens a private review: trim the ends, cut out a stretch, redact or blur part of the picture, undo, play it back, **Save a copy…**, **Discard…** or **Upload** (see [Review a recording](#review-a-recording)). Nothing is saved or uploaded until you choose one. See [screen recording](../design/recording.md) for the details.

To narrate a recording, tick **Microphone** on the bar. The bar beside it moves with your voice, so you can see the microphone is live before you press Record; put.gd opens the microphone only while that box is ticked and lets go of it when the recording stops. The choice is remembered for next time. put.gd uses the system's default microphone unless you pick another in **Settings → Capture → Screen recording → Microphone**. If no microphone can be opened, the box reads **No microphone** and is greyed out (hover it for the reason). A microphone plugged in while the bar is showing is offered within a second or two, with no need to start over; if **Microphone** was ticked last time, it is ticked again. A microphone unplugged during a recording leaves the recording running, silent from there. With **System audio** ticked as well, both are in the clip. GIF and WebP have no sound.

**Settings → Capture → Screen recording → Click highlights** draws a ring at the pointer each time you click, so a viewer can see what you pressed. On X11, GNOME and Hyprland the ring is drawn into the clip only. On KDE Plasma put.gd switches on KWin's own Mouse Click Animation effect while it records, so you also see the rings on your screen, and they appear in screen and region recordings but not in window recordings. With Budgie the rings are drawn into the clip for screen and region recordings, for clicks made on application windows; clicks on the panel and clicks made with unusual modifier combinations are not highlighted, and window recordings have none (the full list is in [Budgie](../desktops/budgie.md#click-highlights)). Where the desktop cannot report clicks at all, for example Hyprland with a configuration put.gd cannot add binds to, or GNOME while an older put.gd extension is still loaded, the switch says why and recordings start without rings. **Settings → Advanced → Desktop support details** says what your desktop offers.

Recording needs an H.264 encoder. When none works, put.gd says so and what to install instead of starting:

- **Ubuntu, Kubuntu, Mint and Debian packages** bring one with their GStreamer dependencies.
- **Fedora**: Fedora installs a placeholder OpenH264 library that the RPM's weak dependencies do not replace. Replace it explicitly with Cisco's: `sudo dnf install openh264 gstreamer1-plugin-openh264`. The one-line installer runs this codec step for you when the repository is enabled and the placeholder remains or either codec package is missing. If the `fedora-cisco-openh264` repository was turned off, it prints the commands instead: first run `sudo dnf config-manager setopt fedora-cisco-openh264.enabled=1`, then the codec install.
- **AppImage**: it carries its own recorder. It encodes with your graphics driver where it can (VA-API or NVIDIA) and otherwise with its bundled x264 and x265, so it needs no setup.

In testing on an AMD Radeon 740M with KDE Plasma, hardware and software encoding were equally smooth at 60 fps. On that setup, a notification with a countdown bar made a 60 fps recording less smooth while it was on screen, about 50 frames per second, with either encoder. Other GPUs may differ.

### Review a recording

Stop opens a private review window (1440×900 where the screen allows; on a smaller screen the side panel folds into an **Edits** button). Nothing is saved or uploaded until you choose **Save a copy…** or **Upload**. A recording that needs no changes can go straight to **Upload**. Closing the window asks whether to discard the recording, as **Discard…** does.

- **Look around.** Click the timeline to move the playhead, press Space to play, and use `,` and `.` to step one frame. The clock shows where you are in what you will upload and how long that is; hover it for the time in the original recording. Hover the timeline while paused to preview that moment without moving the playhead: a small picture from the keyframe just before that moment appears, **without any covers on it**, but only when that keyframe's thumbnail is already loaded; otherwise the window keeps the picture it had (which can be an earlier hover picture) until the exact frame, with the covers for that moment, arrives a fraction of a second later. The preview is silent.
- **Trim the ends.** Drag a bracket at either end of the timeline, or put the playhead where you want the clip to start or end and press **I** or **O**. The frame on screen at the playhead stays in the clip when you set the end; if that frame is not shown yet, put.gd decodes it first, and if it still cannot, it asks you to try again.
- **Cut the middle out.** Drag across the timeline to select a stretch of time, then choose **Cut** or press **C** or Delete. The cut stays on the timeline, hatched, and playback skips it. Select a cut and choose **Restore** to put it back, or **Play across** to see how the join looks. Drag the edges of a cut to adjust it.
- **Cover part of the picture.** Select time if the cover should last for only part of the clip (with nothing selected it covers the whole recording), choose **Redact** (X) or **Blur** (B), and drag a box on the picture. **Redact** removes the covered pixels from the saved and uploaded clip, so use it for passwords, names and anything private. **Blur** can sometimes be reversed, and the window warns you of that when you pick it. Select a cover to retime it: its panel has **Shows from** and **Until** fields with **Set to playhead** buttons, and the arrows beside them jump to its first and last visible frame.
- **Undo.** Ctrl+Z undoes the last edit and Ctrl+Shift+Z or Ctrl+Y redoes it, up to 200 steps. A drag or a run of arrow-key nudges counts as one step.
- **The Edits list.** Once you have changed something, the panel lists every trim, cut and cover in order. Select a row to go to it; hover a row for its action (Reset, Restore or Remove). With time selected, Delete on a row cuts that time rather than removing the row's cover.
- **Keys.** A text field keeps its own keys. Otherwise V, C, X and B choose the Select, Cut, Redact and Blur tools; `[` and `]` (with the timeline focused) set the start and end of the selected time; arrows move the playhead 0.1 s, Shift+arrows 1 s, except that with the picture focused they move or resize the selected cover (or pan the picture at 100 %); Home and End go to the first and last frame of the selected cover, or to the ends of the clip; Alt+← and Alt+→ move between edits; Esc cancels a box you were placing or clears the selection. **Enter** presses the focused button, or opens the focused row of the Edits list; while you are placing a cover it places a centred one; with a cut selected it restores the cut; otherwise the first press uploads. A held key's repeats are ignored, so holding Enter never acts twice. Ctrl+Enter uploads. You cannot start an upload while you are placing a cover or in the middle of a drag (pressing **Upload** then does nothing). Once an upload is waiting, starting a drag or a placement does not cancel it: it uploads what you had when you pressed **Upload**; see below for what does cancel it.
- **Format and destination.** **Format** chooses MP4, WebM, GIF or WebP, and, with **Include system audio** (or microphone), whether the clip carries the sound that was recorded; turning it off removes it from the saved or uploaded file. GIF and WebP have no sound and a length limit that counts only the time you keep, so a cut can bring a long clip under it. They are no longer than the time you keep, except that a clip with less than one frame's worth of kept time still has one frame, and a GIF stores times in hundredths of a second, which can make it a little longer. The menu beside **Upload** shows what Upload will produce, such as a shared link or a private upload.

**What a cover and a cut do in the saved file.** The saved or uploaded file covers a frame when the frame is on screen during a cover's time, or when the picture it shows was captured during the cover. That includes a picture that stays on screen across a still stretch of the recording, and a picture held across a cut: after a cut, the first kept moment can show the picture that was on screen then, which may come from inside the cut, and it is covered if a cover was active for it. Sound from inside a cut is not kept, with one small exception: the sound in the file is only what lies in the time you kept, and where the video needs a little extra at an edge, it is silent. When the recording's sound has a one-millisecond timestamp gap, up to about a millisecond of sound near a cut edge can remain, softened by the fade between the two sides but not guaranteed silent. The preview in the window is not identical: the paused preview and the hover frame check the moment you point at and the moment the picture was captured, so they can show a later picture where the saved file holds an earlier one, and **while playing**, each frame is covered only for its own moment, so a cover that lies wholly inside a long still stretch may not appear on the still picture. **Before you upload sensitive footage, open the saved copy and check it** (use **Save a copy…** first), including the frames next to each cut and each cover's first and last moment. A **GIF** times its frames in hundredths of a second, so a picture can stay on screen a little longer than its nominal time; the cover applies to that longer display too within a stretch you kept. The one exception is a cut seam: the picture just before a cut can stay on screen up to a hundredth of a second into the next stretch, where a cover starts, because the cover starts in that next stretch. That picture was captured before the cover, so it shows nothing covered. Look for that, and for the one-millisecond sound residue above, at a cut edge.

**Upload waits for your last change.** **Upload** and Ctrl+Enter send the version you see: they send any change you are still making, wait for put.gd to accept it, and then upload that version. While they wait, only a change to what would be uploaded cancels the upload: a later edit, undo or redo, or a change of format, sound or size. So does put.gd refusing any change while it waits (even a later one), a change that put.gd itself refuses before sending it, or put.gd taking more than 2 seconds to answer. Clicking around, opening menus or hovering does not. A second press of **Upload** while it waits does nothing and says "Upload already waiting for your last change to be saved." (that note goes away once the upload is sent). When an upload is cancelled the status line says "Upload cancelled: … Press Upload again." (it is not a dialog, and it stays until you press **Upload** again). Nothing is uploaded from a version you did not see, and if the format or sound changed under it the upload is refused rather than sent. If an upload fails, **Try again** retries that same version with the same format and sound; if you have edited or changed the format or sound since, press **Upload** instead (it says "Press Upload again"; a failed **Save a copy…** likewise needs the same format and sound, and says "Press Save a copy again").

## Destinations and credentials

Use **Add destination** to configure S3, Backblaze B2, Proton Drive or a [custom uploader (HTTP)](custom-uploader.md), which takes a ShareX `.sxcu` file. Valid non-secret fields save automatically. Removing a destination removes it from settings and clears affected output references; original receipts, local files, and remote objects remain. Review the default and exceptions after removal.

To change the order of destinations, drag one up or down the list; a line shows where it will land. From the keyboard, select a destination and press Alt+Up or Alt+Down, or use **Move up** / **Move down** in its ⋯ menu. The new order saves right away and is also the order of the Destination dropdowns. It changes nothing else: the default, per-action exceptions and history stay with the same destinations, and unsaved edits stay with the destination you were editing. If the settings file changed elsewhere, the previous order comes back and Settings asks you to reload.

For S3, enter the bucket and region, then choose public links or **Private bucket: links that expire**. Expiring links last 1–7 days, use the endpoint host rather than Link domain, and never request a public ACL. Temporary credentials may expire sooner. Public links retain the link domain and public-read/omit-ACL choices. Searchable regions accept custom values. **Advanced connection settings** contains endpoints, addressing, storage class, and transport limits. The development template is an explicit import, supplies no credentials, and changes no bucket policy. **Test upload and link** runs only when pressed, uses synthetic content, and shows progress and its result below the destination title. Success means the test upload, link verification and cleanup all passed. In S3 presigned mode the signed link must work and unsigned access must be denied. Errors give the next checks for credentials, permissions, destination settings or connectivity; a denied object check can be ambiguous. Resolve uncertain cleanup through its retained test identity.

S3 credentials are saved only when you choose **Save to keychain** or **Use for this session**; session credentials expire when the resident exits. Configuration contains references rather than secret values. By default put.gd reads saved keychain credentials once when it starts (**Settings → General → Credentials**); if your desktop keyring is locked, it shows its own unlock prompt then. Turn that off to unlock only with **Unlock saved credentials**. Uploads and hotkeys never unlock the keyring. After loading, the form shows only the first four access-key ID characters and whether the secret/token exists; short IDs are fully masked. **Show** beside the secret access key reveals either your typed value or the unlocked saved secret. If saved credentials are not loaded, choose **Unlock saved credentials** first. A revealed saved secret is read-only; **Hide** clears it so you can enter a replacement. Changing destination or provider, leaving Destinations, closing, and reloading hide saved secrets. Store, ordinary close, and reload clear credential inputs.

For Proton, choose **Connect account…** and finish sign-in in the browser. The connected email stays visible when switching destinations. Choose a top-level My files folder, create a new folder, or press **Refresh folders** beside the selector. Folder queries show **Loading folders…** and preserve the known email and selection. Account setup never uploads a file. Configure private/shared output and advanced new-link expiration/password settings as described in [Proton Drive](proton-drive.md). Proton links open a share page; they are not direct image-embedding URLs. Safe remote deletion and account sign-out are not available in this integration.

## Settings and autosave

Settings save automatically after a finished field edit, toggle, or selection. Focusing and leaving an unchanged value does not save. The footer explains autosave without routine success banners. Invalid fields show specific feedback while accepted settings stay active; independently valid changes may still save. A write failure keeps the draft and offers **Retry saving**, rather than replaying the request automatically.

Accepted external changes refresh a clean form. A dirty draft instead reports that settings changed elsewhere and offers reload. **Settings → Advanced → Config file → Reload from file** discards the draft and rereads configuration; an invalid external file leaves the last accepted settings active. The config section shows the canonical path and accepted revision. See [configuration.md](../design/configuration.md) for fields, units, and cooperative locking.

**Settings → Capture → File names** keeps the original names of copied files by default, or uses random names/custom templates. Capture, clipboard-image, clipboard-text, and copied-and-uploaded-file (**Copied & uploaded files**) patterns have separate help. Local collisions preserve files by adding a suffix. Remote collision policy supports rename, skip, or explicit guarded overwrite where the provider supports it; skip never copies an unrelated existing URL. Garage and Zenko CloudServer ignore the write condition that guards this, so a file another client creates under the same name at the same moment can be overwritten there; a template with a unique part, such as `{uuid}`, avoids that ([S3 adapter](s3.md#s3-compatible-services)).

**Settings → Hotkeys** records one combination per action, **Copy text from region** among them (it has no default key). Press Record, enter a chord, then release all keys. Escape cancels and Clear removes the assignment. Moving focus or closing also cancels recording. Hotkeys are paused during recording so an existing chord cannot trigger a capture. Accepted changes save automatically. A chord with Shift on a digit or punctuation key is saved by its unshifted key: pressing Ctrl+Shift+4 on a US keyboard saves `Ctrl+Shift+4`, not `Ctrl+Shift+$`. Each row reports conflicts, unsupported keys and any binding other than the one put.gd asked the desktop for; a row with no status line is bound as requested. Configured text alone does not prove a binding. CLI callers use Qt PortableText such as `Ctrl+1`. Capture hotkeys also work while a menu is open, put.gd's tray menu or another app's. **Capture desktop** includes the open menu. **Capture window** includes only what lies over the active window, and on GNOME and Plasma a window's own menus are not part of it. **Capture region** on X11 starts once the menu closes: press Esc or click outside the menu within 3 seconds. Otherwise put.gd says why it could not start, and you press the hotkey again. On GNOME the region selector opens over the open menu, so you can select it. Plasma closes the menu when the selector opens. Capture hotkeys are refused while the screen is locked, as are delays that end while locked. On GNOME/X11, an immediate agent desktop request has no lock-state check and can save the pixels visible on the locked display; this does not imply access behind the locker. On Budgie 10.10/labwc, a blanked or powered-off display also refuses screenshots with `session_locked` even when unlocked; keep the display awake for unattended captures. Budgie checks visibility before and after acquisition; a complete lock-and-unlock between those checks cannot be detected.

General contains notifications, launch at login, Appearance (Match the desktop, Dark or Light, applied as soon as you pick it; see [appearance](../design/appearance.md)) and Local copies. Advanced contains upload/image limits, queued-image memory, thumbnail cache and retention controls, **Diagnostics** (debug logging, logs, Create diagnostics package…; see [diagnostics](../design/diagnostics.md)) directly above Config file, plus desktop support details. **Reset all to defaults** resets only the limits. Blank history retention means forever. Each setting has keyboard-accessible help. Tray, notifications, launch at login, capture, clipboard and hotkeys depend on the actual desktop support reported there.

## History and recovery

History combines current operations and persistent file receipts in list or gallery view, grouped by day with work in progress first. The heading says what the page counts (`236 items`, or `12 of 168 items` while a search or filter narrows it) and the sidebar shows how many items each category holds. Search local stored names, choose **Filter** to add a filter for dates, destination, upload, local-copy or remote state, or file type (each active filter shows as a chip you can change or remove), and select a file to inspect its retained result. The **Failed uploads** category lists uploads that failed and uploads whose outcome is unknown (the request may have reached the destination, so check there before retrying); the tray's **Failed uploads** row opens it, and both count the same items. Close the details pane with its close button or Esc. In the list it then stays closed while you select other items, until you press Enter, double-click an item or use the details toggle in the header; the choice lasts while put.gd keeps running, including closing and reopening the window, and details show again after put.gd restarts. In the gallery, clicking an item opens its details as before. With details closed, selecting several items shows their actions in the header. **Previous** and **Next** (Alt+Left and Alt+Right) move between pages and show which page and items you are on. **Manage** holds Clear history records and Clear temporary files. Browsing does not list or download cloud files. Previews use bounded retained images; videos show a still with a play glyph and their length, made after the upload finishes (see [screen recording](../design/recording/history-posters.md#history-poster-frames)). Selecting a file whose preview is missing (an older video, a failed poster or one cleanup removed) makes it from the retained file or an unchanged local copy; when neither is left, the inspector says "Preview unavailable" and the list keeps a type fallback. The default 50-artifact page can split a large batch across pages.

S3 progress uses body bytes and a matching total. Proton displays encrypted transfer bytes separately from source size because overhead and retries make a percentage misleading. Counters can go backward during retries. A complete counter can still be finalizing; the confirmed receipt establishes completion.

Copy/open uses the retained result. Copying a result does not upload again. Retry local save preserves publication independently; upload retry uses retained failed/interrupted artifacts and original receipt identity. A failure that stored nothing, such as an image too large to edit, is labelled **Edit failed**, **Upload failed** (a clipboard upload) or **Capture failed** with its reason and offers no retry; start a new capture instead. Accepted work continues after the UI closes. Image copying and Save as use the retained original, never a thumbnail or an unannounced remote download. **Copy text** and **Show QR code** are described under [Copy text from the screen](#copy-text-from-the-screen).

S3 presigned and Backblaze B2 authorized links show their expiry in History. Choose **Refresh and copy link** after expiry to create a fresh link for the saved object, using its original destination and loaded credentials. If credentials are missing, History offers an explicit **Unlock saved credentials** action; it never opens the keyring on its own. Expiry leaves the uploaded object in place.

**Settings → General → After uploading** chooses individual links as they finish (default), or one ordered newline-separated list after overlapping uploads settle. Failed, cancelled, private, removed, and revoked results add no new links. A manual clipboard change or explicit History copy supersedes pending automatic writes; links remain available for deliberate copy. Partial copying remains a partial clipboard result.

For supported providers, remote deletion first prepares the exact selected history IDs, original destinations, and provider effects. Review that confirmation before accepting. Cancel sends no deletion. Retain the deletion operation ID for per-item removed, missing, changed, failed, unknown, unsupported, or retained-version outcomes. Cancellation cannot undo an already sent request. Local files and history survive remote deletion; caches may continue serving older bytes. Proton offers link creation/inspection/revocation, preserving the file and existing member access, but rejects unsafe remote deletion.

History maintenance clears local records or eligible retry artifacts/previews with confirmation. Neither deletes remote objects or user-owned local files. There is no single-record local-history removal endpoint.

Hovering the unchanged tray shows active work without opening the window: blue for capture/preparation/queue waits, green for storage activity, and a brief red/blue flash for genuine errors. Cancelling capture does not flash an error. The tooltip shows up to three active operations and an additional count. Closing all UI views releases the helper while the resident and tray remain.

Offscreen tests prove widget/core behavior only. Native desktop acceptance uses the installed-package lab, independent paste, actual capture bytes, and visible console witnesses described in [testing](../testing/testing.md).

### Arch Linux and derivatives

The install script recognizes Arch and Arch-based systems (including EndeavourOS,
CachyOS and Omarchy), installs the pacman package from pkg.put.gd and configures
signed pacman updates
([#1275](https://github.com/patrick-hudson/put.gd-desktop/issues/1275)). The
package is in nightly builds until the next stable release. Keep the system fully
updated with `sudo pacman -Syu`, or on Omarchy with `omarchy update`, because Omarchy
refuses a direct `pacman -Syu` unless told otherwise. Remove put.gd with `sudo pacman -R putgd`;
repository configuration remains administrator-owned. AppImage is also available;
package availability does not imply acceptance of every derivative's compositor.

The pacman package is built on current Arch against its OpenSSL (OpenSSL 4 today). A system that
follows Arch with a delay, Omarchy 4.5 with its OpenSSL 3.6 for one, cannot load it: there the native
package's `putgd-cli` and its S3 and R2 uploads fail with `libcrypto.so.4: cannot open shared object
file`, until Omarchy ships OpenSSL 4. The install script handles this for new installs:

- On Arch Linux itself (`ID=arch` in `/etc/os-release`, not Omarchy) it installs the pacman package, as
  always. Its `pacman -Syu putgd` brings OpenSSL to the version the package was built against.
- On every other Arch-family system (Omarchy, after its own system update; Manjaro, EndeavourOS and the
  like) it asks pacman whether the installed system has that OpenSSL (`pacman -T libcrypto.so=4-64`). If so
  it installs the pacman package. If not it installs the AppImage (`~/.local/bin/putgd.AppImage`)
  instead and says why. Any other pacman failure stops the install.
- On every Arch-family system, Arch Linux included, it then runs `/usr/bin/putgd-cli --version`, the binary the package installed. If
  that does not start (the same transaction can replace OpenSSL with a version the package does not
  match), it removes the package with `sudo pacman -R --noconfirm putgd` and installs the AppImage.

The AppImage path chosen by the `pacman -T` check adds no pacman repository or signing key and changes
nothing of put.gd's on the system. On Omarchy the install script first offers Omarchy's own system update,
which you agree to at the prompt. That update can sync a put.gd repository that was already configured, and
it can upgrade an already installed native put.gd before the script picks the AppImage; that is the update,
not the fallback.

When the `putgd-cli` check after a native install is what falls back, the package is removed but the put.gd
repository and its signing key stay configured. To remove them as well (use the key of the channel you
installed from):

```sh
sudo rm /etc/pacman.d/putgd.conf
sudo sed -i '\|^Include = /etc/pacman.d/putgd.conf$|d' /etc/pacman.conf
sudo pacman-key --delete 4032E28D893BAF66F4AC23D9A11D38D55536B371   # nightly
sudo pacman-key --delete C0962EFA929F5A5477A4509C0CC9E5A0F0B6FC1C   # stable
```

If the pacman package is already installed on such a system, switch to the AppImage with this command. The
package is removed only after the AppImage install succeeded, so a failed download leaves what you have:

```sh
curl -fsSL https://app.put.gd/install -o /tmp/putgd-install && sh /tmp/putgd-install --appimage && sudo pacman -R putgd
```
The AUR recipe is a draft and has not been published.
