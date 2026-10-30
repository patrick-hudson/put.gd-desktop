"""Settings lane: real controls, resident observations, and private on-disk state.

No desktop names or coordinates belong here. Fixture requirements are declared
on the recipes; missing fixtures must produce blocked receipts, never passes.
"""
from __future__ import annotations

import json
import shlex

from tools.lab.surface_driver.api import recipe, RecipeBlocked
from .settings_input import (click as live_click, observe as input_witness,
                             prepare as prepare_input, ensure_accessible, collapse, field_click)

CORE_ISSUE = 681

REFERENCES = ('07-settings-general', '08-settings-capture',
              '06-settings-shortcuts', '05-settings-advanced')


def result(ctx, reply):
    """Accept resident results or the CLI/MCP JSON envelope, checking errors."""
    if 'returncode' in reply:
        ctx.expect(reply['returncode'] == 0, 'command exited successfully')
        reply = json.loads(reply['stdout'])
    if 'ok' in reply:
        ctx.expect(reply['ok'] is True, 'resident accepted request: ' +
                   str(reply.get('error', {}).get('code', '')))
        return reply['result']
    return reply


def call(ctx, method, params=None):
    return result(ctx, ctx.call(method, params or {}))


def get(ctx):
    return call(ctx, 'settings.get')


def at(document, path):
    for key in path.split('.'):
        document = document[key]
    return document


def value_is(document, path, value):
    try:
        return at(document, path) == value
    except KeyError:
        # Optional fields need not exist before the first accepted autosave.
        return False


# Read only a private profile, and check it matches the resident's actual path.
# Never fall back to the user's real configuration directory.
PRIVATE_CONFIG = '''import json, os, pathlib, stat
root = pathlib.Path(os.environ['XDG_CONFIG_HOME'])
p = root / 'putgd' / 'config.json'
assert root.is_absolute() and root.resolve() != (pathlib.Path.home()/'.config').resolve()
s = p.parent.lstat()
assert stat.S_ISDIR(s.st_mode) and s.st_uid == os.getuid() and not s.st_mode & 0o077
assert not p.is_symlink() and p.is_file()
print(json.dumps({'path': str(p), 'document': json.loads(p.read_text())}))
'''


def persisted(ctx):
    reply = ctx.shell('python3 -c ' + shlex.quote(PRIVATE_CONFIG))
    if reply['returncode'] != 0:
        ctx.blocked('Private XDG_CONFIG_HOME/putgd/config.json is required; refusing the normal user profile.')
    data = json.loads(reply['stdout'])
    ctx.expect(get(ctx)['metadata']['path'] == data['path'],
               'resident and shell use the same private configuration')
    return data['document']


def patch(ctx, values):
    metadata = get(ctx)['metadata']
    call(ctx, 'settings.patch', {'patch': values,
                               'expectedRevision': metadata['observedRevision']})


def state(ctx):
    return call(ctx, 'ui.viewState')


def opened(ctx, section, *, fixture='settings', keep_output=True):
    if ctx.profile.get('session') == 'wayland':
        # Prove native input is available before restarting a fixture family.
        # Missing compositor coordinates/focus remain blocked, never bypassed
        # with XWayland or application-local accessibility actions.
        persisted(ctx)
        result(ctx, ctx.view('ui.show', {'view': 'settings'}))
        try:
            ctx.focus('settings')
        except RecipeBlocked as error:
            ctx.blocked('Native Wayland Settings input preflight: ' + error.reason, issue=750)
    ctx.fixture(fixture)
    persisted(ctx)  # fail closed before any mutation, including resident setup
    # The primary catalog has a working local output and the dark scheme.
    # Without local output, the resident adds a warning above the page and all
    # catalog control rectangles move. Seed only this private run's preferences.
    document = get(ctx)['document']
    baseline = {}
    if keep_output and not document['archive']['keepUploads']:
        baseline['archive'] = {'keepUploads': True}
    if document['preferences'].get('appearance', {}).get('theme', 'system') != 'dark':
        baseline['preferences'] = {'appearance': {'theme': 'dark'}}
    if baseline:
        patch(ctx, baseline)
    result(ctx, ctx.view('ui.show', {'view': 'settings'}))
    # A blocked diagnostics recipe can leave its modal dialog open. Restore
    # this private lane's known page before sending navigation or field input.
    diagnostics = call(ctx, 'ui.diagnostics', {'action': 'inspect'})
    if diagnostics['package'].get('open'):
        ctx.wait(lambda: not package(ctx).get('saving'), 'previous package save settled', timeout=90)
        call(ctx, 'ui.diagnostics', {'action': 'closePackage'})
        ctx.wait(lambda: not package(ctx)['open'], 'previous package dialog closed')
    ctx.wait(lambda: state(ctx).get('appearance', {}).get('theme') == 'dark',
             'catalog appearance applied to the UI')
    # Profile preparation also enables the live control probe. A first-launch
    # Qt 6.4 helper may need a cooperative reopen to pick that up.
    prepare_input(ctx)
    ensure_accessible(ctx)
    ctx.use_catalog('settings', REFERENCES[section])
    live_click(ctx, 'settingsSection' + str(section))
    ctx.wait(lambda: state(ctx).get('settings', {}).get('section') == section,
             'Settings section ' + str(section))


def frame(ctx, label, section):
    ctx.frame(label, window='settings', reference=REFERENCES[section])


def saved(ctx, path, value, section):
    ctx.wait(lambda: value_is(get(ctx)['document'], path, value),
             path + ' accepted by resident')
    ctx.wait(lambda: value_is(persisted(ctx), path, value), path + ' persisted')
    ctx.expect(state(ctx)['settings']['section'] == section, 'expected Settings page remains active')


def scroll_click(ctx, control, window='settings', *, after_window=None):
    """Search clipped controls with real wheel input; core verifies every hit."""
    if window == 'settings':
        return live_click(ctx, control, after_window=after_window)
    for attempt in range(16):
        try:
            return ctx.click(control, window=window)
        except RecipeBlocked as error:
            if '0 matches' not in error.reason:
                raise
            geometry = ctx.geometry(window)
            ctx.focus(window)
            ctx.scroll(geometry['x'] + geometry['width'] - 24,
                       geometry['y'] + geometry['height'] // 2,
                       40 if attempt == 0 else -3)
    ctx.blocked('Live Settings control remains unavailable after scrolling: ' + control, issue=CORE_ISSUE)


def choose(ctx, control, index):
    live_click(ctx, control)
    # Open the popup so Home does not edit an editable choice's line edit.
    ctx.keys('Esc', 'Alt+Down', 'Home', *('Down',) * index, 'Return', 'Tab')
    ctx.frame('keys-after-' + control)


def clipboard_text(ctx):
    # Native clipboard writers acquire ownership asynchronously. A missing
    # owner is not a content match; let the caller's bounded wait retry it.
    try:
        return ctx.clipboard_text()
    except RecipeBlocked as error:
        if not error.reason.startswith('clipboard text unavailable'):
            raise
        return ''


