"""Surface driver contract: recipes, the context they drive, and receipt rows.

This module is the interface every recipe lane codes against. The driver core
(runner, input backends, catalog comparison) implements `Context`; recipe lanes
only import from here. Change this file only through the driver-core lane.

A recipe exercises one or more `model.json` interactions with real input and
asserts the effect through a resident inspect/view-state method:

    from tools.lab.surface_driver.api import recipe

    @recipe('history.copy-link', needs=('upload',), destinations=('local', 's3', 'proton'))
    def copy_link(ctx):
        ctx.view('ui.history', {'select': ctx.fixture('upload').history_id})
        ctx.frame('before')
        ctx.click('historyCopyLinkButton', window='history')
        ctx.expect(ctx.clipboard_text() == ctx.fixture('upload').link, 'clipboard holds the link')
        ctx.frame('after', reference='01-history-list')

Recipes never fork per desktop. Per-desktop differences (tolerances, input
backend, notification server, browser, known gaps) live in
`profiles/<desktop-id>.json` and fragments `profiles/<desktop-id>.d/<lane>.json`,
read through `ctx.profile`.
"""
from __future__ import annotations

import dataclasses
from dataclasses import dataclass, field
from typing import Any, Callable, Iterable, Optional

SCHEMA_VERSION = 2

FUNCTIONAL = ('passed', 'failed', 'blocked')
VISUAL = ('frames', 'reviewed-ok', 'reviewed-wrong')
PACKAGES = ('deb', 'rpm', 'appimage', 'source')
PLACEMENTS = ('native', 'guest', 'boat')
DESTINATIONS = ('local', 's3', 'b2', 'proton')
HOSTS = ('m74', 'f44', 'd69', 'k72', 'aurora-amd', 'boat')


class RecipeFailed(AssertionError):
    """An expectation did not hold: functional=failed."""


class RecipeBlocked(RuntimeError):
    """The recipe cannot run here (missing session, lock, capability): functional=blocked."""

    def __init__(self, reason: str, issue: Optional[int] = None):
        super().__init__(reason)
        self.reason = reason
        self.issue = issue


@dataclass(frozen=True)
class Recipe:
    interactions: tuple[str, ...]
    function: Callable[['Context'], None]
    needs: tuple[str, ...] = ()            # fixture names (see Context.fixture)
    destinations: tuple[str, ...] = ('local',)
    desktop_drawn: bool = False            # notifications, portals, tray menus, browser pages: no catalog reference
    timeout: float = 120.0

    @property
    def name(self) -> str:
        return self.function.__module__.rsplit('.', 1)[-1] + '.' + self.function.__name__


REGISTRY: list[Recipe] = []


def recipe(*interactions: str, needs: Iterable[str] = (), destinations: Iterable[str] = ('local',),
           desktop_drawn: bool = False, timeout: float = 120.0):
    """Register a recipe for one or more interaction ids it proves."""
    def wrap(function):
        REGISTRY.append(Recipe(tuple(interactions), function, tuple(needs), tuple(destinations),
                               desktop_drawn, timeout))
        return function
    return wrap


@dataclass
class Frame:
    label: str            # 'before', 'after', or a recipe-chosen step name
    path: str             # PNG, relative to the run directory
    window: Optional[str] = None
    reference: Optional[str] = None       # catalog capture name, e.g. '01-history-list'
    distance: Optional[float] = None      # catalog comparison score, filled by the runner
    tolerance: Optional[float] = None


