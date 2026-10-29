"""Real-input screenshot/recording selectors and recording controls.

The ``selector-desktop`` fixture (core request #664) owns a moving synthetic
pattern and its cleanup, and supplies ordered logical ``screens`` rectangles,
an absolute private ``output_dir`` and the exact-build ``record_helper`` path.
No fixture supplies the result of an interaction. AT-SPI descriptions are the
current screenshot selector's only live geometry readback; completed captures
and every recording transition are checked through resident inspection.
"""
from __future__ import annotations

import re
import shlex
from contextlib import contextmanager
from pathlib import PurePosixPath

from tools.lab.surface_driver.api import RecipeBlocked, RecipeFailed, recipe

# The initial core runner has no selector fixture. A campaign can supply its
# equivalent immutable metadata in the selectors profile fragment until #664
# lands. Keep setup outside this module; recipes never implement input/runner code.
NEEDS = ()
REGION = "44-region-keyboard"
RECSEL = "45-record-region-keyboard"
BAR = "33-record-ready-bar"


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
    value = ctx.profile.get("selectors", {}).get("fixture")
    if value is None:
        try:
            value = ctx.fixture("selector-desktop")
        except RecipeBlocked:
            ctx.blocked("selector-desktop fixture or selectors.fixture profile metadata is required (#664)", issue=664)
    if not isinstance(value, dict) or not value.get("screens"):
        ctx.blocked("selector-desktop must supply live logical screen rectangles (#664)", issue=664)
    return value


def _screen(ctx):
    return _env(ctx)["screens"][0]


def _half(screen):
    x, y, w, h = screen
    return [x + w / 4, y + h / 4, w / 2, h / 2]


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
        if env.get("prepare_command"):
            prepared = ctx.shell(env["prepare_command"])
            ctx.expect(prepared.get("returncode") == 0, "moving pattern fixture is visible")
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
        ctx.blocked("Live selector accessibility description unavailable; no resident selection inspect (#664)", issue=664)
    return descriptions[0]


def _rect(ctx, recording=False):
    text = _description(ctx, recording)
    match = re.search(r"Region ([\d.]+) × ([\d.]+) at (-?[\d.]+), (-?[\d.]+)\.", text)
    if match is None:
        return None
    w, h, x, y = map(float, match.groups())
    return [x, y, w, h]


def _expect_rect(ctx, wanted, recording=False):
    ctx.wait(lambda: _rect(ctx, recording) == wanted, "selector rectangle " + str(wanted))
    ctx.note("Inspected selector rectangle " + str(wanted))


def _open_region(ctx, edit=False):
    # action.invoke durably acknowledges before interactive acquisition finishes.
    # Surface setup therefore does not depend on the shell window's layout.
    opened = _result(ctx.view("action.invoke", {"action": "edit" if edit else "region"}))
    ctx.expect(bool(opened.get("operationId")), "capture action returns a durable operation identity")
    oid = opened["operationId"]
    ctx.note("Capture operation " + oid)
    try:
        ctx.wait(lambda: ctx.accessible({"name": "Capture region"}), "screenshot selector opened")
    except Exception as error:
        ctx.frame("selector-readback-unavailable")
        observed = _op(ctx, oid)
        if observed["state"] == "failed":
            ctx.expect(False, "capture failed before selector readback: " + observed.get("error", {}).get("code", "unknown"))
        _cancel_region(ctx, oid)
        if isinstance(error, RecipeFailed) and observed["state"] in ("accepted", "acquiring"):
            ctx.blocked("Capture selector has no live AT-SPI node in this session; resident has no selection inspect fallback (#664)", issue=664)
        raise
    return oid


def _op(ctx, oid):
    return _call(ctx, "operation.inspect", {"id": oid})


def _cancel_region(ctx, oid, key="Esc"):
    if key == "right":
        x, y, w, h = _screen(ctx)
        ctx.click_at(int(x + w / 2), int(y + h / 2), button="right")
    else:
        ctx.keys(key)
    op = ctx.wait(lambda: (o if (o := _op(ctx, oid))["state"] in ("cancelled", "failed") else None), "capture cancelled")
    ctx.expect(op.get("cancelled") or op.get("error", {}).get("code") == "cancelled", "cancel has no capture result")
    ctx.expect(not op.get("items"), "cancel creates no artifact")