def replace(ctx, control, text):
    paste = control.endswith('Template')
    if paste:
        writer = ctx.profile.get('settings', {}).get('clipboard_write_command')
        if not writer:
            ctx.blocked('Template input requires the profile synthetic clipboard writer.')
        reply = ctx.shell('printf %s ' + shlex.quote(text) + ' | ' + writer + ' >/dev/null 2>&1')
        if reply['returncode'] != 0:
            ctx.blocked('Profile synthetic clipboard writer unavailable; template input withheld.')
        ctx.wait(lambda: clipboard_text(ctx) == text, 'synthetic template clipboard is ready')
    field_click(ctx, control)
    # Clipboard ownership changes can clear the X11 text selection. Prepare
    # the clipboard first, then select and paste without an intervening probe.
    ctx.keys('Ctrl+A')
    if paste:
        ctx.keys('Ctrl+V')
    else:
        ctx.type_text(text)
    ctx.keys('Tab')
    if control in ctx.profile.get('settings', {}).get('live_controls', {}) or paste or ctx.profile.get('session') == 'wayland':
        if ctx.profile.get('settings', {}).get('input_state_script'):
            input_witness(ctx, ctx.geometry('settings'))
        ctx.frame('keys-after-' + control, window='settings')


def checkbox(identifier, control, path, section, default):
    @recipe(identifier)
    def toggle(ctx):
        opened(ctx, section)
        # Optional preferences can be absent. Use their documented effective
        # value, preserving the current setting rather than racing a setup patch.
        initial = get(ctx)['document']
        for key in path.split('.'):
            initial = initial.get(key, default) if isinstance(initial, dict) else default
        frame(ctx, 'before', section)
        try:
            live_click(ctx, control)
            saved(ctx, path, not initial, section)
            if identifier == 'settings.launch-at-login':
                ctx.wait(lambda: call(ctx, 'ui.snapshot')['diagnostics'].get('loginStartup', {}).get('enabled') == (not initial)
                         and call(ctx, 'ui.snapshot')['diagnostics'].get('loginStartup', {}).get('applied') is True,
                         'login startup integration applied')
            if identifier == 'advanced.debug-logging' and not initial:
                ctx.wait(lambda: call(ctx, 'ui.diagnostics', {'action': 'inspect'})['debugLogging'].get('expiresAt'),
                         '24-hour debug logging expiry reported')
            frame(ctx, 'after', section)
        finally:
            values = leaf = {}
            parts = path.split('.')
            for key in parts[:-1]:
                leaf[key] = {}
                leaf = leaf[key]
            leaf[parts[-1]] = initial
            patch(ctx, values)
            saved(ctx, path, initial, section)
    toggle.__name__ = identifier.replace('-', '_').replace('.', '_')
    return toggle


checkbox('settings.launch-at-login', 'launchAtLogin', 'preferences.launchAtLogin', 0, False)
checkbox('settings.unlock-at-startup', 'unlockCredentialsAtStartup',
         'preferences.unlockCredentialsAtStartup', 0, True)
checkbox('settings.notifications', 'notifications', 'preferences.notifications', 0, True)
checkbox('capture.keep-copies', 'keepUploadCopies', 'archive.keepUploads', 1, True)
checkbox('advanced.debug-logging', 'debugLogging', 'preferences.diagnostics.debugLogging', 3, False)


@recipe('settings.nav')
def navigation(ctx):
    opened(ctx, 0)
    for section in (1, 2, 3, 0):
        frame(ctx, 'before-' + str(section), state(ctx)['settings']['section'])
        live_click(ctx, 'settingsSection' + str(section))
        ctx.wait(lambda: state(ctx)['settings']['section'] == section, 'rail changes section')
        frame(ctx, 'after-' + str(section), section)
    persisted(ctx)


@recipe('settings.theme')
def appearance(ctx):
    opened(ctx, 0)
    original = get(ctx)['document']['preferences'].get('appearance', {}).get('theme', 'system')
    frame(ctx, 'before', 0)
    try:
        for index, theme in ((1, 'dark'), (2, 'light'), (0, 'system')):
            choose(ctx, 'appearanceTheme', index)
            saved(ctx, 'preferences.appearance.theme', theme, 0)
            ctx.wait(lambda: state(ctx)['appearance']['theme'] == theme, 'appearance preference is live')
            actual = state(ctx)['appearance']
            if theme != 'system':
                ctx.expect(actual['scheme'] == theme, 'explicit scheme applied')
            elif actual.get('desktop'):
                ctx.expect(actual['scheme'] == actual['desktop'], 'system follows observed desktop')
            reference = ctx.profile.get('settings', {}).get('theme_references', {}).get(actual['scheme'])
            if not reference:
                ctx.blocked('No general-page catalog for observed scheme ' + actual['scheme'])
            ctx.frame('after-' + theme, window='settings', reference=reference)
    finally:
        patch(ctx, {'preferences': {'appearance': {'theme': original}}})


def combo(identifier, control, path, section, values):
    @recipe(identifier)
    def change(ctx):
        opened(ctx, section)
        frame(ctx, 'before', section)
        for index, value in values:
            choose(ctx, control, index)
            saved(ctx, path, value, section)
            frame(ctx, 'after-' + str(index), section)
    change.__name__ = identifier.replace('-', '_').replace('.', '_')
    return change


combo('capture.after-upload', 'uploadClipboardBehavior', 'preferences.uploadClipboardBehavior',
      1, ((1, 'list'), (0, 'each_link')))
combo('capture.region-mode', 'regionSelectionMode', 'preferences.regionSelectionMode',
      1, ((1, 'screenshot_first'), (0, 'live')))
combo('advanced.history-retention', 'historyRetentionChoice', 'retention.historyRetentionDays',
      3, ((1, 30), (2, 90), (3, 365), (0, None)))
combo('advanced.failed-artifact', 'failedArtifactChoice', 'retention.failedArtifactDays',
      3, ((0, 0), (1, 1), (4, 14), (3, 7)))


@recipe('advanced.recent-limit')
def recent_limit(ctx):
    opened(ctx, 3)
    frame(ctx, 'before', 3)
    for value in (37, 100):
        replace(ctx, 'recentActivityLimit', str(value))
        saved(ctx, 'retention.recentActivityLimit', value, 3)
    frame(ctx, 'after', 3)


@recipe('capture.archive-root')
def archive_root(ctx):
    opened(ctx, 1)
    original = get(ctx)['document']['archive']['root']
    frame(ctx, 'before', 1)
    try:
        replace(ctx, 'archiveRoot', original.rstrip('/') + '/surface-settings')
        saved(ctx, 'archive.root', original.rstrip('/') + '/surface-settings', 1)
        frame(ctx, 'after', 1)
    finally:
        replace(ctx, 'archiveRoot', original)
        saved(ctx, 'archive.root', original, 1)


