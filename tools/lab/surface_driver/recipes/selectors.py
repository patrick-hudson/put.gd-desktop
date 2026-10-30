"""Real-input screenshot/recording selectors and recording controls.

The core ``selector-desktop`` fixture owns a moving synthetic
pattern and its cleanup, and supplies ordered logical ``screens`` rectangles,
an absolute private ``output_dir`` and the exact-build ``record_helper`` path.
No fixture supplies the result of an interaction. AT-SPI descriptions are the
current screenshot selector's only live geometry readback; completed captures
and every recording transition are checked through resident inspection.
"""
from __future__ import annotations

import re
import shlex
import base64
import hashlib
import json
from contextlib import contextmanager
from pathlib import PurePosixPath

from tools.lab.surface_driver.api import RecipeBlocked, RecipeFailed, recipe
from tools.lab.surface_driver.fixtures import consent
from tools.lab.surface_driver.fixtures.base import Scope

# Fixture setup/cleanup belongs to the core; profile fragments hold only desktop differences.
NEEDS = ("selector-desktop",)
REGION = "44-region-keyboard"
RECSEL = "45-record-region-keyboard"
BAR = "33-record-ready-bar"



def _frame(ctx, label, **options):
    if ctx.destination == "local" and ctx.placement in ("boat", "guest") and ctx.profile.get("guest"):
        options["desktop_safe"] = "Disposable guest: synthetic moving pattern, selectors and recording controls; isolated loopback publications only; no live account or credentials"
    return ctx.frame(label, **options)


def _result(value):
    # Context implementations may retain the wire envelope or return its result.
    return value.get("result", value)


def _call(ctx, method, params=None):
    value = ctx.call(method, params or {})
    # Inspect results themselves carry the operation's historical error. Only
    # a wire-envelope error (or a bare command error) rejects the read.
    if "result" in value or "state" not in value:
        ctx.expect(not (value.get("error") or {}).get("code"), method + " succeeded")
    return _result(value)


def _env(ctx):
    value = ctx.fixture("selector-desktop")
    if not isinstance(value, dict) or not value.get("screens"):
        ctx.blocked("selector-desktop must supply live logical screen rectangles (#664)", issue=664)
    return value


def _screen(ctx):
    return _env(ctx)["screens"][0]


def _half(screen):
    x, y, w, h = screen
    return [x + w / 4, y + h / 4, w / 2, h / 2]


@contextmanager
def _portal_scope(ctx):
    """Reuse core's request ownership and one-click consent, never guess a point."""
    if ctx.profile.get("session") != "wayland":
        yield
        return
    scope = Scope(ctx)
    previous = getattr(ctx, "_selectors_consent", None)
    ctx._selectors_consent = (scope, {})
    try:
        yield
    finally:
        ctx._selectors_consent = previous
        errors = scope.close()
        ctx.expect(not errors, "selector consent monitor cleanup: " + str(errors))


@contextmanager
def _local(ctx, mode="live"):
    """Setup only: retain/rebase configuration; never publish from this lane."""
    env = _env(ctx)
    root = env.get("output_dir", "")
    if not PurePosixPath(root).is_absolute():
        ctx.blocked("selector-desktop requires a private absolute output_dir (#664)", issue=664)
    old = _call(ctx, "settings.get")
    document = old["document"]
    if "regionSelectionMode" not in document["preferences"]:
        # Defaults omit this optional field. Seed the equivalent default once in
        # the runner's private session, then take a fully restorable baseline.
        _call(ctx, "settings.patch", {"patch": {"preferences": {"regionSelectionMode": "live"}}, "expectedRevision": old["metadata"]["acceptedRevision"]})
        old = _call(ctx, "settings.get")
        document = old["document"]
        ctx.note("Seeded explicit live selection mode in the private run configuration")
    # A shortcut-only record entry inherits region. Adding route fields to it
    # cannot be undone by recursive settings.patch (null is a value).
    routed = [a for a in ("region", "edit", "record") if "destination" in document["actions"][a]]
    patch = {"actions": {a: {"destination": "local", "profileId": None} for a in routed},
             "archive": {"keepUploads": True, "root": root, "folders": {k: None for k in document["archive"]["folders"]}},
             "preferences": {"regionSelectionMode": mode}}
    _call(ctx, "settings.patch", {"patch": patch, "expectedRevision": old["metadata"]["acceptedRevision"]})
    try:
        accepted = _call(ctx, "settings.get")["document"]
        ctx.expect(accepted["preferences"]["regionSelectionMode"] == mode, "selection mode accepted")
        # The resident can open Settings on its first start. Bring the synthetic
        # fixture back before opening a selector, never while a selector is live.
        command = env.get("command")
        if not callable(command):
            ctx.blocked("selector-desktop requires its core-owned pattern command callback (#664)", issue=664)
        command("raise")
        ctx.note("Core-owned moving pattern raised before selection")
        with _portal_scope(ctx):
            yield
    finally:
        # Restore only owned fields against a freshly read revision.
        now = _call(ctx, "settings.get")
        restore = {"actions": {a: document["actions"][a] for a in routed},
                   "archive": {k: document["archive"][k] for k in ("keepUploads", "root", "folders")},
                   "preferences": {"regionSelectionMode": document["preferences"]["regionSelectionMode"]}}
        _call(ctx, "settings.patch", {"patch": restore, "expectedRevision": now["metadata"]["acceptedRevision"]})