def _open_record(ctx, entire=False):
    caps = _call(ctx, "recording.capabilities")
    if not caps.get("formats"):
        ctx.blocked("This desktop has no recording formats")
    target = {"kind": "region"}
    if entire:
        target["rect"] = _screen(ctx)
    opened = _result(ctx.view("recording.prepare", {"target": target, "format": "mp4", "codec": "h264", "fps": 15, "audio": {"system": False}}))
    ctx.expect(bool(opened.get("recordingId")), "recording prepare returns an operation identity")
    rid = opened["recordingId"]
    ctx.note("Recording operation " + rid)
    if entire:
        _state(ctx, rid, "ready")
    else:
        ctx.wait(lambda: _inspect(ctx, rid).get("selecting"), "recording selector opened")
    return rid


def _inspect(ctx, rid):
    return _call(ctx, "recording.inspect", {"id": rid})


def _state(ctx, rid, wanted):
    if wanted == "cancelled":
        # Cancel retires the active recording session. Its durable operation is
        # the inspectable result, not recording.inspect (which is now unavailable).
        return ctx.wait(lambda: (s if (s := _op(ctx, rid))["state"] == wanted else None), "recording cancelled")
    return ctx.wait(lambda: (s if (s := _inspect(ctx, rid))["state"] == wanted else None), "recording " + wanted, timeout=45)


def _choose(ctx, rid, rect=None):
    if rect is None:
        ctx.keys("Right")
        rect = _half(_screen(ctx))
        _expect_rect(ctx, rect, True)
        ctx.keys("Return")
    else:
        x, y, w, h = rect
        ctx.drag((int(x), int(y)), (int(x + w), int(y + h)))
    ready = _state(ctx, rid, "ready")
    ctx.expect(ready["target"]["rect"] == rect, "chosen recording bounds match real input")
    ctx.expect(not ready["selecting"], "selector closes before ready bar")
    ctx.note("Inspected recording target " + str(rect))
    return ready


def _cancel_record(ctx, rid):
    # Cleanup is not claimed as an input interaction.
    if _call(ctx, "recording.capabilities").get("active") == rid:
        _call(ctx, "recording.cancel", {"id": rid})


def _ready(ctx, entire=False):
    rid = _open_record(ctx, entire)
    try:
        if not entire:
            _choose(ctx, rid, _half(_screen(ctx)))
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
    ctx.expect(not (done["lastExport"].get("error") or {}).get("code"), "motion probe export succeeded")
    result = ctx.shell(shlex.quote(helper) + " --motion " + shlex.quote(path) + " 8")
    found = re.search(r"^motion=([0-9.]+)", result.get("stdout", ""), re.M)
    ctx.expect(result.get("returncode") == 0 and found is not None, "clip motion analyzer succeeded")
    score = float(found.group(1))
    ctx.note("recording " + rid + " motion=" + str(score))
    ctx.expect(score > 1.0, "clip contains moving pattern, not a frozen desktop")


def _start(ctx, rid):
    ctx.click("record", window="bar")
    _state(ctx, rid, "recording")


def _stop(ctx, rid):
    # A changing pattern is supplied by the fixture; elapsed time alone is no proof.
    ctx.shell("sleep 2")
    ctx.click("stop", window="bar")
    reviewed = _state(ctx, rid, "reviewing")
    ctx.expect(reviewed["durationUs"] > 0, "stopped clip has retained duration")
    _motion(ctx, rid)


@recipe("region.key-arrows", "region.key-resize", needs=NEEDS)
def region_keyboard(ctx):
    with _local(ctx):
        oid = _open_region(ctx)
        try:
            ctx.frame("before", reference="46-region-entire-display")
            ctx.keys("Right")
            rect = _half(_screen(ctx))
            _expect_rect(ctx, rect)
            ctx.frame("centred", reference=REGION)
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
            ctx.frame("after", reference=REGION)
        finally:
            _cancel_region(ctx, oid)


@recipe("region.key-esc", "region.right-click", needs=NEEDS)
def region_cancel(ctx):
    with _local(ctx):
        for gesture in ("Esc", "right"):
            oid = _open_region(ctx)
            ctx.frame("before-" + gesture, reference="46-region-entire-display")
            _cancel_region(ctx, oid, gesture)
            ctx.frame("after-" + gesture)