@recipe('capture.category-folders', timeout=240)
def category_folders(ctx):
    opened(ctx, 1)
    original = get(ctx)['document']['archive']['folders']
    root = get(ctx)['document']['archive']['root']
    frame(ctx, 'before', 1)
    # Empty overrides ensure the disclosure starts closed when the page opens.
    patch(ctx, {'archive': {'folders': {k: None for k in ('images', 'videos', 'text', 'other')}}})
    live_click(ctx, 'categoryFoldersToggle')
    ctx.use_catalog('settings', 'settings-category-folders')
    try:
        for category in ('images', 'videos', 'text', 'other'):
            target = root.rstrip('/') + '/surface-' + category
            replace(ctx, 'folder' + category.title(), target)
            saved(ctx, 'archive.folders.' + category, target, 1)
        frame(ctx, 'after', 1)
    finally:
        patch(ctx, {'archive': {'folders': original}})
        collapse(ctx, 'categoryFoldersToggle')


@recipe('capture.naming', timeout=240)
def naming(ctx):
    opened(ctx, 1)
    # Both this disclosure and the template editor contain a Clipboard text
    # field. Restore the intended page state before resolving that live name.
    collapse(ctx, 'categoryFoldersToggle')
    original = get(ctx)['document']['naming']
    frame(ctx, 'before', 1)
    try:
        for index, mode in ((1, 'generated'), (2, 'template'), (0, 'original')):
            choose(ctx, 'namingMode', index)
            saved(ctx, 'naming.mode', mode, 1)
        for index, policy in ((1, 'skip'), (2, 'overwrite'), (0, 'rename')):
            choose(ctx, 'collisionPolicy', index)
            saved(ctx, 'naming.collisionPolicy', policy, 1)
        choose(ctx, 'namingMode', 2)
        for key in ('fileTemplate', 'captureTemplate', 'imageTemplate', 'textTemplate'):
            value = 'surface-{yyyy}-{MM}-{dd}-{name}.{ext}' if key == 'fileTemplate' else 'surface-{yyyy}-{MM}-{dd}-{HH}-{mm}-{ss}.{ext}'
            replace(ctx, key, value)
            saved(ctx, 'naming.' + key, value, 1)
        frame(ctx, 'after', 1)
    finally:
        patch(ctx, {'naming': original})


def shortcut_status(ctx):
    diagnostics = call(ctx, 'ui.snapshot')['diagnostics']
    return diagnostics.get('shortcuts', diagnostics.get('capabilities', {}).get('source', {}).get('shortcuts', {}))


def recording(ctx, action):
    return shortcut_status(ctx).get('bindings', {}).get(action, {}).get('state') == 'recording'


def binding(ctx, action, chord):
    status = shortcut_status(ctx)
    if not status.get('available'):
        ctx.blocked('Desktop reports no global shortcut channel; binding effect cannot be asserted.')
    observed = None
    def matches():
        nonlocal observed
        entry = shortcut_status(ctx).get('bindings', {}).get(action, {})
        current = {key: entry.get(key) for key in ('state', 'bound', 'requested', 'effective', 'actual', 'error')}
        if current != observed:
            ctx.note('native shortcut binding ' + action + ': ' + json.dumps(current, sort_keys=True))
            observed = current
        actual = entry.get('actual')
        # KGlobalAccel confirms the layout-normalized sequence (for example a
        # shifted digit), while requested retains the person's saved chord.
        effective = entry.get('effective', chord)
        return (entry.get('requested', chord) == chord and bool(effective) and
                entry.get('bound') is True and not entry.get('error') and
                actual in (effective, [effective]))
    ctx.wait(matches, action + ' binding confirmed by desktop')


def shortcut_campaign(ctx, check):
    opened(ctx, 2)
    original = get(ctx)['document']['actions']
    frame(ctx, 'before', 2)
    try:
        for action in ('region', 'window', 'desktop', 'clipboard', 'edit', 'record'):
            control = 'record' + action.title() + 'Shortcut'
            chord = 'Ctrl+Alt+Shift+9'
            if check == 'record':
                # Preserve the Escape -> save -> second-click sequence that
                # exposed #731, independently of the other shortcut verdicts.
                live_click(ctx, control)
                ctx.wait(lambda: recording(ctx, action), 'recorder ready before Escape')
                ctx.keys('Esc')
                ctx.wait(lambda: not recording(ctx, action), 'Escape ends shortcut recording')
                saved(ctx, 'actions.' + action + '.shortcut', original[action]['shortcut'], 2)
            live_click(ctx, control)
            ctx.wait(lambda: recording(ctx, action), 'shortcut recorder pauses the desktop channel')
            if check == 'escape':
                ctx.keys('Esc')
                ctx.wait(lambda: not recording(ctx, action), 'Escape ends shortcut recording')
                saved(ctx, 'actions.' + action + '.shortcut', original[action]['shortcut'], 2)
            else:
                ctx.keys(chord)
                saved(ctx, 'actions.' + action + '.shortcut', chord, 2)
                binding(ctx, action, chord)
                if check == 'clear':
                    live_click(ctx, 'clear' + action.title() + 'Shortcut')
                    saved(ctx, 'actions.' + action + '.shortcut', '', 2)
                elif check in ('record', 'backspace'):
                    live_click(ctx, control)
                    ctx.wait(lambda: recording(ctx, action), 'recorder ready')
                    if check == 'record':
                        live_click(ctx, control)
                        ctx.wait(lambda: not recording(ctx, action), 'second keycap click cancels recording (app issue #731)')
                        saved(ctx, 'actions.' + action + '.shortcut', chord, 2)
                    else:
                        ctx.keys('Backspace')
                        ctx.wait(lambda: not recording(ctx, action), 'Backspace ends shortcut recording')
                        saved(ctx, 'actions.' + action + '.shortcut', '', 2)
            frame(ctx, 'after-' + action, 2)
            patch(ctx, {'actions': {action: original[action]}})
    finally:
        ctx.keys('Esc')
        patch(ctx, {'actions': original})


@recipe('shortcuts-page.field', timeout=360)
def shortcut_field(ctx):
    shortcut_campaign(ctx, 'field')


@recipe('shortcuts-page.record', timeout=360)
def shortcut_recording(ctx):
    shortcut_campaign(ctx, 'record')


@recipe('shortcuts-page.clear', timeout=360)
def shortcut_clear(ctx):
    shortcut_campaign(ctx, 'clear')


@recipe('shortcuts-page.key-esc', timeout=360)
def shortcut_escape(ctx):
    shortcut_campaign(ctx, 'escape')


@recipe('shortcuts-page.key-backspace', timeout=360)
def shortcut_backspace(ctx):
    shortcut_campaign(ctx, 'backspace')