def _description(ctx, recording=False):
    name = "Recording region" if recording else "Capture region"
    nodes = ctx.accessible({"name": name})
    descriptions = [n.get("description", "") for n in nodes if n.get("description")]
    if not descriptions:
        ctx.blocked("Live selector accessibility description unavailable; no resident selection inspect (#942)", issue=942)
    return descriptions[0]


def _rect(ctx, recording=False):
    text = _description(ctx, recording)
    match = re.search(r"Region ([\d.]+) × ([\d.]+) at (-?[\d.]+), (-?[\d.]+)\.", text)
    if match is None:
        return None
    w, h, x, y = map(float, match.groups())
    return [x, y, w, h]


def _visible(ctx, name):
    # AT-SPI can retain hidden child nodes. A name alone is not visibility.
    return any((r := node.get("rect")) and len(r) == 4 and r[2] > 0 and r[3] > 0
               and r[0] > -1_000_000 and r[1] > -1_000_000 for node in ctx.accessible({"name": name}))


def _expect_rect(ctx, wanted, recording=False):
    ctx.wait(lambda: _rect(ctx, recording) == wanted, "selector rectangle " + str(wanted))
    ctx.note("Inspected selector rectangle " + str(wanted))


def _focus_selector(ctx, role):
    # The helper advertises its accessible before the native window is mapped.
    # Retry only an absent window; ambiguous geometry and input refusals remain
    # immediate blockers. focus(..., False) observes and never raises a cover.
    missing = None
    def mapped():
        nonlocal missing
        try:
            ctx.focus(role, raise_window=False)
            return True
        except RecipeBlocked as error:
            if "live window match is missing or ambiguous (0 matches)" not in str(error):
                raise
            missing = error
            return False
    try:
        ctx.wait(mapped, role + " selector mapped", timeout=10)
    except RecipeFailed:
        if missing is not None:
            raise missing
        raise


def _open_region(ctx, edit=False, accessible=True):
    # action.invoke durably acknowledges before interactive acquisition finishes.
    # Surface setup therefore does not depend on the shell window's layout.
    opened = _result(ctx.view("action.invoke", {"action": "edit" if edit else "region"}))
    ctx.expect(bool(opened.get("operationId")), "capture action returns a durable operation identity")
    oid = opened["operationId"]
    ctx.note("Capture operation " + oid)
    try:
        if accessible:
            ctx.wait(lambda: ctx.accessible({"name": "Capture region"}), "screenshot selector opened")
        else:
            ctx.wait(lambda: _op(ctx, oid)["state"] == "acquiring", "capture selector acquiring")
        # view(action.invoke) has no shell-window role. Select the actual overlay
        # through the public focus contract before guarded keyboard/pointer input.
        _focus_selector(ctx, "region")
    except (RecipeBlocked, RecipeFailed) as error:
        _frame(ctx, "selector-readback-unavailable")
        observed = _op(ctx, oid)
        if observed["state"] == "failed":
            ctx.expect(False, "capture failed before selector input: " + observed.get("error", {}).get("code", "unknown"))
        if observed["state"] in ("accepted", "acquiring"):
            # Cleanup only; a blocked input is never represented as a cancel pass.
            _call(ctx, "operation.cancel", {"id": oid})
            ctx.blocked("Capture selector has no live AT-SPI node or authorized geometry in this session (#942): " + str(error), issue=942)
        raise
    return oid


def _op(ctx, oid):
    return _call(ctx, "operation.inspect", {"id": oid})


def _png_dimensions(ctx, path, width, height):
    probe = ctx.shell("python3 -c 'import struct,sys; print(*struct.unpack(\"!II\",open(sys.argv[1],\"rb\").read(24)[16:24]))' " + shlex.quote(path))
    ctx.expect(probe.get("returncode") == 0 and probe.get("stdout", "").strip() == f"{int(width)} {int(height)}", "capture dimensions match chosen output")
    _retain(ctx, path, "png")


def _retain(ctx, path, kind):
    """Export only signature-checked synthetic outputs from this private run."""
    program = '''import base64,hashlib,json,os,stat
from pathlib import Path
p=Path(PATH);root=Path(ROOT).resolve(strict=True)
assert p.resolve(strict=True).is_relative_to(root)
fd=os.open(p,os.O_RDONLY|os.O_NOFOLLOW)
with os.fdopen(fd,'rb') as f:
 s=os.fstat(f.fileno());assert stat.S_ISREG(s.st_mode) and s.st_uid==os.getuid() and 0<s.st_size<=32*1024*1024
 data=f.read(32*1024*1024+1);assert len(data)==s.st_size
assert data.startswith(bytes([137,80,78,71,13,10,26,10])) if KIND=='png' else data[4:8]==b'ftyp'
print(json.dumps({'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest(),'data':base64.b64encode(data).decode()}))
'''.replace('PATH', repr(path)).replace('ROOT', repr(ctx.target.root)).replace('KIND', repr(kind))
    reply = ctx.shell("python3 - <<'SD_RETAIN'\n" + program + 'SD_RETAIN\n')
    ctx.expect(reply.get("returncode") == 0, "synthetic functional artifact readback succeeded")
    value = json.loads(reply["stdout"])
    data = base64.b64decode(value["data"], validate=True)
    digest = hashlib.sha256(data).hexdigest()
    ctx.expect(len(data) == value["bytes"] and digest == value["sha256"], "functional artifact transport hash matches")
    directory = ctx.out / "functional"
    directory.mkdir(mode=0o700, exist_ok=True)
    output = directory / (digest + "." + kind)
    if not output.exists():
        with output.open("xb") as stream:
            stream.write(data)
    ctx.expect(hashlib.sha256(output.read_bytes()).hexdigest() == digest, "retained functional artifact hash matches")
    output.with_suffix(output.suffix + ".json").write_text(json.dumps({"source": path, "bytes": len(data), "sha256": digest, "kind": kind}) + "\n")
    ctx.note("Retained synthetic " + kind + " sha256=" + digest)