class Context:
    """What a recipe may do. Implemented by the driver core; faked in recipe unit tests."""

    desktop: str          # model.json desktop id
    host: str             # one of HOSTS
    placement: str        # one of PLACEMENTS
    package: str          # one of PACKAGES
    destination: str      # one of DESTINATIONS
    profile: dict         # merged lab profile for this desktop

    # Surfaces and state
    def view(self, method: str, params: Optional[dict] = None) -> dict:
        """Open a surface in a known state through a resident view command (ui.history, editor.view, ...)."""
        raise NotImplementedError

    def call(self, method: str, params: Optional[dict] = None) -> dict:
        """Call any resident method (inspect, view-state, operation.inspect, share.inspect, ...)."""
        raise NotImplementedError

    def geometry(self, window: str) -> dict:
        """Screen rectangle {x, y, width, height, scale} of a put.gd window by role ('history', 'editor', ...)."""
        raise NotImplementedError

    def wait(self, predicate: Callable[[], Any], what: str, timeout: float = 10.0, interval: float = 0.2) -> Any:
        """Poll until predicate() is truthy; raises RecipeFailed naming `what` on timeout."""
        raise NotImplementedError

    # Real input (QMP for guests, the desktop's VNC/input path for physical hosts)
    def use_catalog(self, window: str, reference: str) -> None:
        """Optionally select the catalog state used to resolve this window's controls."""
        raise NotImplementedError

    def click(self, control: str, window: str, button: str = 'left', offset: Optional[tuple] = None) -> None:
        """Click a control resolved from the current build's NAME.controls.json, mapped onto the live window."""
        raise NotImplementedError

    def click_at(self, x: int, y: int, button: str = 'left', head: Optional[int] = None) -> None:
        raise NotImplementedError

    def drag(self, start: tuple, end: tuple, button: str = 'left', steps: int = 12, head: Optional[int] = None) -> None:
        raise NotImplementedError

    def keys(self, *sequence: str) -> None:
        """Press Qt portable key names in order, e.g. keys('Ctrl+Z'), keys('Tab', 'Return')."""
        raise NotImplementedError

    def type_text(self, text: str) -> None:
        raise NotImplementedError

    def scroll(self, x: int, y: int, clicks: int, head: Optional[int] = None) -> None:
        raise NotImplementedError

    # Commands that are themselves the interaction (cli.*, mcp.*)
    def cli(self, *args: str, timeout: float = 60.0) -> dict:
        """Run `putgd ARGS` on the target; returns {returncode, stdout, stderr}."""
        raise NotImplementedError

    def mcp(self, tool: str, arguments: Optional[dict] = None) -> dict:
        raise NotImplementedError

    # Desktop-drawn content (no catalog reference)
    def accessible(self, query: dict) -> list[dict]:
        """Read-only AT-SPI query. Unknown fields fail closed; filters are ANDed.

        name/text/description/application/window: case-sensitive globs; role:
        lowercase AT-SPI role (spaces); pid: integer; objectName: exact exported
        accessible ID; owned: private XDG family; visible/showing/enabled: bool;
        states: {state: bool}; within: [x,y,width,height] (center containment).
        Missing exported object IDs never match by name.
        Results: name, role, text, description, states (bool map), rect
        [x,y,width,height] or null, bounds {x,y,width,height} or null, pid,
        application, window, owned, objectName, accelerator, attributes,
        visible/showing/enabled, protected, popup and ancestors, plus
        coordinate_space ('desktop' or 'window-relative'), coordinate_error and
        window_id (compositor identity) where placement applies. rect is in
        desktop coordinates. On X11 that is AT-SPI DESKTOP_COORDS. On Wayland,
        client toolkits report extents relative to their own window; a profile
        'compositor' witness (GNOME lab extension, KWin script) places them, and
        without one (or when placement is implausible) rect/bounds are null with
        coordinate_error, never a guess. Rows also include out-of-tree accessibles
        seen by the run's observer: showing QMenu popups and the focused widget
        (start_notifications starts it). role 'button' is normalized to
        'push button'. Password text is never read. RecipeBlocked when
        unavailable, including profile gaps.
        """
        raise NotImplementedError

    def windows(self, query: Optional[dict] = None) -> list[dict]:
        """Read-only visible window inventory, private family by default; [] means absent.

        X11 title is a regex; include_hidden includes minimized windows.
        Wayland uses accessible() filters. Window states
        include minimized/maximized on X11 and the AT-SPI state map on Wayland.
        """
        raise NotImplementedError

    def focus(self, window: str, raise_window: bool = True) -> dict:
        """Select and verify an input target; optionally raise it using desktop policy.

        Every input rechecks focus/stacking. Obstruction is RecipeFailed naming
        the window (including shell-drawn banners/panels); unavailable witnesses
        are RecipeBlocked. X11 lets a just-mapped target settle (#750). Owned
        override-redirect windows (bar, selector cover) are selected by a server
        hit-test and refuse keyboard input unless they hold X input focus.
        Wayland uses the profile compositor witness (pick/focus; raise through
        its explicit activation where supported) or a compositor_script.
        """
        raise NotImplementedError

    def double_click(self, control: str, window: str, button: str = 'left') -> None:
        """One double-click gesture with live geometry and a foreground check."""
        raise NotImplementedError

    def hover(self, x: int, y: int, head: Optional[int] = None) -> None:
        """Move the real pointer on the selected head, checking the target first."""
        raise NotImplementedError

    def held_keys(self, *keys: str):
        """Context manager holding guest QMP keys across drag/scroll; always releases.

        Physical held keys are blocked pending input-agent scoped cleanup.
        VNC-only AltGr chords are blocked before dispatch."""
        raise NotImplementedError

    def click_accessible(self, query: dict, button: str = 'left') -> dict:
        """Real pointer click on one live desktop node with explicit ownership.

        Requires pid/application/owned in query and a compositor hit-test.
        Never calls AT-SPI doAction or D-Bus Activate.
        """
        raise NotImplementedError

    def hover_accessible(self, query: dict) -> dict:
        """Move the real pointer over one owned desktop node, verifying its hit-test."""
        raise NotImplementedError

    def notification_action(self, identity: int, action: str = 'default') -> None:
        """Real input to a drawn notification, correlated to ActionInvoked by ID.

        Waits briefly (profile notifications.draw_seconds) for the banner to be
        drawn, hovers it, then clicks the title (default) or the action button,
        each with a fresh compositor stage pick.
        """
        raise NotImplementedError

    def tray(self) -> list[dict]:
        """Read-only private-family SNI identities, tooltips and icon-pixmap hashes.

        Rows include pid/start/owner/service/path/id/title/status/icon_name/icon_pixmaps/
        tooltip/menu. Menu activation still requires real desktop input.
        start is the Linux process start tick; owner is the pinned unique bus
        name. Both are rechecked with private-family membership after reading
        properties. KDE bus/path and GNOME bus@/path watcher formats are accepted;
        stale, invalid or changed-owner items are omitted.
        """
        raise NotImplementedError

    def notifications(self) -> list[dict]:
        """Notify observations since recipe start (not proof of a drawn popup).

        Rows contain summary/body/actions/id, sender/server unique bus owners,
        timeout, hints/resident, replaces_id, at, action_events [{action,at}],
        and closed (null or {reason,at}). IDs correlate with replies/signals;
        id is null until the server's reply is seen. Only put.gd Notify calls
        from the private family, their replies and signals for their IDs are
        recorded; other applications' traffic never reaches the log.
        """
        raise NotImplementedError

    def clipboard_text(self) -> str:
        raise NotImplementedError

    def shell(self, script: str, timeout: float = 60.0, cleanup: bool = False) -> dict:
        """Run a shell script on the target as the session user with the run's private XDG profile.

        Ordinary calls fail after the recipe deadline and are clipped to the time
        left. ``cleanup=True`` is for restoring what a recipe changed (default
        browser, theme, dconf): it runs even after the deadline, with its own
        timeout (30 s when none is given, at most 600 s), never the clipped
        remainder, and the recipe alarm and termination signals are deferred
        until it returns (critical.py). The
        target's safety checks still apply (desktop lock for physical targets,
        an owned overlay for guests).
        """
        raise NotImplementedError

    # Fixtures shared between recipes in one run (an upload, a recording, a history row)
    def fixture(self, name: str, **options: Any) -> Any:
        """Resolve a per-recipe owned descriptor (mapping and attribute access).

        Additive round-3 contracts, restricted to private profiles/synthetic
        loopback destinations. Finalizers restore faults before restoring the
        accepted configuration, and run outside the recipe deadline:

        * file-dialog-portal / file-dialog-fallback: fresh private family,
          available/reason/backend/environment, resident() authenticated
          PID/start and observe(dialog='outputFolder', field=None) returning
          the production chooser state unchanged. Native buses/display stay
          intact. Portal-to-fallback failures are not Failed chooser results;
          chooser-failure blocks on #671 until an isolated boundary exists.
          Missing fallback catalog references remain review-required.
        * destinations-uncached: the destinations contract after a private
          resident restart; credentials_cached=False. destinations also exposes
          reset_credentials() to retire its session credential cache without
          locking the desktop or changing any keyring alias/item. Credential
          material remains target-only in the owned private synthetic file.

        * settings-multi-profile / settings-destinations / settings-share-destination:
          profiles, profile_ids, share_profile_id, publication_modes and
          private_supported. S3 supports default/share; it cannot prove private
          Proton publication. No live provider is enabled.
        * settings-no-output: accepted inherited routes with no profile/local
          copy, recovery_actions; assertions/input remain the recipe's job.
        * settings-action-routes: add(action, profile_id, publication_mode),
          remove(action), observe(). Removes recording's optional overrides by
          full settings.save, preserving shortcuts.
        * settings-save-failure: restore()/retry() remove an actual directory
          write fault before UI Retry; observe() returns real footer text, retry
          accessible rows, settings draft state and visible nonsecret nodes;
          external_change(patch) uses the existing atomic revision guard. It
          never manufactures a successful save, conflict or footer error.
        * settings-observations: observe(), disclosure(id, window='settings')
          returns control, expanded and visible accessible text; recording_status()
          reads the live accessible shortcut-recording label (None if absent).
          It uses accessible text or description; a visible label without
          current text blocks on #862. Ambiguous exports block.
        * settings-focus-loss: focus()/deactivate()/hide()/restore(), process,
          role, observe(). Native minimize is X11-only; Wayland focus uses the
          existing compositor witness. settings-input-events provides
          overlap(first, second, modifiers), altgr(key), shifted_digit(digit)
          through Context.held_keys/keys, with their unchanged input guards.
        * settings-binding-{conflict,unavailable,pending,unsupported}: state,
          controlled, observe(), release() for an owned X11 conflict. Other
          states must actually exist in the native channel or block; desktop
          environment labels and diagnostics replies are never substituted.
          observe() reads ui.snapshot.diagnostics.shortcuts unchanged; X11
          conflicts use error='shortcut_conflict', not a fabricated state.
          In a disposable GNOME lab guest, unavailable disables only the
          already-enabled putgd@put.gd helper, then restores it and verifies
          native availability. The independent compositor witness stays enabled.
        * locked-keyring: unique blank-password synthetic collection,
          lock()/unlock(); no personal item/default alias is read or changed.
        * events.update-feed / settings-update-feed: url/root/residentPid,
          resident() (authenticated peer PID/start), channel/version/supported,
          publish(schema-1 feed), ledger() (sequence/method/route/channel/status/
          bytes and privacy booleans queryEmpty/authorizationAbsent/cookieAbsent/
          bodyEmpty/genericAgent plus unexpectedHeaderCount). No header values,
          queries or raw URLs are logged, including rejected HTTP methods.
          Feed, release and mirror overrides are literal loopback endpoints.
        * settings-update-lifecycle / events.update-lifecycle: updates.bundle
          in the profile must point to authentic tagged AppImage releases and
          signed checksum files (see updates.lifecycle). Returns image, build,
          releases, install(**params), restart(**params) plus feed contract.
          Production signature verification stays enabled. Missing bundles
          block before switching; development bytes are never relabelled.
        * recording-lifecycle: fresh synthetic scene, prepare parameters,
          prepare_recording(**overrides), recording_ids, output_dir, destination
          and capabilities. Callers exercise the production CLI/MCP lifecycle;
          native recording consent is supplied separately, never bypassed.
          events.recording-source preflights/stages the distro GTK3 GStreamer
          sink and ffmpeg/ffprobe in disposable guests, before scene/capture
          setup; an unavailable decoder prerequisite blocks precisely.
        * events.expired-multipart: real failed SDK operation/journal,
          operationId/itemId, listing()/requests() independent loopback S3
          evidence; automatic_cleanup() ages only that stopped owned journal
          and restarts once for the genuine 60-second startup cleanup. No
          manual cache.clear is counted as an automatic notice. Recipe timeout
          must cover fixture setup plus automatic_delay_seconds (60).
        * upload-incomplete now adds automatic=True and cleanup_result to its
          operationId/itemId descriptor: it waits for that genuine automatic
          cleanup, so the tray's native notification is observed since recipe
          start. events.incomplete-multipart retains the fresh-failure contract.
        * resident-lifecycle: initial/identity() (authenticated PID/start),
          stopped(), quit(), launch() after real Quit, restart(). A finalizer
          launches a quit private resident before settings restoration.
        * tray-qt-fallback: a real QApplication resident through the native
          -platform xcb/wayland override, preserving desktop/session identity;
          restores the original executable route. No desktop is impersonated.
        * tray-attention: genuine retained local-save failure and retry();
          tray-two-uploads: two independently identified loopback operations,
          inspect()/cancel(id); tray-active-recording: live synthetic recording
          id/inspect()/pause()/resume()/stop(); tray-output: the action-routes
          contract; tray-update-available: an actual checked signed-release
          bundle via settings-update-lifecycle, never a relabelled source build.
        * shortcut-other-menu: separate real popup process/role/observe()/focus().
          keyboard_grab_verified=False explicitly withholds any native grab
          claim; callers must obtain that witness from their input backend.
        * shortcut-settings-recorder: start(action), cancel(), status(),
          recording_status(), no_operation_baseline() through real controls and
          the actual native shortcut channel. shortcut-keyboard-layout(layout)
          changes/restores an owned X11 map exactly; Wayland blocks without its
          native input-source controller. No lock/unlock or ended-session
          lifecycle is inferred from these fixtures.
        """
        raise NotImplementedError

    # Evidence and verdicts
    def frame(self, label: str, window: Optional[str] = None, reference: Optional[str] = None) -> Frame:
        """Save a frame. With `reference`, the runner compares it with that catalog capture."""
        raise NotImplementedError

    def expect(self, condition: Any, what: str) -> None:
        if not condition:
            raise RecipeFailed(what)

    def note(self, text: str) -> None:
        raise NotImplementedError

    def review(self, reason: str) -> None:
        """Automation cannot decide this one: the row gets review=true with this reason."""
        raise NotImplementedError

    def blocked(self, reason: str, issue: Optional[int] = None) -> None:
        raise RecipeBlocked(reason, issue)


@dataclass
class ReceiptRow:
    """One coverage entry. surface_map.py import accepts these under {"interactions": [...]}."""
    interaction: str
    desktop: str
    package: str
    functional: str                       # FUNCTIONAL
    visual: Optional[str] = None          # VISUAL, None when visual does not apply
    receipt: Optional[str] = None         # path of the run's receipt.json, relative to the repo
    issue: Optional[int] = None
    note: Optional[str] = None
    host: Optional[str] = None            # HOSTS
    placement: Optional[str] = None       # PLACEMENTS; guest rows also set guest
    guest: Optional[str] = None           # golden/profile name for guest rows
    destination: Optional[str] = None     # DESTINATIONS
    review: bool = False
    review_reason: Optional[str] = None
    frames: list = field(default_factory=list)   # [{label, path, reference, distance, tolerance}]
    recipe: Optional[str] = None
    build: Optional[str] = None           # commit or version of the exact artifact
    at: Optional[str] = None

    def to_json(self) -> dict:
        data = dataclasses.asdict(self)
        return {k: v for k, v in data.items() if v not in (None, [], False) or k in ('functional',)}