@recipe('advanced.desktop-support')
def desktop_support(ctx):
    opened(ctx, 3)
    call(ctx, 'ui.diagnostics', {'action': 'setDesktopDetails', 'enabled': False})
    frame(ctx, 'before', 3)
    live_click(ctx, 'desktopSupportDetailsToggle')
    ctx.wait(lambda: call(ctx, 'ui.diagnostics', {'action': 'inspect'})['desktopSupport']['expanded'],
             'desktop support disclosure expands')
    ctx.use_catalog('settings', 'settings-desktop-support-expanded')
    scroll_click(ctx, 'refreshDesktopSupport')
    scroll_click(ctx, 'copyDesktopDetails')
    ctx.wait(lambda: 'capabilities' in clipboard_text(ctx), 'copied desktop diagnostics')
    ctx.expect(persisted(ctx) == get(ctx)['document'], 'support inspection preserves configuration')
    frame(ctx, 'after', 3)


def package(ctx):
    return call(ctx, 'ui.diagnostics', {'action': 'inspect'})['package']


def open_package(ctx):
    opened(ctx, 3)
    live_click(ctx, 'createDiagnosticsPackage', after_window='diagnostics')
    ctx.use_catalog('diagnostics', '58-diagnostics-package')
    ctx.wait(lambda: package(ctx).get('open') and not package(ctx).get('previewing'), 'diagnostics preview ready')
    ctx.expect(package(ctx)['includeNames'] is False, 'file names excluded on every new dialog')


def package_click(ctx, control):
    # Diagnostics is runtime-generated; select its actual visible labels. The
    # core still resolves the private live control and verifies foreground input.
    labels = {'includeFileNames': 'Include file names',
              'packageContentsToggle': 'Package contents',
              'uploadPackage': 'Also upload and copy a link'}
    if control == 'closePackage':
        label = 'Close' if package(ctx).get('saved') else 'Cancel'
    elif control == 'savePackage':
        label = 'Save and upload' if package(ctx).get('upload') else 'Save package'
    else:
        label = labels[control]
    ctx.click('text:' + label, window='diagnostics')


def package_frame(ctx, label):
    ctx.frame(label, window='diagnostics', reference='58-diagnostics-package')


@recipe('advanced.create-package', 'diagnostics.include-names', 'diagnostics.contents', 'diagnostics.close')
def package_options(ctx):
    opened(ctx, 3)
    frame(ctx, 'before', 3)
    live_click(ctx, 'createDiagnosticsPackage', after_window='diagnostics')
    ctx.use_catalog('diagnostics', '58-diagnostics-package')
    ctx.wait(lambda: package(ctx).get('open') and not package(ctx).get('previewing'), 'package opened by real click')
    package_frame(ctx, 'before-options')
    ctx.expect(package(ctx)['includeNames'] is False, 'names initially excluded')
    package_click(ctx, 'includeFileNames')
    ctx.wait(lambda: package(ctx)['includeNames'] and not package(ctx)['previewing'], 'names included in preview')
    package_click(ctx, 'packageContentsToggle')
    ctx.expect(bool(package(ctx)['contents']) and bool(package(ctx)['redaction']), 'preview lists package contents and redaction')
    package_frame(ctx, 'after-options')
    package_click(ctx, 'closePackage')
    ctx.wait(lambda: not package(ctx)['open'], 'package dialog closes')
    # Opening again must reset the privacy choice.
    result(ctx, ctx.view('ui.diagnostics', {'action': 'openPackage'}))
    ctx.expect(package(ctx)['includeNames'] is False, 'reopening resets file-name consent')
    package_click(ctx, 'closePackage')
    persisted(ctx)


@recipe('diagnostics.save')
def package_save(ctx):
    open_package(ctx)
    ctx.wait(lambda: not package(ctx)['uploadTarget']['checking'], 'upload-target check finishes', timeout=90)
    if package(ctx)['upload']:
        package_click(ctx, 'uploadPackage')
    package_frame(ctx, 'before')
    package_click(ctx, 'savePackage')
    saved_package = ctx.wait(lambda: package(ctx).get('saved'), 'package saved locally')
    ctx.expect(not saved_package.get('uploadError'), 'local save has no upload error')
    path = saved_package['path']
    verify = "import pathlib,zipfile; p=pathlib.Path(" + repr(path) + "); assert p.is_file(); z=zipfile.ZipFile(p); assert z.testzip() is None; assert z.namelist()"
    response = ctx.shell('python3 -c ' + shlex.quote(verify))
    ctx.expect(response['returncode'] == 0, 'saved package is a readable nonempty ZIP')
    ctx.expect(persisted(ctx) == get(ctx)['document'], 'package save preserves configuration')
    package_frame(ctx, 'after')
    package_click(ctx, 'closePackage')


@recipe('diagnostics.upload', needs=('diagnostics-upload',), destinations=('local', 's3'))
def package_upload(ctx):
    """Fixture: isolated working S3 default, synthetic inputs; no live creds on Boat."""
    if ctx.host == 'boat' and ctx.destination != 'local':
        ctx.blocked('Boat diagnostics uploads require the loopback S3 fixture; live S3 is forbidden.')
    fixture = ctx.fixture('diagnostics-upload')
    ctx.expect(fixture['isolated'] is True, 'isolated synthetic diagnostics destination')
    if ctx.destination == 'local':
        ctx.expect(fixture['loopback'] is True, 'local destination is the loopback S3 fixture')
    open_package(ctx)
    target = ctx.wait(lambda: package(ctx)['uploadTarget'] if not package(ctx)['uploadTarget']['checking'] else None,
                      'diagnostics upload target ready', timeout=90)
    if not target.get('available'):
        ctx.blocked('Diagnostics fixture target unavailable: ' + str(target.get('reason')))
    ctx.expect(target['profileId'] == fixture['profileId'], 'only isolated destination selected')
    package_frame(ctx, 'before')
    # Exercise the opt-in checkbox with real input, then send through Save.
    if package(ctx)['upload']:
        package_click(ctx, 'uploadPackage')
    ctx.expect(package(ctx)['upload'] is False, 'upload checkbox can opt out')
    package_click(ctx, 'uploadPackage')
    ctx.expect(package(ctx)['upload'] is True, 'explicit upload enabled')
    package_click(ctx, 'savePackage')
    outcome = ctx.wait(lambda: package(ctx).get('saved') if not package(ctx).get('saving') else None,
                       'diagnostics saved and uploaded', timeout=90)
    ctx.expect(not outcome.get('uploadError'), 'diagnostics upload succeeded')
    ctx.expect(bool(outcome.get('link')) and outcome.get('linkCopied') is True, 'upload returned and copied link')
    ctx.expect(clipboard_text(ctx) == outcome['link'], 'clipboard contains exact returned link')
    ctx.expect(persisted(ctx) == get(ctx)['document'], 'diagnostics upload preserves configuration')
    package_frame(ctx, 'after')
    package_click(ctx, 'closePackage')