@recipe("region.drag", "region.to-local", "region.key-enter", needs=NEEDS)
def region_capture(ctx):
    with _local(ctx):
        for gesture in ("drag", "Return"):
            oid = _open_region(ctx)
            ctx.frame("before-" + gesture, reference="46-region-entire-display")
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
                # Decode only our synthetic capture, on the guest, to check accepted bounds.
                path = shlex.quote(item["local"]["path"])
                probe = ctx.shell("python3 -c 'import struct,sys; print(*struct.unpack(\"!II\",open(sys.argv[1],\"rb\").read(24)[16:24]))' " + path)
                ctx.expect(probe.get("returncode") == 0 and probe.get("stdout", "").strip() == f"{int(w)} {int(h)}", "capture dimensions match chosen area")
            ctx.frame("after-" + gesture)
        # A click (zero-size drag) must cancel rather than capture a pixel.
        oid = _open_region(ctx)
        x, y, _, _ = map(int, _half(_screen(ctx)))
        ctx.frame("before-tiny", reference="46-region-entire-display")
        ctx.click_at(x, y)
        ctx.wait(lambda: _op(ctx, oid)["state"] in ("cancelled", "failed"), "zero-size region cancelled")
        ctx.expect(not _op(ctx, oid)["items"], "zero-size region creates no image")
        ctx.frame("after-tiny")


@recipe("region.to-editor", "region.outcome-line", needs=NEEDS)
def region_editor(ctx):
    with _local(ctx):
        oid = _open_region(ctx, edit=True)
        try:
            ctx.expect("Opens the image editor" in _description(ctx), "edit outcome is announced")
            ctx.frame("before", reference="46-region-entire-display")
            x, y, w, h = map(int, _half(_screen(ctx)))
            ctx.drag((x, y), (x + w, y + h))
            ctx.wait(lambda: _op(ctx, oid)["state"] == "editing", "capture enters editor")
            info = _call(ctx, "editor.inspect", {"id": oid})
            ctx.expect(bool(info), "editor inspects captured image")
            ctx.frame("after", window="editor", reference="20-editor-redact")
        finally:
            _call(ctx, "editor.cancel", {"id": oid})


@recipe("region.live-mode", "region.screenshot-first", "region.hint-pill", "region.backends", needs=NEEDS)
def region_modes(ctx):
    for mode in ("live", "screenshot_first"):
        with _local(ctx, mode):
            oid = _open_region(ctx)
            try:
                ctx.note("source capabilities: " + str(_call(ctx, "capabilities").get("source")))
                ctx.frame(mode + "-before", reference="46-region-entire-display")
                ctx.shell("sleep 2.2")
                ctx.keys("Right")
                _expect_rect(ctx, _half(_screen(ctx)))
                ctx.frame(mode + "-after", reference=REGION)
                ctx.review("Compare moving-pattern frames: desktop updates only in live mode; hint fades after 1.7+0.3 s; size pill follows selection; no monitor scrim. Resident inspect does not expose rendered pixels or backend identity.")
            finally:
                _cancel_region(ctx, oid)


@recipe("region.key-tab", "region.multi-display", needs=NEEDS)
def region_heads(ctx):
    _heads(ctx, False)


def _heads(ctx, recording):
    screens = _env(ctx)["screens"]
    if len(screens) != 2:
        ctx.blocked("Tab across heads requires exactly two independently sized guest heads (#664)", issue=664)
    with _local(ctx):
        oid = _open_record(ctx) if recording else _open_region(ctx)
        ref = RECSEL if recording else REGION
        try:
            ctx.keys("Right")
            rect = _half(screens[0])
            _expect_rect(ctx, rect, recording)
            ctx.frame("before", reference=ref)
            ctx.frame("all-heads-before")
            a, b = screens
            transferred = [b[0] + min(rect[0] - a[0], b[2] - min(rect[2], b[2])),
                           b[1] + min(rect[1] - a[1], b[3] - min(rect[3], b[3])), min(rect[2], b[2]), min(rect[3], b[3])]
            ctx.keys("Tab")
            _expect_rect(ctx, transferred, recording)
            ctx.expect("Display 2" in _description(ctx, recording), "Tab changes active output")
            ctx.frame("second-head", reference=ref)
            ctx.frame("all-heads-second")
            ctx.keys("Shift+Tab")
            # Same-size heads round trip exactly; unequal heads preserve the clamped size.
            back = [a[0] + min(transferred[0] - b[0], a[2] - transferred[2]), a[1] + min(transferred[1] - b[1], a[3] - transferred[3]), transferred[2], transferred[3]]
            _expect_rect(ctx, back, recording)
            ctx.expect("Display 1" in _description(ctx, recording), "Shift+Tab returns to first output")
            ctx.frame("after", reference=ref)
            ctx.review("Both output covers and absence of dim on the other monitor require full-desktop visual review; AT-SPI proves selection transfer only.")
        finally:
            if recording:
                _cancel_record(ctx, oid)
            else:
                _cancel_region(ctx, oid)


