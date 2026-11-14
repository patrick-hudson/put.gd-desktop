# put.gd user guide

Open put.gd to reach one window with **History**, **Destinations**, and **Settings**. The tray's **Settings and setup** and **Upload history** commands open that window, and clicking the tray icon opens History, which also shows current work. (The tray's former **Recent activity** command opened the same History page and has been removed.) Closing the window keeps the tray and accepted work running. The launcher reopens it. The tray pane and its capture commands are unchanged.

The left rail starts **Capture region**, **Capture window**, **Capture monitor**, **Capture desktop**, **Upload clipboard**, **Capture scrolling region**, **Copy text** or **Record screen** after finished settings edits are saved. The tray lists the same captures in this order: **Capture region**, **Repeat last region**, **Capture window**, **Capture monitor**, **Capture desktop** and the **Capture delay** choice, then **Capture region & edit**, **Edit image** (from the clipboard or a file), **Record screen** and **Upload clipboard**, with **Capture scrolling region** and then **Copy text from region** after them. Screen captures close this window to keep it out of the capture. Clipboard imports keep it open. A save failure keeps the window open; retry saving and start the action again. Store or clear unfinished credential entry before a screen capture. Region capture supports Escape to cancel. Window capture targets the focused external app where supported; monitor capture takes the one monitor the pointer is on, at its full size; desktop capture includes enabled monitors and panels. See [One monitor, a delay, or the same region again](#one-monitor-a-delay-or-the-same-region-again). Clipboard import prefers copied local files, then an image, then plain text. Text URLs are imported as text. **Edit & upload** selects a region and opens a small editor first; see [Edit before uploading](#edit-before-uploading). The information button and action tooltips explain the selected action and its saved output. Capture is also available through hotkeys and the CLI.

## GNOME 50: log out once

On GNOME 50, put.gd's capture and hotkeys need a Shell extension that put.gd sets
up for you. After installing, whether by the one-line installer, a DEB or RPM, or an
AppImage, start put.gd and log out and back in once when it asks, then start put.gd
again unless you turned on Launch at login. There is nothing to run or download. If
something else is in the way, put.gd says what in plain words.

Copy text needs the extension at version 23, which is the one these packages carry. After an update, log out and back in once. Until you do, Copy text from a region reports the same log-out step, and every other way of using it keeps working: History's **Copy text**, the editor's tools and the CLI's `text.recognize` from a window, a monitor, the desktop, the clipboard or a file.

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
capture, and before Repeat last region and Capture region & edit, so you can open a
menu, hover a button or hold a key first. Choose it in **Settings → Capture → Capture
delay**, in the tray's **Capture delay** submenu, or right-click one of those capture
buttons in the window and open **Capture delay** there; the choice stays until you set
it back to None. A capture you start with a hotkey is immediate unless you
turn on **Also delay captures started by a hotkey** in the same Settings section. A small pill at the top of the
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
no selection step: handy for a series of the same area. It's in the tray, in the
right-click menu of **Capture region**, and has its own hotkey (unassigned to begin
with). It uses Capture region's output. put.gd forgets the rectangle when it quits,
and says so instead of capturing when there is nothing to repeat yet or the monitors
have changed since.

## Upload from your file manager

Right-click files or folders in your file manager and choose **Upload with put.gd** (in Thunar it is under **Send To**; with the AppImage, Files and Caja list it under **Scripts**). They upload right away to your capture destination (or your only destination; with neither, Settings opens on Destinations), shared and copied as a capture is. Folders upload as one `.zip` by default; **Settings → General → File manager → Folders** can upload each file instead. The AppImage's entries switch off under **File manager menu**. After installing the DEB or RPM, Files, Caja and Peony show the entry after a restart; put.gd's first notice offers one. See [file-manager-upload.md](../design/file-manager-upload.md).

## Edit before uploading

Choose **Capture region & edit** from the rail or tray, or assign it a key in **Settings → Hotkeys** (it has no default key). Drag the region to edit; Escape cancels. To edit something you already have, use **Edit image** at the top of History (the clipboard image or a PNG/JPEG file), right-click the rail button, or drop a PNG or JPEG on it. **Edit again** on a History item reopens its local copy when **Keep a local copy** was on; the edited result is uploaded as a new item. In the editor:

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

Use **Capture scrolling region** to get one tall image of something longer than the screen: a web page, a document, a chat, a settings list. Choose it from the tray or the main window (its button reads **Scroll** when the window is narrow), or give **Capture scrolling region** a hotkey in **Settings → Hotkeys** (it has none by default). Drag a rectangle over the scrollable content, and include its scrollbar. There is no Start button: put.gd begins as soon as you let go.

1. put.gd scrolls the content to the top first, so the image starts at the top of the page. A long page takes a while; the bar says so. Turn this off in **Settings → Capture → Scrolling capture → Scroll to the top first** to start from wherever the content is now.
2. It turns the mouse wheel for you. The pointer waits outside your rectangle while put.gd reads the screen and comes in just inside its edge only to turn the wheel, then leaves, so a link under it does not show its address in the image. A small bar outside the rectangle shows the height so far, with **Stop** and **Cancel**. Keep your own mouse and keyboard still: the wheel goes to whatever is under the pointer, and it waits until Ctrl, Shift, Alt and Super are up on X11.
3. It stops when the content stops moving (the whole scroll area is then in the image, from where it started: the top unless you turned **Scroll to the top first** off; a page that answers very slowly can still be taken for ended, see the limits below), when the image reaches the height cap, or when you stop it.

**Stop** keeps the image so far; the bar's button, the hotkey again, **Esc** on X11 and `scroll.stop` all do it. If you stop while it is still scrolling to the top, you get the viewport as it is then. **Cancel** throws everything away: nothing is saved, uploaded or added to History. The result is one PNG that goes to the output chosen for this action (it starts out as **Capture region**'s; change it under **Per-action exceptions**): local copy, upload, History and link copy. To edit it first, an agent or script can run `editor.open {"source":"scroll"}`, which opens the image in the editor and uses **Capture region & edit**'s output.