@recipe('cli.settings')
def cli_settings(ctx):
    opened(ctx, 0)
    frame(ctx, 'before', 0)
    current = result(ctx, ctx.cli('settings.get'))
    ctx.expect(current['document'] == persisted(ctx), 'CLI reads persisted configuration')
    result(ctx, ctx.cli('settings.validate', json.dumps({'document': current['document']})))
    expected = not current['document']['preferences']['notifications']
    try:
        result(ctx, ctx.cli('settings.patch', json.dumps({'patch': {'preferences': {'notifications': expected}},
                           'expectedRevision': current['metadata']['observedRevision']})))
        saved(ctx, 'preferences.notifications', expected, 0)
        result(ctx, ctx.cli('settings.reload'))
        frame(ctx, 'after', 0)
    finally:
        patch(ctx, {'preferences': {'notifications': not expected}})
    ctx.blocked('Partial CLI settings evidence saved; setup.* needs an isolated setup lifecycle fixture. The multi-profile fixture is available, but this CLI recipe still needs explicit save/reorder coverage.', issue=CORE_ISSUE)


@recipe('cli.ui')
def cli_ui(ctx):
    opened(ctx, 0)
    frame(ctx, 'before', 0)
    for view in ('history', 'settings', 'about'):
        result(ctx, ctx.cli('ui.show', json.dumps({'view': view})))
    result(ctx, ctx.cli('ui.viewState'))
    # ui.quit needs runner-controlled resident restart, so never terminate a shared lane.
    frame(ctx, 'after', 0)
    ctx.blocked('Partial ui.* evidence saved; ui.quit/restart and ui.fileDialog coverage require runner lifecycle/dialog fixtures.', issue=CORE_ISSUE)


@recipe('cli.diagnostics')
def cli_diagnostics(ctx):
    opened(ctx, 3)
    frame(ctx, 'before', 3)
    preview = result(ctx, ctx.cli('diagnostics.bundle', json.dumps({'preview': True, 'includeNames': False})))
    ctx.expect(bool(preview), 'CLI returns package preview')
    outcome = result(ctx, ctx.cli('diagnostics.bundle', json.dumps({'includeNames': False})))
    ctx.expect(bool(outcome['path']), 'CLI saved package path')
    ctx.expect(persisted(ctx) == get(ctx)['document'], 'CLI package creation preserves configuration')
    frame(ctx, 'after', 3)
    ctx.blocked('Partial diagnostics CLI evidence saved; operation/since/output/upload/reveal/clear variants require isolated diagnostics fixtures.', issue=CORE_ISSUE)


@recipe('mcp.configuration', needs=('settings-multi-profile',), timeout=180)
def mcp_configuration(ctx):
    ctx.fixture('settings-multi-profile')
    opened(ctx, 0)
    frame(ctx, 'before', 0)
    old = get(ctx)
    expected = not old['document']['preferences']['notifications']
    try:
        result(ctx, ctx.mcp('patch_configuration', {'patch': {'preferences': {'notifications': expected}},
                           'expected_revision': old['metadata']['observedRevision']}))
        saved(ctx, 'preferences.notifications', expected, 0)
        result(ctx, ctx.mcp('reload_configuration'))
        order = [profile['id'] for profile in old['document']['profiles']]
        ctx.expect(len(order) >= 3, 'multi-profile fixture provides reorderable destinations')
        reversed_order = list(reversed(order))
        result(ctx, ctx.mcp('reorder_destinations', {'order': reversed_order,
                           'expected_revision': get(ctx)['metadata']['observedRevision']}))
        ctx.wait(lambda: [p['id'] for p in persisted(ctx)['profiles']] == reversed_order,
                 'MCP reorder persisted exact profile IDs')
        result(ctx, ctx.mcp('move_destination', {'direction': 'up', 'profile_id': order[0]}))
        moved = reversed_order[:]
        index = moved.index(order[0])
        moved[index - 1], moved[index] = moved[index], moved[index - 1]
        ctx.wait(lambda: [p['id'] for p in get(ctx)['document']['profiles']] == moved
                 and [p['id'] for p in persisted(ctx)['profiles']] == moved,
                 'MCP move persisted the requested adjacent order')
        opened(ctx, 0)
        frame(ctx, 'after', 0)
    finally:
        current = get(ctx)
        call(ctx, 'settings.save', {'document': old['document'],
                                  'expectedRevision': current['metadata']['observedRevision']})


@recipe('mcp.diagnostics')
def mcp_diagnostics(ctx):
    opened(ctx, 3)
    frame(ctx, 'before', 3)
    result(ctx, ctx.mcp('settings_diagnostics', {'action': 'openPackage'}))
    result(ctx, ctx.mcp('settings_diagnostics', {'action': 'setPackage', 'include_names': True}))
    ctx.expect(package(ctx)['includeNames'] is True, 'MCP sets file-name option')
    result(ctx, ctx.mcp('settings_diagnostics', {'action': 'closePackage'}))
    ctx.expect(not package(ctx)['open'], 'MCP closes package dialog')
    preview = result(ctx, ctx.mcp('create_diagnostics_bundle', {'preview': True}))
    ctx.expect(bool(preview), 'MCP returns package preview')
    frame(ctx, 'after', 3)
    ctx.blocked('Partial MCP diagnostics evidence saved; upload target/reveal/clear and saved package variants need isolated diagnostics fixtures.', issue=CORE_ISSUE)


def visible(ctx, title):
    if ctx.profile.get('session') == 'wayland':
        # Native inventory uses the accessibility query contract. These lane
        # callers supply exact titles; X11 keeps its EWMH title expression.
        name = title.removeprefix('^').removesuffix('$')
        return ctx.windows({'name': name, 'visible': True, 'showing': True})
    return ctx.windows({'title': title})


def about_open(ctx):
    opened(ctx, 0)
    result(ctx, ctx.view('ui.show', {'view': 'about'}))
    ctx.wait(lambda: visible(ctx, '^About put.gd$'), 'About window is visible')
    ctx.use_catalog('about', 'about')
    ctx.focus('about', raise_window=True)
    ctx.frame('about-before', window='about', reference='about')


@recipe('about.build-details', 'about.copy-build-details')
def about_details(ctx):
    about_open(ctx)
    expected = call(ctx, 'ui.snapshot')['build']
    try:
        ctx.click('buildDetails', window='about')
        ctx.wait(lambda: ctx.accessible({'owned': True, 'window': 'About put.gd',
                                        'name': 'Copy build details', 'showing': True}),
                 'build disclosure exposes copy action')
        ctx.use_catalog('about', 'about-build-details')
        commit = expected.get('gitCommit') or expected.get('commit')
        facts = ctx.accessible({'owned': True, 'window': 'About put.gd', 'name': 'Commit', 'showing': True})
        ctx.expect(any(row.get('description') == commit or row.get('text') == commit[:12]
                       for row in facts), 'About displays this resident build commit')
        ctx.click('copyBuildDetails', window='about')
        ctx.wait(lambda: clipboard_text(ctx).startswith('{'), 'build JSON copied')
        ctx.expect(json.loads(clipboard_text(ctx)) == expected, 'copied build details exactly match resident')
        ctx.frame('about-expanded', window='about', reference='about-build-details')
        ctx.click('buildDetails', window='about')
        ctx.wait(lambda: not ctx.accessible({'owned': True, 'window': 'About put.gd',
                                            'name': 'Copy build details', 'showing': True}),
                 'build disclosure collapses')
    finally:
        if visible(ctx, '^About put.gd$'):
            ctx.focus('about', raise_window=True)
            ctx.keys('Esc')