@recipe("region.to-upload", destinations=("local", "s3", "b2", "proton"))
def region_upload(ctx):
    ctx.blocked("Selectors campaign authorizes local synthetic output only; isolated provider upload belongs to the live lane. No S3 or Proton on boat.")


@recipe("recsel.drag", "recsel.key-enter", "recsel.to-bar", needs=NEEDS)
def recording_selection(ctx):
    with _local(ctx):
        for gesture in ("drag", "Return", "entire"):
            rid = _open_record(ctx)
            try:
                ctx.frame("before-" + gesture, reference="47-record-entire-display")
                if gesture == "entire":
                    ctx.keys("Return")
                    ready = _state(ctx, rid, "ready")
                    ctx.expect(ready["target"]["rect"] == _screen(ctx), "Return with no selection chooses full active output")
                else:
                    _choose(ctx, rid, _half(_screen(ctx)) if gesture == "drag" else None)
                ctx.frame("after-" + gesture, window="bar", reference=BAR)
            finally:
                _cancel_record(ctx, rid)
        rid = _open_record(ctx)
        try:
            ctx.frame("before-tiny", reference="47-record-entire-display")
            x, y, _, _ = map(int, _half(_screen(ctx)))
            ctx.drag((x, y), (x + 3, y + 3))
            _state(ctx, rid, "cancelled")
            ctx.frame("after-tiny")
        finally:
            _cancel_record(ctx, rid)


@recipe("recsel.key-esc", "recsel.right-click", needs=NEEDS)
def recording_cancel(ctx):
    with _local(ctx):
        for gesture in ("Esc", "right"):
            rid = _open_record(ctx)
            try:
                ctx.frame("before-" + gesture, reference="47-record-entire-display")
                if gesture == "right":
                    x, y, w, h = _screen(ctx)
                    ctx.click_at(int(x + w / 2), int(y + h / 2), button="right")
                else:
                    ctx.keys("Esc")
                _state(ctx, rid, "cancelled")
                ctx.frame("after-" + gesture)
            finally:
                _cancel_record(ctx, rid)


@recipe("recsel.key-arrows", needs=NEEDS)
def recording_keyboard(ctx):
    with _local(ctx):
        rid = _open_record(ctx)
        try:
            ctx.frame("before", reference="47-record-entire-display")
            ctx.keys("Right")
            rect = _half(_screen(ctx))
            _expect_rect(ctx, rect, True)
            for prefix, index, step in (("", 0, 1), ("Ctrl+", 0, 10), ("Shift+", 2, 1), ("Ctrl+Shift+", 2, 10)):
                for key, axis, delta in (("Right", index, step), ("Down", index + 1, step), ("Left", index, -step), ("Up", index + 1, -step)):
                    ctx.keys(prefix + key)
                    rect[axis] += delta
                    _expect_rect(ctx, rect, True)
            ctx.frame("after", reference=RECSEL)
        finally:
            _cancel_record(ctx, rid)
    # This id also promises display switching, so one-head evidence cannot pass it.
    _heads(ctx, True)


@recipe("recsel.inherits-region-output", needs=NEEDS)
def recording_output(ctx):
    with _local(ctx):
        # Read the accepted normalized document and the resulting operation route.
        doc = _call(ctx, "settings.get")["document"]
        rid = _open_record(ctx)
        try:
            ctx.frame("before", reference="47-record-entire-display")
            record_route = doc["actions"]["record"]
            ctx.expect(doc["actions"]["region"]["destination"] == record_route.get("destination", "local") == "local", "recording route matches region local output")
            _choose(ctx, rid, _half(_screen(ctx)))
            ctx.expect(not _op(ctx, rid)["destination"].get("providerId"), "local recording has no provider destination")
            ctx.frame("after", window="bar", reference=BAR)
            ctx.review("Local route inspected (shortcut-only entries inherit region); upload inheritance and an explicit recording exception need the live lane's isolated provider fixture.")
        finally:
            _cancel_record(ctx, rid)


