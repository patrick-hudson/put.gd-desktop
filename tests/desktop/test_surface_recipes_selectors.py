#!/usr/bin/env python3
"""Recipe orchestration contracts only; these are never desktop evidence."""
from __future__ import annotations

import copy
import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.lab.surface_driver.api import Context, RecipeBlocked, RecipeFailed, REGISTRY
from tools.lab.surface_driver.recipes import selectors


def merge(target, patch):
    for key, value in patch.items():
        if isinstance(value, dict):
            merge(target.setdefault(key, {}), value)
        else:
            target[key] = value


class FakeContext(Context):
    desktop = "mint-cinnamon-x11"
    host = "boat"
    placement = "boat"
    package = "deb"
    destination = "local"
    profile = {}

    def __init__(self, heads=1):
        self.env = {"screens": [[0, 0, 1280, 800], [1280, 0, 1024, 768]][:heads],
                    "output_dir": "/tmp/sel-artifacts", "record_helper": "/opt/test/putgd-record"}
        self.document = {"actions": {a: {"destination": "default", "profileId": None, "shortcut": ""} for a in ("region", "edit", "record")},
                         "archive": {"keepUploads": False, "root": "/tmp/original", "folders": {k: None for k in ("images", "videos", "text", "other")}},
                         "preferences": {"regionSelectionMode": "live"}}
        self.original = copy.deepcopy(self.document)
        self.revision = 1
        self.operations = {}
        self.recordings = {}
        self.active = None
        self.surface = None
        self.selection = None
        self.head = 0
        self.question = False
        self.audio_intent = False
        self.bar = dict(x=170, y=650, width=850, height=100, scale=1)
        self.frames = []
        self.inputs = []
        self.calls = []
        self.reviews = []
        self.freeze_selection = False
        self.motion = 5.0
        self.no_accessibility = False
        self.last_size = None

    def fixture(self, name):
        assert name == "selector-desktop"
        return self.env

    def wait(self, predicate, what, timeout=10, interval=0.2):
        result = predicate()
        if not result:
            raise RecipeFailed(what)
        return result

    def view(self, method, params=None):
        if method == "recording.prepare":
            return self.call(method, params)
        if method == "action.invoke":
            oid = self._operation(params["action"])
            self.surface, self.selection, self.head = "region", None, 0
            return {"operationId": oid}
        assert method == "ui.show" and params == {"view": "history"}
        self.surface = "history"
        return {"shown": "history"}

    def call(self, method, params=None):
        self.calls.append((method, copy.deepcopy(params)))
        params = params or {}
        if method == "settings.get":
            return {"document": copy.deepcopy(self.document), "metadata": {"acceptedRevision": str(self.revision)}}
        if method == "settings.patch":
            assert params["expectedRevision"] == str(self.revision), "stale configuration write"
            for route in params["patch"].get("actions", {}).values():
                if "profileId" in route:
                    assert route["profileId"] is None, "local profile reference must be JSON null"
            merge(self.document, copy.deepcopy(params["patch"]))
            self.revision += 1
            return {"acceptedRevision": str(self.revision)}
        if method == "capabilities":
            return {"source": {"region": True}}
        if method == "operation.recent":
            return {"operations": list(copy.deepcopy(self.operations).values())}
        if method == "operation.inspect":
            return copy.deepcopy(self.operations[params["id"]])
        if method == "operation.cancel":
            self.operations[params["id"]].update(state="cancelled", cancelled=True)
            self.surface = "history"
            return {}
        if method == "recording.capabilities":
            return {"active": self.active if self.active in self.recordings and self._record()["state"] != "cancelled" else None,
                    "formats": ["mp4", "webm", "gif", "webp"], "codecs": {"mp4": ["h264", "h265"], "webm": ["vp9"]}, "audio": {"system": True}}
        if method == "recording.prepare":
            rid = self._operation("record")
            target = copy.deepcopy(params["target"])
            entire = "rect" in target
            self.recordings[rid] = {"recordingId": rid, "state": "ready" if entire else "preparing", "selecting": not entire,
                                    "target": target, "config": {"format": "mp4", "codec": "h264", "audio": {"system": False}},
                                    "overlayPlacement": "handle" if entire else "outside", "durationUs": 0, "lastExport": None,
                                    "error": {"code": "", "message": "", "details": {}}}
            self.surface = "bar" if entire else "recsel"
            self.selection = None
            self.head = 0
            self.bar.update(x=170, y=650, width=850)
            return {"recordingId": rid}
        if method == "recording.inspect":
            assert self.recordings[params["id"]]["state"] != "cancelled", "cancelled session is retired"
            return copy.deepcopy(self.recordings[params["id"]])
        if method == "recording.cancel":
            self.recordings[params["id"]]["state"] = "cancelled"
            self.operations[params["id"]]["state"] = "cancelled"
            self.surface = "history"
            return {}
        if method == "recording.saveCopy":
            self.recordings[params["id"]]["lastExport"] = {"status": "saved", "path": params["path"], "error": {"code": ""}}
            return {"state": "exporting"}
        if method == "editor.inspect":
            assert self.operations[params["id"]]["state"] == "editing"
            return {"width": 640, "height": 400}
        if method == "editor.cancel":
            self.operations[params["id"]]["state"] = "cancelled"
            self.surface = "history"
            return {}
        raise AssertionError("Unexpected command (possible faked input): " + method)

    def _operation(self, action):
        oid = "synthetic-" + str(len(self.operations) + 1)
        self.active = oid
        self.operations[oid] = {"id": oid, "action": action, "state": "acquiring", "items": [], "destination": {}, "error": {"code": ""}}
        return oid

    def _record(self):
        return self.recordings[self.active]

    def accessible(self, query):
        if self.no_accessibility:
            return []
        if query["name"] == "Discard this recording?":
            return [{"name": query["name"], "rect": [10, 10, 300, 24] if self.question else [0, 0, 0, 0]}]
        if query["name"] != {"region": "Capture region", "recsel": "Recording region"}.get(self.surface):
            return []
        prefix = "No region selected."
        if self.selection:
            x, y, w, h = self.selection
            prefix = f"Region {w:g} × {h:g} at {x:g}, {y:g}."
        return [{"name": query["name"], "description": prefix + f" Display {self.head + 1} · Opens the image editor"}]

    def frame(self, label, window=None, reference=None):
        self.frames.append((label, window, reference))

    def note(self, text):
        pass

    def review(self, reason):
        self.reviews.append(reason)

    def focus(self, window, raise_window=True):
        assert window in ("region", "recsel")

    def geometry(self, window):
        assert window == "bar"
        return self.bar.copy()

    def click(self, control, window, button="left", offset=None):
        self.inputs.append(("click", control, window))
        if window == "history":
            assert self.surface == "history"
            self._operation("edit" if "& edit" in control else "region")
            self.surface, self.selection, self.head = "region", None, 0
        elif control == "record":
            self._record()["state"] = "recording"
            if self._record()["overlayPlacement"] == "handle":
                self.bar["width"] = 150
        elif control == "text:Pause":
            self._record()["state"] = "paused"
        elif control == "text:Resume":
            self._record()["state"] = "recording"
        elif control == "stop":
            self._record().update(state="reviewing", durationUs=2_000_000)
            self.surface = "review"
        elif control == "cancel":
            if self._record()["state"] == "ready":
                self._record()["state"] = "cancelled"
                self.operations[self.active]["state"] = "cancelled"
                self.surface = "history"
            else:
                self.question = True
        elif control == "keepRecording":
            self.question = False
        elif control == "discardRecording":
            self.question = False
            self._record()["state"] = "cancelled"
            self.operations[self.active]["state"] = "cancelled"
            self.surface = "history"
        elif control == "text:System audio":
            self.audio_intent = not self.audio_intent
            self._record()["config"]["audio"]["system"] = self.audio_intent
        elif control == "format":
            self.surface = "format"
            self.combo_index = 0
        else:
            raise AssertionError(control)

    def _cancel(self):
        if self.surface == "recsel":
            self._record().update(state="cancelled", selecting=False)
            self.operations[self.active]["state"] = "cancelled"
        else:
            self.operations[self.active].update(state="failed", error={"code": "cancelled"})
        self.surface = "history"

    def click_at(self, x, y, button="left"):
        self.inputs.append(("click_at", x, y, button))
        assert self.surface in ("region", "recsel")
        self._cancel()

    def drag(self, start, end, button="left", steps=12):
        self.inputs.append(("drag", start, end))
        if self.surface == "bar":
            # Reject a drag into the selected area; only horizontal outside move.
            if end[1] == start[1]:
                self.bar["x"] += end[0] - start[0]
            return
        self.selection = [min(start[0], end[0]), min(start[1], end[1]), abs(end[0] - start[0]), abs(end[1] - start[1])]
        if self.surface == "recsel" and min(self.selection[2:]) < 4:
            self._cancel()
        else:
            self._accept()

    def _accept(self):
        rect = self.selection or self.env["screens"][self.head]
        if self.surface == "recsel":
            self._record().update(state="ready", selecting=False, target={"kind": "region", "rect": rect})
            self.surface = "bar"
        else:
            op = self.operations[self.active]
            op["state"] = "editing" if op["action"] == "edit" else "completed"
            self.last_size = rect[2:]
            op["items"] = [{"local": {"status": "saved", "path": "/tmp/synthetic.png"}, "uploadStatus": "not_requested"}]
            self.surface = "editor" if op["action"] == "edit" else "history"

    def keys(self, *sequence):
        self.inputs.extend(("key", key) for key in sequence)
        for key in sequence:
            if self.surface == "format":
                if key == "Esc":
                    self.surface = "bar"
                elif key == "Home":
                    self.combo_index = 0
                elif key == "Down":
                    self.combo_index += 1
                elif key == "Return":
                    fmt, codec = [("mp4", "h264"), ("mp4", "h265"), ("webm", "vp9"), ("gif", ""), ("webp", "")][self.combo_index]
                    self._record()["config"].update(format=fmt, codec=codec, audio={"system": self.audio_intent and fmt not in ("gif", "webp")})
                    self.surface = "bar"
                continue
            if self.surface == "bar":
                if key == "Esc":
                    if self.question:
                        self.question = False
                    elif self._record()["state"] == "ready":
                        self._record()["state"] = "cancelled"
                        self.operations[self.active]["state"] = "cancelled"
                        self.surface = "history"
                continue
            assert self.surface in ("region", "recsel")
            if key == "Esc":
                self._cancel()
            elif key == "Return":
                self._accept()
            elif key in ("Tab", "Shift+Tab"):
                previous = self.env["screens"][self.head]
                self.head = 1 - self.head
                current = self.env["screens"][self.head]
                if self.selection:
                    self.selection[0] += current[0] - previous[0]
            elif not self.freeze_selection:
                if self.selection is None:
                    self.selection = [320, 200, 640, 400]
                    continue
                tokens = key.split("+")
                distance = 10 if "Ctrl" in tokens else 1
                index = 2 if "Shift" in tokens else 0
                direction = tokens[-1]
                axis = index + (direction in ("Down", "Up"))
                self.selection[axis] += distance * (1 if direction in ("Right", "Down") else -1)

    def shell(self, script, timeout=60):
        if "--motion" in script:
            return {"returncode": 0, "stdout": f"motion={self.motion}\n"}
        if "struct.unpack" in script:
            return {"returncode": 0, "stdout": " ".join(str(int(v)) for v in self.last_size)}
        assert script in ("sleep 2", "sleep 2.2")
        return {"returncode": 0, "stdout": ""}

    def scroll(self, x, y, clicks, head=None):
        self.inputs.append(("scroll", x, y, clicks))