def about_close_recipe(identifier, key):
    @recipe(identifier)
    def close(ctx):
        about_open(ctx)
        ctx.note('About close foreground: ' + json.dumps(ctx.focus('about', raise_window=True), sort_keys=True))
        ctx.note('About close control: ' + json.dumps(ctx.accessible(
            {'owned': True, 'window': 'About put.gd', 'name': 'Close',
             'role': 'push button', 'showing': True}), sort_keys=True))
        ctx.frame('about-before-close')
        if key:
            ctx.keys(key)
        else:
            ctx.click('closeAbout', window='about')
        ctx.frame('about-after-close')
        consecutive = 0
        def settled():
            nonlocal consecutive
            closed = not visible(ctx, '^About put.gd$') and bool(visible(ctx, '^put.gd$'))
            consecutive = consecutive + 1 if closed else 0
            return consecutive >= 3
        ctx.wait(settled, 'About stays closed and the main window stays visible')
        ctx.frame('about-closed')
    close.__name__ = identifier.replace('.', '_').replace('-', '_')


about_close_recipe('about.close', None)
about_close_recipe('about.key-esc', 'Esc')


@recipe('about.modeless')
def about_modeless(ctx):
    about_open(ctx)
    try:
        result(ctx, ctx.view('ui.show', {'view': 'settings'}))
        live_click(ctx, 'settingsSection1')
        ctx.wait(lambda: state(ctx)['settings']['section'] == 1, 'main window remains usable with About open')
        ctx.expect(bool(visible(ctx, '^About put.gd$')), 'About remains open during main-window input')
        ctx.frame('modeless-after')
    finally:
        ctx.focus('about', raise_window=True)
        ctx.keys('Esc')


@recipe('about.version')
def about_version(ctx):
    about_open(ctx)
    try:
        expected = call(ctx, 'ui.snapshot')['build']['version'].split('-')[0].split('+')[0]
        query = {'owned': True, 'window': 'About put.gd', 'name': expected,
                 'role': 'label', 'showing': True}
        rows = ctx.accessible(query)
        ctx.expect(len(rows) == 1, 'About version label matches build')
        ctx.click('text:' + expected, window='about')
        ctx.keys('Ctrl+A', 'Ctrl+C')
        ctx.wait(lambda: clipboard_text(ctx) == expected, 'version text can be selected and copied')
        ctx.frame('about-version-selected', window='about', reference='about')
    finally:
        ctx.focus('about', raise_window=True)
        ctx.keys('Esc')