def _cancel_region(ctx, oid, key="Esc"):
    if key == "right":
        x, y, w, h = _screen(ctx)
        ctx.click_at(int(x + w / 2), int(y + h / 2), button="right")
    else:
        ctx.keys(key)
    op = ctx.wait(lambda: (o if (o := _op(ctx, oid))["state"] in ("cancelled", "failed") else None), "capture cancelled")
    ctx.expect(op.get("cancelled") or op.get("error", {}).get("code") == "cancelled", "cancel has no capture result")
    ctx.expect(not op.get("items"), "cancel creates no artifact")


def _open_record(ctx, entire=False, rect=None):
    caps = _call(ctx, "recording.capabilities")
    if not caps.get("formats"):
        ctx.blocked("This desktop has no recording formats")
    target = {"kind": "region"}
    if entire or rect is not None:
        target["rect"] = _screen(ctx) if entire else rect
    portal = getattr(ctx, "_selectors_consent", None)
    watch = consent.start(portal[0]) if portal else None
    opened = _result(ctx.view("recording.prepare", {"target": target, "format": "mp4", "codec": "h264", "fps": 15, "audio": {"system": False}}))
    ctx.expect(bool(opened.get("recordingId")), "recording prepare returns an operation identity")
    rid = opened["recordingId"]
    if portal:
        portal[1][rid] = watch
    ctx.note("Recording operation " + rid)
    if "rect" in target:
        _state(ctx, rid, "ready")
    else:
        ctx.wait(lambda: _inspect(ctx, rid).get("selecting"), "recording selector opened")
        try:
            _focus_selector(ctx, "recsel")
        except (RecipeBlocked, RecipeFailed) as error:
            _frame(ctx, "recording-selector-readback-unavailable")
            observed = _inspect(ctx, rid)
            _cancel_record(ctx, rid)
            ctx.expect(observed["state"] != "failed", "recording failed before selector input")
            ctx.blocked("Recording selector input/readback unavailable (#942): " + str(error), issue=942)
    return rid


def _inspect(ctx, rid):
    return _call(ctx, "recording.inspect", {"id": rid})


def _state(ctx, rid, wanted):
    portal = getattr(ctx, "_selectors_consent", None)
    if wanted == "ready" and portal and rid in portal[1]:
        return consent.await_ready(portal[0], rid, portal[1][rid], timeout=45)
    if wanted == "cancelled":
        # Cancel retires the active recording session. Its durable operation is
        # the inspectable result, not recording.inspect (which is now unavailable).
        return ctx.wait(lambda: (s if (s := _op(ctx, rid))["state"] == wanted else None), "recording cancelled")
    return ctx.wait(lambda: (s if (s := _inspect(ctx, rid))["state"] == wanted else None), "recording " + wanted, timeout=45)


def _choose(ctx, rid, rect=None):
    if rect is None:
        ctx.keys("Right")
        rect = _half(_screen(ctx))
        # The ready target independently verifies the keyboard-selected bounds.
        ctx.keys("Return")
    else:
        x, y, w, h = rect
        ctx.drag((int(x), int(y)), (int(x + w), int(y + h)))
    ready = _state(ctx, rid, "ready")
    ctx.note("Observed recording target " + str(ready["target"]))
    _frame(ctx, "chosen-recording-area")
    ctx.expect(ready["target"]["rect"] == rect, "chosen recording bounds match real input")
    ctx.expect(not ready["selecting"], "selector closes before ready bar")
    ctx.note("Inspected recording target " + str(rect))
    return ready


def _cancel_record(ctx, rid):
    # Cleanup is not claimed as an input interaction.
    if _call(ctx, "recording.capabilities").get("active") == rid:
        _call(ctx, "recording.cancel", {"id": rid})


def _ready(ctx, entire=False):
    # Prepare the bar's surface, independently of the selector input/readback
    # gate. Selection gestures have their own recipes; every bar transition
    # below still comes from real input, never recording.start/pause/stop.
    rect = _screen(ctx) if entire else _half(_screen(ctx))
    rid = _open_record(ctx, rect=rect)
    try:
        ctx.expect(_inspect(ctx, rid)["target"]["rect"] == rect, "bar setup uses the requested synthetic region")
    except Exception:
        _cancel_record(ctx, rid)
        raise
    return rid