**Top not reached.** If put.gd cannot get to the top (the page keeps loading more above, never holds still, or is longer than a minute and a half of scrolling), it captures from where the page is. The image is kept and shared as usual, and a warning says "Could not reach the top of the page. The image starts partway down." History carries the same note. Scroll the page to its top yourself and capture again, with **Scroll to the top first** off if it keeps happening.

**Height cap.** An image stops at the **Maximum height** in **Settings → Capture → Scrolling capture** (2,000 to 60,000 px, 30,000 by default), or earlier when the image would pass the size limits. The image so far is kept and shared as usual, and a warning says "Stopped at 30,000 px. The page continues below." History carries the same note. If the upload or the local save failed, History shows that failure first and keeps the note after it. Raise the cap, or capture the rest separately, if you need more. The image never passes the cap; a sticky footer counts in it, and if the cap is too small to hold the footer you get the first screenful.

Tips:

- Include the scrollbar in the rectangle. Without it a blank stretch the height of the window looks like the end.
- A sticky header or footer is kept once, at the top and bottom of the image, not repeated. A header that only shows after you have scrolled a little (a bar that appears once the title has gone) is left out of the image, and a banner that slides in over the bottom later is kept once as the footer. A bar the page pins beside every row, such as a side bar, is in the image beside every row, as it is on the screen.
- The last rows of each frame wait for the next frame, so a link panel or a window border at the bottom edge is in the image only at its end, where it is on the screen.
- A page that is slow to answer the wheel takes longer, and at its end the page may move up and back down once: put.gd sends a step up and one down again to prove that it is at the end, and a page that does not answer them ends the capture as `lost_track` with the rows so far.
- Content that changes while it scrolls (images that load late, video, animations) can stop the capture early. The image so far is exact. Wait for the page to finish loading first.
- put.gd closes its own notifications (including the file-manager notice and the fallback balloon) when a capture starts and holds new ones until it ends. If one cannot be closed the capture does not start (`notification_close_failed`): dismiss it and try again. A notification from another application over the rectangle ends up in the image, so close it first.
- The region has to lie on one monitor and be at least 64 px each way.
- GNOME shows its screen-sharing indicator while a capture runs. Clicking it ends the capture like Stop. Scrolling capture needs the put.gd Shell extension at version 22 or later: after an update, log out and back in once, or it reports an error until then.
- On the Ubuntu 24.04 package for KDE Wayland there is no region selector, so the window's button says scrolling capture is unavailable (`region_unavailable`); use the AppImage. A script cannot capture there either: with no selector, scrolling capture is unavailable for every caller, including a rectangle named through `putgd-cli` or MCP.
- A new capture can start as soon as the image is finished, while it is still waiting to upload.
- If the size of the captured area changes while it scrolls, the capture ends with `display_changed` and keeps nothing. If the page's layout inside the rectangle changes (the window is resized, a bar appears at the bottom), the capture ends as `lost_track` and keeps the rows from before the change; a change before the first row is stitched, while the page is still going up, is not a change, and the image shows the layout on screen from then on. A bar that is there from the first scroll on is kept once as the footer and the capture completes ([the layout rule](../design/scroll-capture.md#the-layout-rule)).