@recipe("bar.format", "bar.system-audio", needs=NEEDS)
def bar_options(ctx):
    with _local(ctx):
        rid = _ready(ctx)
        try:
            ctx.frame("before", window="bar", reference=BAR)
            caps = _call(ctx, "recording.capabilities")
            if not caps.get("audio", {}).get("system"):
                ctx.blocked("System audio unavailable on this desktop")
            ctx.click("text:System audio", window="bar")
            ctx.wait(lambda: _inspect(ctx, rid)["config"]["audio"]["system"], "system audio enabled")
            # Format order is the catalog's real combo order, not a translated label.
            for index, (fmt, codec) in enumerate((("mp4", "h264"), ("mp4", "h265"), ("webm", "vp9"), ("gif", ""), ("webp", ""))):
                if fmt not in caps["formats"] or (codec and codec not in caps.get("codecs", {}).get(fmt, [])):
                    ctx.blocked("Required recording format unavailable: " + fmt + "/" + codec)
                ctx.click("format", window="bar")
                ctx.keys("Home", *(["Down"] * index), "Return")
                info = ctx.wait(lambda: (s if (s := _inspect(ctx, rid))["config"]["format"] == fmt and s["config"]["codec"] == codec else None), "format " + fmt + "/" + codec)
                ctx.expect(info["config"]["audio"]["system"] == (fmt not in ("gif", "webp")), "animation formats are silent")
                ctx.frame("format-" + fmt + "-" + codec, window="bar", reference="33a-record-gif-requested-audio" if fmt == "gif" else BAR)
            ctx.click("format", window="bar")
            ctx.keys("Home", "Return")
            ctx.wait(lambda: _inspect(ctx, rid)["config"]["audio"]["system"], "video restores audio intent")
            ctx.frame("after", window="bar", reference="33b-record-restored-audio")
        finally:
            _cancel_record(ctx, rid)


@recipe("bar.record", "bar.pause", "bar.stop", "bar.state-text", "recsel.bounds", "recsel.backends", needs=NEEDS, timeout=240)
def bar_recording(ctx):
    with _local(ctx):
        rid = _ready(ctx)
        try:
            ctx.frame("before", window="bar", reference=BAR)
            _start(ctx, rid)
            ctx.frame("recording", window="bar", reference="34-record-recording-bar")
            ctx.frame("recording-bounds")
            ctx.click("text:Pause", window="bar")
            _state(ctx, rid, "paused")
            ctx.frame("paused", window="bar", reference="35-record-paused-bar")
            ctx.click("text:Resume", window="bar")
            _state(ctx, rid, "recording")
            _stop(ctx, rid)
            ctx.frame("after", window="review", reference="30-review")
            ctx.review("Inspect proves phases and chosen target; check recorded bounds ring/dim strips, state dot/text/time/target, backend placement and absence of capture occlusion in the saved desktop frames.")
        finally:
            _cancel_record(ctx, rid)