def _motion(ctx, rid):
    """Probe a real clip, using the same --motion analyzer as record_run.py."""
    env = _env(ctx)
    helper = env.get("record_helper")
    if not helper or not PurePosixPath(helper).is_absolute():
        ctx.blocked("Exact-build record_helper is required for motion scoring (#664)", issue=664)
    path = str(PurePosixPath(env["output_dir"]) / (rid + ".mp4"))
    _call(ctx, "recording.saveCopy", {"id": rid, "path": path, "overwrite": False})
    done = ctx.wait(lambda: (s if (s := _inspect(ctx, rid))["state"] == "reviewing" and s.get("lastExport") else None), "motion probe export", timeout=60)
    ctx.expect(done["lastExport"].get("status") == "saved" and done["lastExport"].get("path") == path
               and not (done["lastExport"].get("error") or {}).get("code"), "motion probe export succeeded")
    result = ctx.shell(shlex.quote(helper) + " --motion " + shlex.quote(path) + " 8")
    found = re.search(r"^motion=([0-9.]+)", result.get("stdout", ""), re.M)
    ctx.expect(result.get("returncode") == 0 and found is not None, "clip motion analyzer succeeded")
    score = float(found.group(1))
    ctx.note("recording " + rid + " motion=" + str(score))
    _retain(ctx, path, "mp4")
    ctx.expect(score > 1.0, "clip contains moving pattern, not a frozen desktop")


@contextmanager
def _bar_input(ctx):
    try:
        yield
    except RecipeFailed as error:
        if re.fullmatch(r"input target 'bar(?:-[a-z-]+)?' is obscured by .+", str(error)):
            # The core foreground guard refuses the gesture. Preserve the live
            # surface and independent cursor/focus readback before API cleanup.
            _frame(ctx, "bar-input-refused")
            if ctx.profile.get("session") == "x11":
                witness = ctx.shell("xdotool getmouselocation --shell; xprop -root _NET_ACTIVE_WINDOW")
                if witness.get("returncode") == 0:
                    ctx.note("X11 cursor/focus witness: " + witness.get("stdout", "").strip())
            ctx.blocked("Recording bar foreground authorization unavailable (#705): " + str(error), issue=705)
        raise


def _bar_click(ctx, control, window):
    with _bar_input(ctx):
        ctx.click(control, window=window)


def _bar_point(box, point):
    return tuple(round((point[i] - box.get(("screen_x", "screen_y")[i], 0)) * box.get("scale", 1)) for i in (0, 1))


def _bar_drag(ctx, start, end):
    with _bar_input(ctx):
        ctx.drag(start, end)


def _start(ctx, rid):
    _bar_click(ctx, "record", window="bar-ready")
    _state(ctx, rid, "recording")


def _stop(ctx, rid):
    # A changing pattern is supplied by the fixture; elapsed time alone is no proof.
    ctx.shell("sleep 2")
    _bar_click(ctx, "stop", window="bar-handle" if _inspect(ctx, rid).get("overlayPlacement") == "handle" else "bar-recording")
    reviewed = _state(ctx, rid, "reviewing")
    ctx.expect(reviewed["durationUs"] > 0, "stopped clip has retained duration")
    _motion(ctx, rid)


@recipe("region.key-arrows", "region.key-resize", needs=NEEDS)
def region_keyboard(ctx):
    with _local(ctx):
        oid = _open_region(ctx)
        try:
            _frame(ctx, "before", reference="46-region-entire-display")
            ctx.keys("Right")
            rect = _half(_screen(ctx))
            _expect_rect(ctx, rect)
            _frame(ctx, "centred", reference=REGION)
            for prefix, step in (("", 1), ("Ctrl+", 10)):
                for key, dx, dy in (("Right", step, 0), ("Down", 0, step), ("Left", -step, 0), ("Up", 0, -step)):
                    ctx.keys(prefix + key)
                    rect[0] += dx; rect[1] += dy
                    _expect_rect(ctx, rect)
            for prefix, step in (("Shift+", 1), ("Ctrl+Shift+", 10)):
                for key, dw, dh in (("Right", step, 0), ("Down", 0, step), ("Left", -step, 0), ("Up", 0, -step)):
                    ctx.keys(prefix + key)
                    rect[2] += dw; rect[3] += dh
                    _expect_rect(ctx, rect)
            _frame(ctx, "after", reference=REGION)
        finally:
            _cancel_region(ctx, oid)


@recipe("region.key-esc", "region.right-click", needs=NEEDS)
def region_cancel(ctx):
    with _local(ctx):
        for gesture in ("Esc", "right"):
            oid = _open_region(ctx, accessible=False)
            _frame(ctx, "before-" + gesture, reference="46-region-entire-display")
            _cancel_region(ctx, oid, gesture)
            _frame(ctx, "after-" + gesture)