**Where a live page ends early.** On real web pages a capture can end as `lost_track` (or, for the first, `top_not_reached`) with an honest note and an image that is exact up to there; none of these puts a wrong row in the image.

- A page that lays itself out again under the viewport while it is scrolled (Chromium on a long pull request page does) can leave the picture where it was after a wheel step, and the capture ends there.
- A banner that appears inside a step's own frame and covers much of the overlap (more than a tenth of it) ends the capture. A banner that stays in two frames running becomes the footer and the capture completes.
- A step where the page moves a different distance than its text is redrawn at (a few pixels' difference, with the text drawn anew) cannot be proved by equal rows, and the capture ends.
- The way up can be slow when the point where the pointer comes in sits on a link: the browser shows its link panel after each visit and the picture is not at rest until it has gone. It can take a minute or more on Plasma, and can end `top_not_reached` for an image that is whole.
- Side bars beside every row are in the image beside every row, as on the screen, not once as in a browser's own full-page picture.
- A bar the page pins to the bottom that appears only once the capture is at its end covers the page's last rows in the image, as on the screen.
- A page that paints the first notch of a wheel step at once and then works through a long backlog before it paints the rest could still be taken for the end of the content.
- A column that scrolls at its own pace (a minimap, a parallax strip) ends the capture, `lost_track`, with the rows from before it moved: no single tall image can show it beside the page.

## Copy text from the screen

Use **Copy text** to copy the text out of something you can see but cannot select: a screenshot of a terminal, an error in a dialog, a slide, a video frame, a PDF that is only an image. Choose **Copy text from region** in the tray, **Copy text** in the main window (its button reads **Text** when the window is narrow), or give **Copy text from region** a hotkey in **Settings → Hotkeys** (it has none by default). Drag a rectangle over the text. Escape cancels. A capture delay works as it does for a region. The selector looks the same as for any other capture. It works on Cinnamon, GNOME, Plasma, Budgie and Hyprland, and on GNOME and Plasma sessions that run on X11.

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

**Quick redact** is a menu in the Redact tool's panel: **Email addresses**, **Phone numbers**, **IP addresses**, **Card numbers**, **Keys and tokens** and **All of these**. It adds ordinary Redact marks over matching text, as one step you can undo, and says "Added 3 suggested redactions" or "No matches found. Text can still be there." Hover **Keys and tokens** to see the shapes it knows: AWS access key IDs, GitHub, GitLab, Slack and Stripe tokens, JWTs, private-key blocks, and long unbroken hex or base64 runs. Recognition often splits or misreads a long token, and Quick redact copes with that. A mark reaches about a character and a half beyond the text it covers on each side, and it can take in an ordinary word that is joined to a token, such as a label like "sha256" or the words after a token. A version number such as 26.1112.4660 is not suggested as a phone number, and neither is a long run of digits unless it starts with "+". It also marks a long run of one or two repeated letters, such as a masked secret shown as a row of x's.

**Quick redact only suggests. Recognition can miss text, and a page with no matches can still hold some.** Look at the result yourself before you upload. Redact is what removes the pixels, and Quick redact only places Redacts for you. A number made of repeated digits can be counted wrong, so a card number like that can be missed. If you submit or close the edit while Quick redact is still working, it adds nothing and says "Quick redact added nothing: the edit was already submitted."

### Copy text and show a QR code from History

- **Copy text** in an image item's inspector and ⋯ menu does the same as the action, reading the item's local copy. It is off, and says why, when the item has no local copy (turn on **Keep a local copy** before capturing) or is not a PNG or JPEG image. put.gd never downloads an upload to read it.
- **QR code** in an uploaded item's inspector and **Show QR code** in its ⋯ menu open a window with a QR code of the link **Copy link** would copy, so a phone can scan it. An expired link that can be refreshed is refreshed first. **Copy image** puts the code on the clipboard and **Save…** writes it as a 1,024 pixel PNG. It is off, and says why, for a private, removed or revoked link and for an expired link that cannot be refreshed. A link over 2,331 bytes is too long for a QR code, and the window says so. **Save…** never replaces a file without asking.
- History search is still by name only. put.gd does not read the text in your captures unless you ask.

## Record the screen

Choose **Record screen** on the rail or in the tray, or assign the record hotkey in **Settings → Hotkeys**. Drag the area to record on X11, Plasma, GNOME (with the put.gd Shell extension), Budgie and Hyprland; other Wayland desktops ask once which screen or window to share. The floating bar chooses MP4, WebM, GIF or WebP, system audio and the microphone, then records, pauses and stops. Stop always opens a private review: trim, add blur or solid covers for part of the clip, play it back, **Save copy…**, **Discard** or **Upload**. Nothing is saved or uploaded until you choose one. See [screen recording](../design/recording.md) for the details.

To narrate a recording, tick **Microphone** on the bar. The bar beside it moves with your voice, so you can see the microphone is live before you press Record; put.gd opens the microphone only while that box is ticked and lets go of it when the recording stops. The choice is remembered for next time. put.gd uses the system's default microphone unless you pick another in **Settings → Capture → Screen recording → Microphone**. If no microphone can be opened, the box reads **No microphone** and is greyed out (hover it for the reason). A microphone plugged in while the bar is showing is offered within a second or two, with no need to start over; if **Microphone** was ticked last time, it is ticked again. A microphone unplugged during a recording leaves the recording running, silent from there. With **System audio** ticked as well, both are in the clip. GIF and WebP have no sound.

**Settings → Capture → Screen recording → Click highlights** draws a ring at the pointer each time you click, so a viewer can see what you pressed. On X11, GNOME and Hyprland the ring is drawn into the clip only. On KDE Plasma put.gd switches on KWin's own Mouse Click Animation effect while it records, so you also see the rings on your screen, and they appear in screen and region recordings but not in window recordings. With Budgie the rings are drawn into the clip for screen and region recordings, for clicks made on application windows; clicks on the panel and clicks made with unusual modifier combinations are not highlighted, and window recordings have none (the full list is in [Budgie](../desktops/budgie.md#click-highlights)). Where the desktop cannot report clicks at all, for example Hyprland with a configuration put.gd cannot add binds to, or GNOME while an older put.gd extension is still loaded, the switch says why and recordings start without rings. **Settings → Advanced → Desktop support details** says what your desktop offers.

Recording needs an H.264 encoder. When none works, put.gd says so and what to install instead of starting:

- **Ubuntu, Kubuntu, Mint and Debian packages** bring one with their GStreamer dependencies.
- **Fedora**: Fedora installs a placeholder OpenH264 library that the RPM's weak dependencies do not replace. Replace it explicitly with Cisco's: `sudo dnf install openh264 gstreamer1-plugin-openh264`. The one-line installer runs this codec step for you when the repository is enabled and the placeholder remains or either codec package is missing. If the `fedora-cisco-openh264` repository was turned off, it prints the commands instead: first run `sudo dnf config-manager setopt fedora-cisco-openh264.enabled=1`, then the codec install.
- **AppImage**: it carries its own recorder. It encodes with your graphics driver where it can (VA-API or NVIDIA) and otherwise with its bundled x264 and x265, so it needs no setup.

In testing on an AMD Radeon 740M with KDE Plasma, hardware and software encoding were equally smooth at 60 fps. On that setup, a notification with a countdown bar made a 60 fps recording less smooth while it was on screen, about 50 frames per second, with either encoder. Other GPUs may differ.

## Destinations and credentials

Use **Add destination** to configure S3, Backblaze B2, Proton Drive or a [custom uploader (HTTP)](custom-uploader.md), which takes a ShareX `.sxcu` file. Valid non-secret fields save automatically. Removing a destination removes it from settings and clears affected output references; original receipts, local files, and remote objects remain. Review the default and exceptions after removal.

To change the order of destinations, drag one up or down the list; a line shows where it will land. From the keyboard, select a destination and press Alt+Up or Alt+Down, or use **Move up** / **Move down** in its ⋯ menu. The new order saves right away and is also the order of the Destination dropdowns. It changes nothing else: the default, per-action exceptions and history stay with the same destinations, and unsaved edits stay with the destination you were editing. If the settings file changed elsewhere, the previous order comes back and Settings asks you to reload.

For S3, enter the bucket and region, then choose public links or **Private bucket: links that expire**. Expiring links last 1–7 days, use the endpoint host rather than Link domain, and never request a public ACL. Temporary credentials may expire sooner. Public links retain the link domain and public-read/omit-ACL choices. Searchable regions accept custom values. **Advanced connection settings** contains endpoints, addressing, storage class, and transport limits. The development template is an explicit import, supplies no credentials, and changes no bucket policy. **Test upload and link** runs only when pressed, uses synthetic content, and shows progress and its result below the destination title. Success means the test upload, link verification and cleanup all passed. In S3 presigned mode the signed link must work and unsigned access must be denied. Errors give the next checks for credentials, permissions, destination settings or connectivity; a denied object check can be ambiguous. Resolve uncertain cleanup through its retained test identity.

S3 credentials are saved only when you choose **Save to keychain** or **Use for this session**; session credentials expire when the resident exits. Configuration contains references rather than secret values. By default put.gd reads saved keychain credentials once when it starts (**Settings → General → Credentials**); if your desktop keyring is locked, it shows its own unlock prompt then. Turn that off to unlock only with **Unlock saved credentials**. Uploads and hotkeys never unlock the keyring. After loading, the form shows only the first four access-key ID characters and whether the secret/token exists; short IDs are fully masked. **Show** beside the secret access key reveals either your typed value or the unlocked saved secret. If saved credentials are not loaded, choose **Unlock saved credentials** first. A revealed saved secret is read-only; **Hide** clears it so you can enter a replacement. Changing destination or provider, leaving Destinations, closing, and reloading hide saved secrets. Store, ordinary close, and reload clear credential inputs.

For Proton, choose **Connect account…** and finish sign-in in the browser. The connected email stays visible when switching destinations. Choose a top-level My files folder, create a new folder, or press **Refresh folders** beside the selector. Folder queries show **Loading folders…** and preserve the known email and selection. Account setup never uploads a file. Configure private/shared output and advanced new-link expiration/password settings as described in [Proton Drive](proton-drive.md). Proton links open a share page; they are not direct image-embedding URLs. Safe remote deletion and account sign-out are not available in this integration.

## Settings and autosave

Settings save automatically after a finished field edit, toggle, or selection. Focusing and leaving an unchanged value does not save. The footer explains autosave without routine success banners. Invalid fields show specific feedback while accepted settings stay active; independently valid changes may still save. A write failure keeps the draft and offers **Retry saving**, rather than replaying the request automatically.

Accepted external changes refresh a clean form. A dirty draft instead reports that settings changed elsewhere and offers reload. **Settings → Advanced → Config file → Reload from file** discards the draft and rereads configuration; an invalid external file leaves the last accepted settings active. The config section shows the canonical path and accepted revision. See [configuration.md](../design/configuration.md) for fields, units, and cooperative locking.

**Settings → Capture → File names** keeps the original names of copied files by default, or uses random names/custom templates. Capture, clipboard-image, clipboard-text, and copied-and-uploaded-file (**Copied & uploaded files**) patterns have separate help. Local collisions preserve files by adding a suffix. Remote collision policy supports rename, skip, or explicit guarded overwrite where the provider supports it; skip never copies an unrelated existing URL.

**Settings → Hotkeys** records one combination per action, **Copy text from region** among them (it has no default key). Press Record, enter a chord, then release all keys. Escape cancels and Clear removes the assignment. Moving focus or closing also cancels recording. Hotkeys are paused during recording so an existing chord cannot trigger a capture. Accepted changes save automatically. A chord with Shift on a digit or punctuation key is saved by its unshifted key: pressing Ctrl+Shift+4 on a US keyboard saves `Ctrl+Shift+4`, not `Ctrl+Shift+$`. Each row reports conflicts, unsupported keys and any binding other than the one put.gd asked the desktop for; a row with no status line is bound as requested. Configured text alone does not prove a binding. CLI callers use Qt PortableText such as `Ctrl+1`. Capture hotkeys also work while a menu is open, put.gd's tray menu or another app's. **Capture desktop** includes the open menu. **Capture window** includes only what lies over the active window, and on GNOME and Plasma a window's own menus are not part of it. **Capture region** on X11 starts once the menu closes: press Esc or click outside the menu within 3 seconds. Otherwise put.gd says why it could not start, and you press the hotkey again. On GNOME the region selector opens over the open menu, so you can select it. Plasma closes the menu when the selector opens. Capture hotkeys are refused while the screen is locked, as are delays that end while locked. On GNOME/X11, an immediate agent desktop request has no lock-state check and can save the pixels visible on the locked display; this does not imply access behind the locker. On Budgie 10.10/labwc, a blanked or powered-off display also refuses screenshots with `session_locked` even when unlocked; keep the display awake for unattended captures. Budgie checks visibility before and after acquisition; a complete lock-and-unlock between those checks cannot be detected.

General contains notifications, launch at login, Appearance (Match the desktop, Dark or Light, applied as soon as you pick it; see [appearance](../design/appearance.md)) and Local copies. Advanced contains upload/image limits, queued-image memory, thumbnail cache and retention controls, **Diagnostics** (debug logging, logs, Create diagnostics package…; see [diagnostics](../design/diagnostics.md)) directly above Config file, plus desktop support details. **Reset all to defaults** resets only the limits. Blank history retention means forever. Each setting has keyboard-accessible help. Tray, notifications, launch at login, capture, clipboard and hotkeys depend on the actual desktop support reported there.

## History and recovery

History combines current operations and persistent file receipts in list or gallery view, grouped by day with work in progress first. The heading says what the page counts (`236 items`, or `12 of 168 items` while a search or filter narrows it) and the sidebar shows how many items each category holds. Search local stored names, choose **Filter** to add a filter for dates, destination, upload, local-copy or remote state, or file type (each active filter shows as a chip you can change or remove), and select a file to inspect its retained result. Close the details pane with its close button or Esc. In the list it then stays closed while you select other items, until you press Enter, double-click an item or use the details toggle in the header; the choice lasts while put.gd keeps running, including closing and reopening the window, and details show again after put.gd restarts. In the gallery, clicking an item opens its details as before. With details closed, selecting several items shows their actions in the header. **Previous** and **Next** (Alt+Left and Alt+Right) move between pages and show which page and items you are on. **Manage** holds Clear history records and Clear temporary files. Browsing does not list or download cloud files. Previews use bounded retained images; videos show a still with a play glyph and their length, made after the upload finishes (see [screen recording](../design/recording/history-posters.md#history-poster-frames)). Selecting a file whose preview is missing (an older video, a failed poster or one cleanup removed) makes it from the retained file or an unchanged local copy; when neither is left, the inspector says "Preview unavailable" and the list keeps a type fallback. The default 50-artifact page can split a large batch across pages.

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
updated with `sudo pacman -Syu`. Remove put.gd with `sudo pacman -R putgd`;
repository configuration remains administrator-owned. AppImage is also available;
package availability does not imply acceptance of every derivative's compositor.
The AUR recipe is a draft and has not been published.