class SelectorRecipes(unittest.TestCase):
    def recipes(self):
        return [r for r in REGISTRY if r.function.__module__ == selectors.__name__]

    def test_exact_owned_ids_once(self):
        owners = json.loads((ROOT / "tools/lab/surface_driver/ownership.json").read_text())["owners"]
        ids = [i for r in self.recipes() for i in r.interactions]
        self.assertEqual(set(ids), {i for i, owner in owners.items() if owner == "selectors"})
        self.assertEqual(len(ids), len(set(ids)))

    def test_desktop_differences_live_in_fragments(self):
        model = json.loads((ROOT / "tools/lab/surface_map/model.json").read_text())
        for desktop in model["desktops"]:
            path = ROOT / "tools/lab/surface_driver/profiles" / (desktop["id"] + ".d") / "selectors.json"
            value = json.loads(path.read_text())
            self.assertEqual(value["selectors"], {"bar_drag": desktop["id"].endswith("-x11")})
            self.assertEqual(set(value["window_titles"]), set(value["catalog_windows"]))
            self.assertEqual(value["catalog_windows"]["bar-ready"], ["33-record-ready-bar"])
            self.assertEqual(value["catalog_windows"]["bar-paused"], ["35-record-paused-bar"])

    def test_wayland_drag_is_explicitly_blocked(self):
        ctx = FakeContext()
        ctx.profile = {"selectors": {"bar_drag": False}}
        with self.assertRaisesRegex(RecipeBlocked, "Compositor places"):
            selectors.bar_drag(ctx)
        self.assertFalse(ctx.inputs)

    def test_every_recipe_with_contract_fixture(self):
        for item in self.recipes():
            with self.subTest(recipe=item.name):
                heads = 2 if item.function in (selectors.region_heads, selectors.recording_keyboard) else 1
                ctx = FakeContext(heads)
                if item.function in (selectors.region_upload, selectors.bar_hidden):
                    with self.assertRaises(RecipeBlocked):
                        item.function(ctx)
                else:
                    item.function(ctx)
                    self.assertTrue(ctx.frames)
                    self.assertTrue(ctx.inputs)
                self.assertEqual(ctx.document, ctx.original)
                forbidden = {"recording.start", "recording.pause", "recording.resume", "recording.stop", "recording.select", "recording.overlay", "action.invoke", "editor.open"}
                self.assertFalse(forbidden & {m for m, _ in ctx.calls})

    def test_all_references_exist_in_catalog_source(self):
        source = (ROOT / "tests/gui/ui_catalog.cpp").read_text()
        for item in self.recipes():
            ctx = FakeContext(2 if item.function in (selectors.region_heads, selectors.recording_keyboard) else 1)
            try:
                item.function(ctx)
            except RecipeBlocked:
                pass
            for _, _, reference in ctx.frames:
                if reference:
                    self.assertIn('"' + reference + '"', source)

    def test_stale_selection_cannot_pass_and_configuration_is_restored(self):
        ctx = FakeContext()
        ctx.freeze_selection = True
        with self.assertRaisesRegex(RecipeFailed, "selector rectangle"):
            selectors.region_keyboard(ctx)
        self.assertEqual(ctx.document, ctx.original)
        self.assertTrue(any(o["error"]["code"] == "cancelled" for o in ctx.operations.values()))

    def test_frozen_recording_fails_motion_gate(self):
        ctx = FakeContext()
        ctx.motion = 0.0
        with self.assertRaisesRegex(RecipeFailed, "frozen desktop"):
            selectors.bar_recording(ctx)
        self.assertEqual(ctx.document, ctx.original)
        self.assertTrue(all(r["state"] == "cancelled" for r in ctx.recordings.values()))

    def test_wrong_drag_bounds_retain_frame_and_retire_session(self):
        ctx = FakeContext()
        real_drag = ctx.drag
        def shifted_drag(start, end):
            real_drag(start, end)
            ctx._record()["target"]["rect"][0] += 1
        ctx.drag = shifted_drag
        with self.assertRaisesRegex(RecipeFailed, "chosen recording bounds"):
            selectors.bar_recording(ctx)
        self.assertIn("chosen-recording-area", [f[0] for f in ctx.frames])
        self.assertTrue(all(r["state"] == "cancelled" for r in ctx.recordings.values()))

    def test_shortcut_only_record_route_and_folder_overrides_are_restored(self):
        ctx = FakeContext()
        ctx.document["actions"]["record"] = {"shortcut": ""}
        ctx.document["archive"]["folders"]["images"] = "/tmp/original-images"
        ctx.original = copy.deepcopy(ctx.document)
        selectors.recording_output(ctx)
        self.assertEqual(ctx.document, ctx.original)

    def test_missing_optional_mode_is_seeded_as_equivalent_default(self):
        ctx = FakeContext()
        del ctx.document["preferences"]["regionSelectionMode"]
        ctx.original = copy.deepcopy(ctx.document)
        selectors.region_keyboard(ctx)
        ctx.original["preferences"]["regionSelectionMode"] = "live"
        self.assertEqual(ctx.document, ctx.original)

    def test_two_heads_are_required(self):
        with self.assertRaisesRegex(RecipeBlocked, "two independently sized"):
            selectors.region_heads(FakeContext())

    def test_missing_accessibility_is_blocked(self):
        ctx = FakeContext()
        ctx.no_accessibility = True
        with self.assertRaises(RecipeBlocked):
            selectors._description(ctx)

    def test_missing_selector_node_is_blocked_with_failure_frame_and_cleanup(self):
        ctx = FakeContext()
        ctx.no_accessibility = True
        with self.assertRaisesRegex(RecipeBlocked, "no live AT-SPI node"):
            selectors.region_keyboard(ctx)
        self.assertEqual(ctx.surface, "history")
        self.assertEqual(ctx.document, ctx.original)
        self.assertIn("selector-readback-unavailable", [f[0] for f in ctx.frames])

    def test_missing_description_cleans_up_open_selector(self):
        ctx = FakeContext()
        real_accessible = ctx.accessible
        def missing_description(query):
            return [{k:v for k,v in row.items() if k != "description"} for row in real_accessible(query)]
        ctx.accessible = missing_description
        with self.assertRaises(RecipeBlocked):
            selectors.region_keyboard(ctx)
        self.assertEqual(ctx.surface, "history")
        self.assertEqual(ctx.document, ctx.original)

    def test_bar_cancel_failure_retires_session_before_next_recipe(self):
        ctx = FakeContext()
        real_click = ctx.click
        def unavailable_control(control, **kwargs):
            if control == "cancel":
                raise RecipeBlocked("catalog aspect mismatch")
            return real_click(control, **kwargs)
        ctx.click = unavailable_control
        with self.assertRaisesRegex(RecipeBlocked, "catalog aspect"):
            selectors.bar_discard(ctx)
        self.assertTrue(all(r["state"] == "cancelled" for r in ctx.recordings.values()))

    def test_profile_metadata_cannot_replace_core_fixture(self):
        ctx = FakeContext()
        ctx.profile = {"selectors": {"fixture": {"screens": [[0, 0, 999, 999]]}}}
        self.assertIs(selectors._env(ctx), ctx.env)

    def test_completed_capture_and_cancel_do_not_need_accessibility(self):
        for function in (selectors.region_capture, selectors.region_cancel, selectors.recording_selection):
            with self.subTest(recipe=function.__name__):
                ctx = FakeContext()
                ctx.no_accessibility = True
                function(ctx)
                self.assertEqual(ctx.document, ctx.original)
                self.assertTrue(ctx.inputs)

    def test_keyboard_movement_does_not_require_second_head(self):
        ctx = FakeContext()
        selectors.recording_keyboard(ctx)
        self.assertTrue(any(kind == "key" and key == "Ctrl+Shift+Right"
                            for kind, key, *rest in ctx.inputs))
        self.assertFalse(any(kind == "key" and key == "Tab" for kind, key, *rest in ctx.inputs))

    def test_tab_checks_both_screenshot_and_recording_outputs(self):
        ctx = FakeContext(2)
        ctx.no_accessibility = True
        selectors.region_heads(ctx)
        self.assertEqual(len(ctx.operations), 6)
        captures = [op for op in ctx.operations.values() if op["action"] == "region"]
        self.assertEqual(len(captures), 3)
        self.assertEqual([r["target"]["rect"] for r in ctx.recordings.values()],
                         [ctx.env["screens"][1], ctx.env["screens"][0], ctx.env["screens"][1]])

    def test_unsaved_export_cannot_pass_motion_gate(self):
        ctx = FakeContext()
        real_call = ctx.call
        def failed_export(method, params=None):
            value = real_call(method, params)
            if method == "recording.saveCopy":
                ctx._record()["lastExport"]["status"] = "failed"
            return value
        ctx.call = failed_export
        with self.assertRaisesRegex(RecipeFailed, "motion probe export succeeded"):
            selectors.bar_recording(ctx)
        self.assertTrue(all(r["state"] == "cancelled" for r in ctx.recordings.values()))

    def test_refused_selector_focus_cleans_up_without_gestures(self):
        for function in (selectors.region_capture, selectors.recording_selection):
            with self.subTest(recipe=function.__name__):
                ctx = FakeContext()
                def refused(*args, **kwargs):
                    raise RecipeBlocked("live window match is missing or ambiguous")
                ctx.focus = refused
                with self.assertRaisesRegex(RecipeBlocked, "#701"):
                    function(ctx)
                self.assertFalse(ctx.inputs)
                self.assertTrue(all(op["state"] == "cancelled" for op in ctx.operations.values()))
                self.assertEqual(ctx.document, ctx.original)

    def test_unavailable_fixture_is_blocked(self):
        ctx = FakeContext()
        ctx.env = {}
        with self.assertRaisesRegex(RecipeBlocked, "screen rectangles"):
            selectors.region_keyboard(ctx)


if __name__ == "__main__":
    unittest.main()