def _capture(ctx, gesture):
    oid = _open_region(ctx, accessible=False)
    try:
        _frame(ctx, "before-" + gesture, reference="46-region-entire-display")
        if gesture == "drag":
            x, y, w, h = map(int, _half(_screen(ctx)))
            ctx.drag((x, y), (x + w, y + h))
        else:
            ctx.keys("Return")
            _, _, w, h = _screen(ctx)
        op = ctx.wait(lambda: (o if (o := _op(ctx, oid))["state"] == "completed" else None), "local capture completed")
        ctx.expect(bool(op["items"]), "capture produced an item")
        for item in op["items"]:
            ctx.expect(item["local"]["status"] == "saved", "capture saved locally")
            ctx.expect(item["uploadStatus"] != "uploaded", "local route did not publish")
            _png_dimensions(ctx, item["local"]["path"], w, h)
        _frame(ctx, "after-" + gesture)
    finally:
        if _op(ctx, oid)["state"] in ("accepted", "acquiring"):
            _call(ctx, "operation.cancel", {"id": oid})


@recipe("region.to-local", "region.key-enter", needs=NEEDS)
def region_enter(ctx):
    with _local(ctx):
        _capture(ctx, "Return")


@recipe("region.drag", needs=NEEDS)
def region_capture(ctx):
    with _local(ctx):
        _capture(ctx, "drag")
        # A click (zero-size drag) must cancel rather than capture a pixel.
        oid = _open_region(ctx, accessible=False)
        x, y, _, _ = map(int, _half(_screen(ctx)))
        _frame(ctx, "before-tiny", reference="46-region-entire-display")
        ctx.click_at(x, y)
        ctx.wait(lambda: _op(ctx, oid)["state"] in ("cancelled", "failed"), "zero-size region cancelled")
        ctx.expect(not _op(ctx, oid)["items"], "zero-size region creates no image")
        _frame(ctx, "after-tiny")


@recipe("region.to-editor", "region.outcome-line", needs=NEEDS)
def region_editor(ctx):
    with _local(ctx):
        oid = _open_region(ctx, edit=True)
        try:
            ctx.expect("Opens the image editor" in _description(ctx), "edit outcome is announced")
            _frame(ctx, "before", reference="46-region-entire-display")
            x, y, w, h = map(int, _half(_screen(ctx)))
            ctx.drag((x, y), (x + w, y + h))
            ctx.wait(lambda: _op(ctx, oid)["state"] == "editing", "capture enters editor")
            info = _call(ctx, "editor.inspect", {"id": oid})
            ctx.expect(bool(info), "editor inspects captured image")
            _frame(ctx, "after", window="editor", reference="20-editor-redact")
        finally:
            _call(ctx, "editor.cancel", {"id": oid})


@recipe("region.live-mode", "region.screenshot-first", "region.hint-pill", "region.backends", needs=NEEDS)
def region_modes(ctx):
    for mode in ("live", "screenshot_first"):
        with _local(ctx, mode):
            oid = _open_region(ctx)
            try:
                ctx.note("source capabilities: " + str(_call(ctx, "capabilities").get("source")))
                _frame(ctx, mode + "-before", reference="46-region-entire-display")
                ctx.shell("sleep 2.2")
                ctx.keys("Right")
                _expect_rect(ctx, _half(_screen(ctx)))
                _frame(ctx, mode + "-after", reference=REGION)
                ctx.review("Compare moving-pattern frames: desktop updates only in live mode; hint fades after 1.7+0.3 s; size pill follows selection; no monitor scrim. Resident inspect does not expose rendered pixels or backend identity.")
            finally:
                _cancel_region(ctx, oid)


@recipe("region.key-tab", "region.multi-display", needs=NEEDS)
def region_heads(ctx):
    _heads(ctx, False)
    _heads(ctx, True)


def _heads(ctx, recording):
    screens = _env(ctx)["screens"]
    if len(screens) != 2:
        ctx.blocked("Tab across heads requires exactly two independently sized guest heads (#664)", issue=664)
    if not recording and screens[0][2:] == screens[1][2:]:
        ctx.blocked("Screenshot Tab readback needs unequal head sizes to identify the completed output without AT-SPI (#942)", issue=942)
    if any(s.get("scale", 1) != 1 for s in ctx.profile.get("screens", [])):
        ctx.blocked("Completed-output Tab probe currently requires unit-scale fixture heads")
    with _local(ctx):
        # Completion provides an independent chosen-output assertion even when
        # the native selector has no accessible node. No selection is injected.
        for number, (keys, wanted) in enumerate(((('Tab',), screens[1]), (('Tab', 'Shift+Tab'), screens[0]), (('Shift+Tab',), screens[1]))):
            x, y, w, h = screens[0]
            oid = _open_record(ctx) if recording else _open_region(ctx, accessible=False)
            accepted = False
            try:
                ctx.scroll(int(x + w / 2), int(y + h / 2), 0, head=0)  # guarded pointer on head 0
                ref = "47-record-entire-display" if recording else "46-region-entire-display"
                _frame(ctx, "before-" + str(number), reference=ref)
                ctx.keys(*keys)
                _frame(ctx, "switched-" + str(number), reference=ref)
                ctx.keys("Return")
                if recording:
                    ready = _state(ctx, oid, "ready")
                    ctx.expect(ready["target"]["rect"] == wanted, "Tab chooses expected recording output bounds")
                else:
                    op = ctx.wait(lambda: (o if (o := _op(ctx, oid))["state"] == "completed" else None), "Tab capture completed")
                    ctx.expect(bool(op["items"]), "Tab capture produces a saved image")
                    for item in op["items"]:
                        ctx.expect(item["local"]["status"] == "saved" and item["uploadStatus"] != "uploaded", "Tab capture stays local")
                        _png_dimensions(ctx, item["local"]["path"], wanted[2], wanted[3])
                accepted = True
                ctx.note("Tab sequence " + str(keys) + " chose output " + str(wanted))
                _frame(ctx, "after-" + str(number))
            finally:
                if recording:
                    _cancel_record(ctx, oid)
                elif not accepted and _op(ctx, oid)["state"] in ("accepted", "acquiring"):
                    _cancel_region(ctx, oid)
        ctx.review("Completed output proves Tab/Shift+Tab monitor choice. Selected-rectangle transfer/clamping and both covers still require live readback/full two-head visual review (#664/#942).")