@recipe('advanced.reset-limits', 'confirm.reset-limits', 'confirm.key-esc', needs=('settings',), timeout=180)
def reset_limits(ctx):
    opened(ctx, 3)
    defaults = {'limits': {'maxFiles': 100, 'pendingOperations': 8, 'concurrentUploads': 3,
                          'maxFileBytes': 'auto', 'maxBatchBytes': 'auto', 'pendingArtifactBytes': 'auto',
                          'pendingImageBytes': 67108864, 'clipboardTextBytes': 16777216,
                          'imagePixels': 100000000, 'imageBytes': 536870912},
                'retention': {'failedArtifactDays': 7, 'recentActivityLimit': 100,
                              'previewCacheBytes': 268435456, 'historyRetentionDays': None}}
    changed = {'limits': {key: (1073741824 if value == 'auto' else max(1, value // 2))
                          for key, value in defaults['limits'].items()},
               'retention': {'failedArtifactDays': 3, 'recentActivityLimit': 23,
                             'previewCacheBytes': 134217728, 'historyRetentionDays': 30}}
    patch(ctx, changed)
    baseline = get(ctx)['document']
    live_click(ctx, 'resourceLimitsToggle')
    ctx.use_catalog('settings', 'settings-resource-limits-expanded')
    # Return must choose the safe default, and Escape must cancel too.
    for answer in ('Return', 'Esc', None):
        scroll_click(ctx, 'resetAdvancedDefaults', after_window='settingsConfirm')
        ctx.wait(lambda: visible(ctx, '^Reset limits to defaults$'), 'reset confirmation opens')
        ctx.use_catalog('settingsConfirm', 'confirm-reset-limits')
        ctx.focus('settingsConfirm')
        ctx.frame('reset-confirm-' + str(answer), window='settingsConfirm', reference='confirm-reset-limits')
        if answer:
            ctx.keys(answer)
        else:
            ctx.click('text:Reset limits', window='settingsConfirm')
        ctx.wait(lambda: not visible(ctx, '^Reset limits to defaults$'), 'reset confirmation closes')
        result(ctx, ctx.view('ui.show', {'view': 'settings'}))
        if answer:
            ctx.expect(persisted(ctx) == baseline and get(ctx)['document'] == baseline,
                       'safe default and Escape preserve all settings')
    for group, values in defaults.items():
        for key, value in values.items():
            saved(ctx, group + '.' + key, value, 3)
    accepted = get(ctx)['document']
    ctx.expect(accepted['preferences'] == baseline['preferences'] and accepted['archive'] == baseline['archive'],
               'reset preserves preferences and output')
    frame(ctx, 'reset-after', 3)
    # Fixture teardown restores the complete starting document, including absent keys.


@recipe('confirm.settings-reload', needs=('settings-conflict',), timeout=180)
def reload_confirmation(ctx):
    fixture = ctx.fixture('settings-conflict')
    opened(ctx, 3)
    call(ctx, 'ui.diagnostics', {'action': 'setDesktopDetails', 'enabled': False})
    live_click(ctx, 'desktopSupportDetailsToggle')
    ctx.wait(lambda: call(ctx, 'ui.diagnostics', {'action': 'inspect'})['desktopSupport']['expanded'],
             'configuration actions are disclosed')
    ctx.use_catalog('settings', 'settings-desktop-support-expanded')
    original = get(ctx)['document']['preferences']['notifications']
    # An invalid edit preserves a draft while the external revision arrives.
    replace(ctx, 'recentActivityLimit', 'not-a-number')
    fixture['external_change']({'preferences': {'notifications': not original}})
    scroll_click(ctx, 'reloadSettings', after_window='settingsConfirm')
    ctx.wait(lambda: visible(ctx, '^Reload settings from file$'), 'reload confirmation opens')
    ctx.use_catalog('settingsConfirm', '53-reload-confirm')
    ctx.focus('settingsConfirm')
    ctx.frame('reload-before', window='settingsConfirm', reference='53-reload-confirm')
    ctx.keys('Esc')
    ctx.wait(lambda: not visible(ctx, '^Reload settings from file$'), 'reload canceled')
    result(ctx, ctx.view('ui.show', {'view': 'settings'}))
    scroll_click(ctx, 'reloadSettings', after_window='settingsConfirm')
    ctx.focus('settingsConfirm')
    ctx.click('text:Reload from file', window='settingsConfirm')
    ctx.wait(lambda: not visible(ctx, '^Reload settings from file$'), 'reload accepted')
    saved(ctx, 'preferences.notifications', not original, 3)
    ctx.wait(lambda: not state(ctx)['settings'].get('conflict'), 'reload clears conflict')
    frame(ctx, 'reload-after', 3)


def routing(ctx):
    return state(ctx)['settings']['captureRouting']


@recipe('capture.default-destination')
def default_destination(ctx):
    fixture = ctx.fixture('settings-destinations')
    opened(ctx, 1)
    for index, profile_id in enumerate(fixture.profile_ids, 1):
        choose(ctx, 'defaultCaptureDestination', index)
        saved(ctx, 'captureDefaults.profileId', profile_id, 1)
        ctx.wait(lambda: routing(ctx)['default']['profileId'] == profile_id,
                 'default destination is reflected in the live route')
        for action in ('region', 'window', 'desktop', 'clipboard', 'edit', 'record'):
            ctx.expect(routing(ctx)[action]['inherited'] is True and
                       routing(ctx)[action]['profileId'] == profile_id,
                       action + ' inherits the selected destination')
    frame(ctx, 'destinations-after', 1)


@recipe('capture.default-sharing')
def default_sharing(ctx):
    fixture = ctx.fixture('settings-share-destination')
    opened(ctx, 1)
    for index, mode in ((1, 'share'), (0, 'default')):
        choose(ctx, 'defaultCaptureSharing', index)
        saved(ctx, 'captureDefaults.publicationMode', mode, 1)
        ctx.wait(lambda: routing(ctx)['default']['publicationMode'] == mode and
                 routing(ctx)['default']['effectivePublicationMode'] == 'share',
                 'default/share publication matches the loopback destination')
        frame(ctx, 'sharing-' + mode, 1)
    if fixture.private_supported:
        choose(ctx, 'defaultCaptureSharing', 2)
        saved(ctx, 'captureDefaults.publicationMode', 'private', 1)
        ctx.wait(lambda: routing(ctx)['default']['effectivePublicationMode'] == 'private', 'private publication is reflected in the live route')
    else:
        ctx.blocked('Default and share proved with settings-share-destination; Keep private requires a private-capable synthetic provider, which loopback S3 does not supply.', issue=681)


@recipe('capture.no-output')
def no_output(ctx):
    # Keep the owned choices while removing every output, so both recovery
    # controls can be exercised without uploading or acquiring a source.
    fixture = ctx.fixture('settings-destinations')
    opened(ctx, 1, fixture='settings-no-output', keep_output=False)
    notice = lambda: ctx.accessible({'owned': True, 'name': 'New captures have no output.*', 'visible': True, 'showing': True})
    ctx.wait(notice, 'no-output warning is drawn before source acquisition')
    live_click(ctx, 'chooseCaptureDestination')
    ctx.keys('Home', 'Down', 'Return', 'Tab')
    saved(ctx, 'captureDefaults.profileId', fixture.profile_ids[0], 1)
    ctx.wait(lambda: not notice(), 'choosing a destination clears no-output warning')
    frame(ctx, 'destination-recovery', 1)
    patch(ctx, {'captureDefaults': {'destination': 'upload', 'profileId': None, 'publicationMode': 'default'}})
    ctx.wait(lambda: bool(notice()), 'no-output warning returns')
    live_click(ctx, 'enableLocalCopies')
    saved(ctx, 'archive.keepUploads', True, 1)
    ctx.wait(lambda: not notice(), 'local copies clear no-output warning')
    frame(ctx, 'local-recovery', 1)


@recipe('capture.exceptions', timeout=240)
def capture_exceptions(ctx):
    fixture = ctx.fixture('settings-action-routes')
    opened(ctx, 1)
    live_click(ctx, 'captureExceptionsToggle')
    for action, label in (('region', 'Capture region'), ('window', 'Capture window'),
                          ('desktop', 'Capture desktop'), ('edit', 'Capture region & edit'),
                          ('clipboard', 'Upload clipboard'), ('record', 'Record screen')):
        live_click(ctx, 'addCaptureException')
        ctx.click_accessible({'owned': True, 'pid': ctx.geometry('settings')['pid'],
                              'role': 'menu item', 'name': label, 'visible': True, 'showing': True})
        ctx.wait(lambda: not routing(ctx)[action]['inherited'], action + ' exception added')
        choose(ctx, 'profile' + action.title(), 2)
        saved(ctx, 'actions.' + action + '.profileId', fixture.profile_ids[1], 1)
        for index, mode in ((1, 'share'), (0, 'default')):
            choose(ctx, 'publication' + action.title(), index)
            saved(ctx, 'actions.' + action + '.publicationMode', mode, 1)
            ctx.wait(lambda: routing(ctx)[action]['publicationMode'] == mode,
                     action + ' exception sharing reflected in live route')
        frame(ctx, action + '-exception', 1)
        live_click(ctx, 'remove' + action.title() + 'Exception')
        ctx.wait(lambda: routing(ctx)[action]['inherited'], action + ' exception removed')
        ctx.expect(routing(ctx)[action]['profileId'] == routing(ctx)['region' if action == 'record' else 'default']['profileId'],
                   action + ' output inherits again')
        if action == 'record':
            ctx.wait(lambda: not any(k in get(ctx)['document']['actions']['record'] for k in ('destination', 'profileId', 'publicationMode')), 'record exception overrides are removed')
        else:
            saved(ctx, 'actions.' + action + '.destination', 'default', 1)
            saved(ctx, 'actions.' + action + '.profileId', None, 1)
            saved(ctx, 'actions.' + action + '.publicationMode', 'default', 1)
        persisted(ctx)


def exercise_disclosure(ctx, control, section):
    observe = ctx.fixture('settings-observations').disclosure
    # Bring a clipped disclosure back before inspecting its showing state.
    live_click(ctx, control)
    opened_state = observe(control)
    if not opened_state.expanded:
        live_click(ctx, control)
        opened_state = ctx.wait(lambda: (s if s.expanded else None)
                               if (s := observe(control)) else None, control + ' expands')
    description = opened_state.control.get('description', '').removeprefix('Explanation shown. ')
    if control == 'shortcutHelpToggle':
        ctx.expect(any('Escape cancels' in text for text in opened_state.text), 'shortcut instructions are rendered accessibly')
    else:
        ctx.expect(bool(description) and any(description in text for text in opened_state.text),
                   control + ' renders its actual help description')
    # A checked glyph alone cannot prove that its help body was drawn. Bind
    # the expanded text to the actual disappearance of that body on collapse,
    # even when another caption repeats the same words.
    needle = 'Escape cancels' if control == 'shortcutHelpToggle' else description
    expanded_count = sum(needle in text for text in opened_state.text)
    frame(ctx, control + '-expanded', section)
    live_click(ctx, control)
    def collapsed():
        current = observe(control)
        return not current.expanded and sum(needle in text for text in current.text) < expanded_count
    ctx.wait(collapsed, control + ' collapses and hides its help body')
    frame(ctx, control + '-collapsed', section)


@recipe('capture.help-toggles', timeout=240)
def capture_help(ctx):
    opened(ctx, 1)
    controls = ctx.catalog.candidates('glob:*Help', 'settings', REFERENCES[1])
    names = sorted({c['name'] for c in controls if c.get('visible', True) and c.get('class') == 'QToolButton'})
    ctx.expect(bool(names), 'capture catalog declares visible per-setting help')
    for name in names:
        exercise_disclosure(ctx, name, 1)


@recipe('shortcuts-page.help')
def shortcut_help(ctx):
    opened(ctx, 2)
    exercise_disclosure(ctx, 'shortcutHelpToggle', 2)


@recipe('shortcuts-page.refresh', timeout=120)
def shortcut_refresh(ctx):
    opened(ctx, 2)
    fixture = ctx.fixture('settings-binding-conflict' if ctx.profile.get('session') == 'x11' else 'settings-binding-unavailable')
    # The fixture restarts the private helper to establish actual channel state.
    opened(ctx, 2)
    before = get(ctx)['document']['actions']
    if fixture.state == 'conflict':
        ctx.wait(lambda: fixture.observe().get('bindings', {}).get('region', {}).get('bound') is False,
                 'external contender prevents native region binding')
        fixture.release()
    else:
        ctx.expect(fixture.observe().get('available') is False, 'native shortcut channel is actually unavailable')
    live_click(ctx, 'refreshShortcutSupport')
    if fixture.state == 'conflict':
        ctx.wait(lambda: fixture.observe().get('bindings', {}).get('region', {}).get('bound') is True,
                 'refresh acquires the released native shortcut')
    else:
        ctx.wait(lambda: fixture.observe().get('available') is False,
                 'checking again does not report an absent native helper as available')
    ctx.expect(get(ctx)['document']['actions'] == before, 'refresh preserves configured shortcuts')
    frame(ctx, 'shortcuts-refreshed', 2)
    if fixture.state == 'unavailable' and callable(fixture.get('release')):
        fixture.release()
        ctx.wait(lambda: fixture.observe().get('available') is True, 'owned native helper is restored')
        ctx.expect(get(ctx)['document']['actions'] == before, 'restoring the helper preserves shortcuts')


@recipe('capture.browse', timeout=120)
def browse(ctx):
    from . import dialogs
    backend = 'fallback'
    fixture = ctx.fixture('file-dialog-' + backend)
    if not fixture.available:
        ctx.blocked(fixture.reason, issue=671)
    opened(ctx, 1)
    files = dialogs._files(ctx)
    inspect = lambda: fixture.observe('outputFolder', 'root')
    original = get(ctx)['document']['archive']['root']
    live_click(ctx, 'text:Choose default output folder')
    try:
        dialogs._ready(ctx, inspect, backend, 'folder')
        dialogs._input(ctx, backend, files['folder'], 'folder', inspect)
        # QFileDialog may navigate into an existing typed directory on Return.
        # Complete its real Choose action before asserting the exact result.
        if inspect()['open']:
            ctx.click('text:Choose', window='putgdFileDialog')
        selected = ctx.wait(lambda: (s if not s['open'] else None)
                            if (s := inspect()) else None, 'folder chooser completed')
        ctx.expect(selected['last']['status'] == 'chosen' and
                   selected['last']['path'].rstrip('/') == files['folder'], 'chooser selected the exact synthetic folder')
        saved(ctx, 'archive.root', files['folder'], 1)
        frame(ctx, 'browse-chosen', 1)
    finally:
        if inspect()['open']:
            call(ctx, 'ui.fileDialog', {'dialog': 'outputFolder', 'action': 'cancel', 'field': 'root'})
        patch(ctx, {'archive': {'root': original}})
    ctx.blocked('Fallback Browse open/choose and persisted folder proved; deterministic Failed result and Try again still require the production chooser-failure boundary (#671).', issue=671)


# Honest declarations for edges that the current interface cannot prepare or
# observe. Each gets its own blocked receipt (and never a visual-only pass).
BLOCKED = {
    'settings.footer': 'settings-save-failure and conflict fixtures exist; the core footer bundle still needs a live proof of autosave state, Retry saving and conflict handling (#681). Accepted config alone cannot prove retry UI.',
    'shortcuts-page.chord': 'Input-events fixture exists; current recording status text must be accessible before proving overlapping-key, AltGr and shifted-digit outcomes (#862).',
    'shortcuts-page.focus-loss': 'External focus fixture exists; current recording outcome text is not accessible on Qt 6.4 (#862), and Wayland hide still needs native minimize support.',
    'shortcuts-page.unsupported-desktop': 'Required desktop profiles have a shortcut channel; needs an explicitly unsupported Wayland profile.',
    'advanced.resource-limits': 'Needs complete resource-field exercise including Auto/No limit, resolved hints and per-field resets; partial value checks cannot prove this ID.',
    'advanced.clear-logs': 'Needs seeded persistent logs and confirmation control inventory with accept/cancel assertions.',
    'advanced.unclean-exit': 'Needs runner-owned previous unclean-exit fixture with exact expected identity/cause.',
    'advanced.config-file': 'Needs host editor/file-manager observation and failed-editor Copy path fixture; Copy path is normally hidden.',
    'diagnostics.folder': 'Needs native folder chooser fixture, chosen-directory assertion and deterministic failure/retry.',
    'diagnostics.show-in-folder': 'Needs file-manager observation for exact saved path plus absent/removed-package refusal fixture.',
    'about.website': 'Needs host browser/portal observation, including failed-open feedback; UI state exposes neither.',
    'confirm.clear-logs': 'Needs seeded logs plus confirmation control inventory for accept/cancel.',
    'confirm.in-app-notices': 'Needs runner-owned slow-checkpoint, invalid-draft, slow-close and unfinished-credential fixtures.',
}
for _identifier in ('settings.update-privacy', 'settings.update-toggle', 'settings.update-status',
                    'settings.update-install', 'settings.update-restart', 'settings.update-commands'):
    BLOCKED[_identifier] = 'Update feed/lifecycle APIs exist; updates.bundle is not provisioned in snapshot/profiles. Await the core update bundle for request-ledger, install and restart evidence (#681).'


def _declare_blocked(identifier, reason):
    @recipe(identifier)
    def unavailable(ctx):
        ctx.blocked(reason, issue=862 if identifier in ('shortcuts-page.chord', 'shortcuts-page.focus-loss') else 671 if identifier in ('diagnostics.folder',) else CORE_ISSUE)
    unavailable.__name__ = 'blocked_' + identifier.replace('-', '_').replace('.', '_')


for _identifier, _reason in BLOCKED.items():
    _declare_blocked(_identifier, _reason)