@recipe("bar.cancel", "bar.key-esc", "bar.discard-question", needs=NEEDS, timeout=240)
def bar_discard(ctx):
    with _local(ctx):
        for gesture in ("cancel", "Esc"):
            rid = _ready(ctx)
            try:
                ctx.frame("before-ready-" + gesture, window="bar", reference=BAR)
                ctx.click("cancel", window="bar") if gesture == "cancel" else ctx.keys("Esc")
                _state(ctx, rid, "cancelled")
                ctx.frame("after-ready-" + gesture)
            finally:
                _cancel_record(ctx, rid)
        rid = _ready(ctx)
        try:
            _start(ctx, rid)
            ctx.frame("before", window="bar", reference="34-record-recording-bar")
            ctx.keys("Esc")
            ctx.expect(_inspect(ctx, rid)["state"] == "recording", "Esc while recording does nothing")
            for phase in ("recording", "paused"):
                if phase == "paused":
                    ctx.click("text:Pause", window="bar")
                    _state(ctx, rid, "paused")
                ctx.click("cancel", window="bar")
                ctx.frame("question-" + phase, window="bar", reference="36-record-discard-confirm" if phase == "recording" else "42-record-paused-discard-confirm")
                ctx.expect(bool(ctx.accessible({"name": "Discard this recording?"})), "inline discard question visible")
                ctx.keys("Esc")
                ctx.expect(not ctx.accessible({"name": "Discard this recording?"}), "Esc answers Keep")
                ctx.expect(_inspect(ctx, rid)["state"] == phase, "Keep preserves phase")
                ctx.click("cancel", window="bar")
                ctx.click("keepRecording", window="bar")
                ctx.expect(_inspect(ctx, rid)["state"] == phase, "Keep button preserves phase")
            ctx.click("cancel", window="bar")
            ctx.click("discardRecording", window="bar")
            _state(ctx, rid, "cancelled")
            ctx.frame("after")
        finally:
            _cancel_record(ctx, rid)


@recipe("bar.drag", "bar.placement", needs=NEEDS)
def bar_drag(ctx):
    if ctx.profile.get("selectors", {}).get("bar_drag") is False:
        ctx.blocked("Compositor places this recording bar; X11 dragging is unavailable")
    with _local(ctx):
        rid = _ready(ctx)
        try:
            ctx.frame("before", window="bar", reference=BAR)
            before = ctx.geometry("bar")
            ctx.expect(_inspect(ctx, rid)["overlayPlacement"] == "outside", "half-screen target keeps bar outside")
            # Top border is a safe non-control drag target.
            start = (int(before["x"] + before["width"] / 2), int(before["y"] + 2))
            ctx.drag(start, (start[0] + 30, start[1]))
            after = ctx.geometry("bar")
            ctx.expect(after["x"] != before["x"] or after["y"] != before["y"], "bar moved under pointer drag")
            x, y, w, h = _inspect(ctx, rid)["target"]["rect"]
            ctx.expect(after["x"] + after["width"] <= x or after["x"] >= x + w or after["y"] + after["height"] <= y or after["y"] >= y + h, "bar stays outside captured pixels")
            ctx.frame("after", window="bar", reference=BAR)
            ctx.drag((int(after["x"] + after["width"] / 2), int(after["y"] + 2)), (int(x + w / 2), int(y + h / 2)))
            rejected = ctx.geometry("bar")
            ctx.expect(rejected["x"] + rejected["width"] <= x or rejected["x"] >= x + w or rejected["y"] + rejected["height"] <= y or rejected["y"] >= y + h, "dragging an outside bar cannot overlap the capture")
            ctx.frame("rejected-inside", window="bar", reference=BAR)
        finally:
            _cancel_record(ctx, rid)


@recipe("bar.handle", needs=NEEDS, timeout=240)
def bar_handle(ctx):
    if len(_env(ctx)["screens"]) != 1:
        ctx.blocked("Handle requires a single-head full-screen target; other head may fit the full bar")
    with _local(ctx):
        rid = _ready(ctx, entire=True)
        try:
            ctx.frame("before", window="bar", reference=BAR)
            ctx.expect(_inspect(ctx, rid)["overlayPlacement"] == "handle", "full-screen target requires handle")
            full = ctx.geometry("bar")
            _start(ctx, rid)
            handle = ctx.geometry("bar")
            ctx.expect(handle["width"] < full["width"], "recording collapses the ready bar")
            ctx.frame("handle", window="bar")
            _stop(ctx, rid)
            ctx.frame("after", window="review", reference="30-review")
        finally:
            _cancel_record(ctx, rid)


@recipe("bar.hidden-overlay", needs=NEEDS, timeout=240)
def bar_hidden(ctx):
    # No user gesture exists for this API capability. Do not substitute a CLI
    # state transition for real-input evidence.
    with _local(ctx):
        rid = _ready(ctx, entire=True)
        try:
            ctx.frame("before", window="bar", reference=BAR)
            ctx.frame("after")
            ctx.blocked("Hidden overlay is API-only (recording.overlay); no public key or control exists to exercise it with real input. API-lane lifecycle/occlusion coverage is required.")
        finally:
            _cancel_record(ctx, rid)