@recipe("region.to-upload", needs=NEEDS, destinations=("local",))
def region_upload(ctx):
    with _local(ctx):
        service = ctx.fixture("loopback-s3")
        ctx.expect(service.get("isolated") and service.get("loopback") and service.get("address") == "127.0.0.1",
                   "capture upload uses the isolated guest loopback service")
        current = _call(ctx, "settings.get")
        _call(ctx, "settings.patch", {"expectedRevision": current["metadata"]["acceptedRevision"],
                                     "patch": {"actions": {"region": {"destination": "upload", "profileId": service["profile_id"]}}}})
        oid = _open_region(ctx, accessible=False)
        try:
            _frame(ctx, "before-upload", reference="46-region-entire-display")
            ctx.keys("Return")
            result = ctx.wait(lambda: (o if (o := _op(ctx, oid))["state"] in ("completed", "failed", "partial") else None),
                              "synthetic region upload completed", timeout=40)
            ctx.expect(result["state"] == "completed" and bool(result["items"]), "capture upload completes")
            ctx.expect(all(item["uploadStatus"] == "uploaded" for item in result["items"]), "every captured item was uploaded")
            _frame(ctx, "after-upload")
        finally:
            if _op(ctx, oid)["state"] in ("accepted", "acquiring"):
                _call(ctx, "operation.cancel", {"id": oid})


@recipe("recsel.drag", "recsel.key-enter", "recsel.to-bar", needs=NEEDS)
def recording_selection(ctx):
    with _local(ctx):
        for gesture in ("drag", "Return", "entire"):
            rid = _open_record(ctx)
            try:
                _frame(ctx, "before-" + gesture, reference="47-record-entire-display")
                if gesture == "entire":
                    ctx.keys("Return")
                    ready = _state(ctx, rid, "ready")
                    ctx.expect(ready["target"]["rect"] == _screen(ctx), "Return with no selection chooses full active output")
                else:
                    _choose(ctx, rid, _half(_screen(ctx)) if gesture == "drag" else None)
                _frame(ctx, "after-" + gesture, window="bar", reference=BAR)
            finally:
                _cancel_record(ctx, rid)
        rid = _open_record(ctx)
        try:
            _frame(ctx, "before-tiny", reference="47-record-entire-display")
            x, y, _, _ = map(int, _half(_screen(ctx)))
            ctx.drag((x, y), (x + 3, y + 3))
            _state(ctx, rid, "cancelled")
            _frame(ctx, "after-tiny")
        finally:
            _cancel_record(ctx, rid)


@recipe("recsel.key-esc", "recsel.right-click", needs=NEEDS)
def recording_cancel(ctx):
    with _local(ctx):
        for gesture in ("Esc", "right"):
            rid = _open_record(ctx)
            try:
                _frame(ctx, "before-" + gesture, reference="47-record-entire-display")
                if gesture == "right":
                    x, y, w, h = _screen(ctx)
                    ctx.click_at(int(x + w / 2), int(y + h / 2), button="right")
                else:
                    ctx.keys("Esc")
                _state(ctx, rid, "cancelled")
                _frame(ctx, "after-" + gesture)
            finally:
                _cancel_record(ctx, rid)


@recipe("recsel.key-arrows", needs=NEEDS)
def recording_keyboard(ctx):
    with _local(ctx):
        rid = _open_record(ctx)
        try:
            _frame(ctx, "before", reference="47-record-entire-display")
            ctx.keys("Right")
            rect = _half(_screen(ctx))
            _expect_rect(ctx, rect, True)
            for prefix, index, step in (("", 0, 1), ("Ctrl+", 0, 10), ("Shift+", 2, 1), ("Ctrl+Shift+", 2, 10)):
                for key, axis, delta in (("Right", index, step), ("Down", index + 1, step), ("Left", index, -step), ("Up", index + 1, -step)):
                    ctx.keys(prefix + key)
                    rect[axis] += delta
                    _expect_rect(ctx, rect, True)
            _frame(ctx, "after", reference=RECSEL)
        finally:
            _cancel_record(ctx, rid)


@recipe("recsel.inherits-region-output", needs=NEEDS)
def recording_output(ctx):
    with _local(ctx):
        # Read the accepted normalized document and the resulting operation route.
        doc = _call(ctx, "settings.get")["document"]
        rid = _open_record(ctx)
        try:
            _frame(ctx, "before", reference="47-record-entire-display")
            record_route = doc["actions"]["record"]
            ctx.expect(doc["actions"]["region"]["destination"] == record_route.get("destination", "local") == "local", "recording route matches region local output")
            _choose(ctx, rid, _half(_screen(ctx)))
            ctx.expect(not _op(ctx, rid)["destination"].get("providerId"), "local recording has no provider destination")
            _frame(ctx, "after", window="bar", reference=BAR)
            ctx.review("Local route inspected (shortcut-only entries inherit region); upload inheritance and an explicit recording exception need the live lane's isolated provider fixture.")
        finally:
            _cancel_record(ctx, rid)


FORMAT_LABELS = {
    ("mp4", "h264"): "Compatible video — MP4 (H.264) · Recommended",
    ("mp4", "h265"): "Smaller video — MP4 (H.265)",
    ("webm", "vp9"): "Browser video — WebM (VP9)",
    ("gif", ""): "Animated GIF",
    ("webp", ""): "Animated WebP — silent",
}


def _format(ctx, fmt, codec):
    # The combo popup has its own input focus, outside the unmanaged bar.
    # Resolve the drawn item in the bar's exact private helper PID, then use
    # the core's fresh compositor/server hit-test and real pointer input.
    pid = ctx.geometry("bar")["pid"]
    _bar_click(ctx, "format", window="bar-ready")
    query = {"owned": True, "pid": pid, "role": "list item", "name": FORMAT_LABELS[(fmt, codec)]}
    ctx.wait(lambda: ctx.accessible(dict(query, visible=True, showing=True)), "recording format popup item")
    ctx.click_accessible(query)


@recipe("bar.format", "bar.system-audio", needs=NEEDS)
def bar_options(ctx):
    with _local(ctx):
        rid = _ready(ctx)
        try:
            _frame(ctx, "before", window="bar", reference=BAR)
            caps = _call(ctx, "recording.capabilities")
            if not caps.get("audio", {}).get("system"):
                ctx.blocked("System audio unavailable on this desktop")
            _bar_click(ctx, "text:System audio", window="bar-ready")
            ctx.wait(lambda: _inspect(ctx, rid)["config"]["audio"]["system"], "system audio enabled")
            for fmt, codec in FORMAT_LABELS:
                if fmt not in caps["formats"] or (codec and codec not in caps.get("codecs", {}).get(fmt, [])):
                    ctx.blocked("Required recording format unavailable: " + fmt + "/" + codec)
                _format(ctx, fmt, codec)
                info = ctx.wait(lambda: (s if (s := _inspect(ctx, rid))["config"]["format"] == fmt and s["config"]["codec"] == codec else None), "format " + fmt + "/" + codec)
                ctx.expect(info["config"]["audio"]["system"] == (fmt not in ("gif", "webp")), "animation formats are silent")
                _frame(ctx, "format-" + fmt + "-" + codec, window="bar", reference="33a-record-gif-requested-audio" if fmt == "gif" else BAR)
            _format(ctx, "mp4", "h264")
            ctx.wait(lambda: _inspect(ctx, rid)["config"]["audio"]["system"], "video restores audio intent")
            _frame(ctx, "after", window="bar", reference="33b-record-restored-audio")
        finally:
            _cancel_record(ctx, rid)


@recipe("bar.record", "bar.pause", "bar.stop", "bar.state-text", "recsel.bounds", "recsel.backends", needs=NEEDS, timeout=240)
def bar_recording(ctx):
    with _local(ctx):
        rid = _ready(ctx)
        try:
            _frame(ctx, "before", window="bar", reference=BAR)
            _start(ctx, rid)
            _frame(ctx, "recording", window="bar", reference="34-record-recording-bar")
            _frame(ctx, "recording-bounds")
            _bar_click(ctx, "text:Pause", window="bar-recording")
            _state(ctx, rid, "paused")
            _frame(ctx, "paused", window="bar", reference="35-record-paused-bar")
            _bar_click(ctx, "text:Resume", window="bar-paused")
            _state(ctx, rid, "recording")
            _stop(ctx, rid)
            _frame(ctx, "after", window="review", reference="30-review")
            ctx.review("Inspect proves phases and chosen target; check recorded bounds ring/dim strips, state dot/text/time/target, backend placement and absence of capture occlusion in the saved desktop frames.")
        finally:
            _cancel_record(ctx, rid)


@recipe("bar.cancel", "bar.key-esc", "bar.discard-question", needs=NEEDS, timeout=240)
def bar_discard(ctx):
    with _local(ctx):
        for gesture in ("cancel", "Esc"):
            rid = _ready(ctx)
            try:
                _frame(ctx, "before-ready-" + gesture, window="bar", reference=BAR)
                if gesture == "cancel":
                    _bar_click(ctx, "cancel", window="bar-ready")
                else:
                    # Give the ready bar keyboard focus through a real control.
                    _bar_click(ctx, "format", window="bar-ready")
                    ctx.keys("Esc", "Esc")
                _state(ctx, rid, "cancelled")
                _frame(ctx, "after-ready-" + gesture)
            finally:
                _cancel_record(ctx, rid)
        rid = _ready(ctx)
        try:
            _start(ctx, rid)
            _frame(ctx, "before", window="bar", reference="34-record-recording-bar")
            ctx.keys("Esc")
            ctx.expect(_inspect(ctx, rid)["state"] == "recording", "Esc while recording does nothing")
            for phase in ("recording", "paused"):
                if phase == "paused":
                    _bar_click(ctx, "text:Pause", window="bar-recording")
                    _state(ctx, rid, "paused")
                _bar_click(ctx, "cancel", window="bar-" + phase)
                _frame(ctx, "question-" + phase, window="bar", reference="36-record-discard-confirm" if phase == "recording" else "42-record-paused-discard-confirm")
                ctx.expect(_visible(ctx, "Discard this recording?"), "inline discard question visible")
                ctx.keys("Esc")
                ctx.expect(not _visible(ctx, "Discard this recording?"), "Esc answers Keep")
                ctx.expect(_inspect(ctx, rid)["state"] == phase, "Keep preserves phase")
                _bar_click(ctx, "cancel", window="bar-" + phase)
                _bar_click(ctx, "keepRecording", window="bar-question-" + phase)
                ctx.expect(_inspect(ctx, rid)["state"] == phase, "Keep button preserves phase")
            _bar_click(ctx, "cancel", window="bar-paused")
            _bar_click(ctx, "discardRecording", window="bar-question-paused")
            _state(ctx, rid, "cancelled")
            _frame(ctx, "after")
        finally:
            _cancel_record(ctx, rid)


@recipe("bar.drag", "bar.placement", needs=NEEDS)
def bar_drag(ctx):
    if ctx.profile.get("selectors", {}).get("bar_drag") is False:
        ctx.blocked("Compositor places this recording bar; X11 dragging is unavailable")
    with _local(ctx):
        rid = _ready(ctx)
        try:
            _frame(ctx, "before", window="bar", reference=BAR)
            before = ctx.geometry("bar")
            with _bar_input(ctx):
                ctx.focus("bar", raise_window=False)
            ctx.expect(_inspect(ctx, rid)["overlayPlacement"] == "outside", "half-screen target keeps bar outside")
            # Top border is a safe non-control drag target.
            start = (int(before["x"] + before["width"] / 2), int(before["y"] + 2))
            _bar_drag(ctx, _bar_point(before, start), _bar_point(before, (start[0] + 30, start[1])))
            after = ctx.geometry("bar")
            ctx.expect(after["x"] != before["x"] or after["y"] != before["y"], "bar moved under pointer drag")
            x, y, w, h = _inspect(ctx, rid)["target"]["rect"]
            ctx.expect(after["x"] + after["width"] <= x or after["x"] >= x + w or after["y"] + after["height"] <= y or after["y"] >= y + h, "bar stays outside captured pixels")
            _frame(ctx, "after", window="bar", reference=BAR)
            inside = (int(x + w / 2), int(y + h / 2))
            screen = _env(ctx)["screens"][after.get("head", 0)]
            sx, sy, sw, sh = screen
            if not (sx <= inside[0] < sx + sw and sy <= inside[1] < sy + sh):
                ctx.blocked("Capture exclusion target is on another head; public drag() accepts one head per gesture (#714)", issue=714)
            _bar_drag(ctx, _bar_point(after, (int(after["x"] + after["width"] / 2), int(after["y"] + 2))), _bar_point(after, inside))
            rejected = ctx.geometry("bar")
            ctx.expect(rejected["x"] + rejected["width"] <= x or rejected["x"] >= x + w or rejected["y"] + rejected["height"] <= y or rejected["y"] >= y + h, "dragging an outside bar cannot overlap the capture")
            _frame(ctx, "rejected-inside", window="bar", reference=BAR)
            ctx.review("Outside placement and drag exclusion are inspected here; above/other-screen/compositor placements need their own fixture geometry (#664).")
        finally:
            _cancel_record(ctx, rid)


@recipe("bar.handle", needs=NEEDS, timeout=240)
def bar_handle(ctx):
    if len(_env(ctx)["screens"]) != 1:
        ctx.blocked("Handle requires a single-head full-screen target; other head may fit the full bar")
    with _local(ctx):
        rid = _ready(ctx, entire=True)
        try:
            _frame(ctx, "before", window="bar", reference=BAR)
            ctx.expect(_inspect(ctx, rid)["overlayPlacement"] == "handle", "full-screen target requires handle")
            full = ctx.geometry("bar")
            _start(ctx, rid)
            handle = ctx.geometry("bar")
            ctx.expect(handle["width"] < full["width"], "recording collapses the ready bar")
            _frame(ctx, "handle", window="bar")
            _stop(ctx, rid)
            _frame(ctx, "after", window="review", reference="30-review")
            ctx.review("Handle Stop and width collapse are exercised. Resume on a paused handle still needs a fixture with a known paused session; the handle has no Pause control (#664).")
        finally:
            _cancel_record(ctx, rid)


@recipe("bar.hidden-overlay", needs=NEEDS, timeout=240)
def bar_hidden(ctx):
    # No user gesture exists for this API capability. Do not substitute a CLI
    # state transition for real-input evidence.
    with _local(ctx):
        rid = _ready(ctx, entire=True)
        try:
            _frame(ctx, "before", window="bar", reference=BAR)
            _frame(ctx, "after")
            ctx.blocked("Hidden overlay is API-only (recording.overlay); no public key or control exists to exercise it with real input. API-lane lifecycle/occlusion coverage is required.")
        finally:
            _cancel_record(ctx, rid)
