#!/usr/bin/env python3
"""Live Linux tray-progress replay on an already provisioned disposable VM.

Secrets, package installation, console unlocking and VM lifecycle belong to the
operator. This runner retains protocol assertions and QMP rendering witnesses
separately; exit zero is never a human graphical-acceptance receipt.
"""
import argparse
from decimal import Decimal
from urllib.parse import urlsplit
import importlib.util
import inspect
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import time
import uuid

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('putgd_tray_lab', HERE / 'lab.py')
lab = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lab)
TERMINAL = {'completed', 'partial', 'failed', 'cancelled', 'interrupted'}
# Final clipboard outcomes (OperationClipboard): partial means some links
# were copied while another copy failed or was suppressed.
COPY_SETTLED = ('copied', 'partial', 'suppressed', 'failed', 'no_result')
# TrayController::refreshActivity: the tooltip's title, then its activity rows.
IDLE = 'put.gd'
UPLOADING = 'Uploading'
COMPLETE = 'Upload complete'
ATTENTION = 'Needs attention'
FINISHING = 'Finishing upload'
REVIEW = ' — open the tray menu to review'
# TrayController::latchFailure: the attention item each runner failure latches.
ATTENTION_LABELS = {'local-save': ('Local save needs attention',), 'proton-busy': ('Failed uploads',)}
# A latched failure must survive at least this long without being opened.
LATCH_SECONDS = 3
LABELS = {'preflight': 'Checking destination', 'saving_local': 'Saving local copy',
          'sharing': 'Creating share link', 'reconciling': 'Checking previous upload'}
# Theme::trayTints (src/ui/Theme.cpp): the panel the icon sits on, not the app
# theme, picks the set (#60). IconArt uses encoding for the progress fill over
# the idle glyph; upload is reserved for confirmed completion.
TRAY_TINTS = {
    'dark': {'idle': '#d6d6d2', 'upload': '#91bca2',
             'encoding': '#e8b66d', 'error': '#e59b93', 'live': '#f0524f'},
    'light': {'idle': '#383834', 'upload': '#386a4b',
              'encoding': '#8f5b20', 'error': '#a44139', 'live': '#c8302b'},
}


class TrayProgressError(RuntimeError):
    pass


class ObservationMismatch(TrayProgressError):
    """Only asynchronous production/SNI correlation may receive batching grace."""


class VisibleMenuFailed(TrayProgressError):
    """The desktop's own tray menu did not deliver the click; the D-Bus event may stand in, reported."""


def observed(condition, message):
    if not condition:
        raise ObservationMismatch(message)


def require(condition, message):
    if not condition:
        raise TrayProgressError(message)


def socket_paths(guest_root, qmp_socket, native_paths=()):
    """Read-only preflight, before mkdir or any process launch."""
    paths = [Path(guest_root).resolve() / 'r/putgd/session.sock',
             Path(qmp_socket).resolve(), *(Path(p).resolve() for p in native_paths)]
    result = {str(path): {'utf8Bytes': len(str(path).encode('utf-8'))} for path in paths}
    require(all(p['utf8Bytes'] <= 107 for p in result.values()),
            'Resolved Unix socket exceeds 107 UTF-8 bytes: ' + json.dumps(result) +
            '; preserve this attempt and choose a short new run path')
    return result


def byte_summary(value):
    """ByteSize::summary: MB below a GB, GB above, two decimals, 1024 math."""
    unit = 1024**3 if value >= 1024**3 else 1024**2
    hundredths = (value % unit * 100 + unit // 2) // unit
    whole = value // unit
    if value > 0 and not whole and not hundredths:
        return '<0.01 MB'
    if hundredths == 100:
        whole, hundredths = whole + 1, 0
    fraction = ('.' + str(hundredths).zfill(2).rstrip('0')) if hundredths else ''
    return str(whole) + fraction + (' GB' if unit == 1024**3 else ' MB')


def displayed_interval(text):
    """An integer-byte range consistent with the tooltip's rounded byte summary."""
    if text == '<0.01 MB':
        return 1, (1024**2 + 199) // 200 - 1
    number, suffix = text.split()
    unit = 1024**3 if suffix == 'GB' else 1024**2
    center = Decimal(number) * unit
    half = Decimal(unit) / 200
    # Include boundary integer bytes; percentage is checked against this same range.
    return max(0, int(center - half)), int(center + half)


SIZE = r'(?:<0\.01|[0-9]+(?:\.[0-9]{1,2})?) (?:MB|GB)'
COUNTER = re.compile(r'^(.+?): (' + SIZE + r')(?: of (' + SIZE + r')(?: \(([0-9]+)%\))?| transferred · Source size: (' + SIZE + r')(?: · Estimated ([0-9]+)%)?)$')
# Several files of one operation transferring at once (#175): combined bytes and counts.
BATCH = re.compile(r'^(.+?)(?:: (' + SIZE + r') of (' + SIZE + r') \(([0-9]+)%\)|: (' + SIZE + r') transferred)? · ([0-9]+) uploading'
                   r'(?:, ([0-9]+) waiting)?(?:, ([0-9]+) done)?(?:, ([0-9]+) failed)?$')


def ini_value(text, group, key):
    """One key of one [group] in a KDE configuration file (PanelTone.cpp iniValue)."""
    inside = False
    for raw in text.split('\n'):
        line = raw.strip()
        if line.startswith('['):
            inside = line == '[' + group + ']'
            continue
        if inside and line.startswith(key + '='):
            return line[len(key) + 1:].strip()
    return ''


def light_colour(value):
    """KDE "r,g,b": light when black text contrasts more than white (PanelTone.cpp)."""
    parts = value.split(',')
    if len(parts) < 3:
        return None
    try:
        rgb = [int(part.strip()) for part in parts[:3]]
    except ValueError:
        return None
    if any(c < 0 or c > 255 for c in rgb):
        return None
    channel = lambda c: c / 12.92 if c <= 0.03928 else ((c + 0.055) / 1.055) ** 2.4
    lum = sum(w * channel(c / 255) for w, c in zip((0.2126, 0.7152, 0.0722), rgb))
    return (lum + 0.05) / 0.05 > 1.05 / (lum + 0.05)


def panel_tone(inputs):
    """PanelTone::decide: (light, source) from what the resident's environment reads."""
    names = [name for name in inputs.get('desktop', '').lower().split(':') if name]
    if 'kde' in names:
        style = ini_value(inputs.get('plasmarc', ''), 'Theme', 'name').lower()
        tone = light_colour(ini_value(inputs.get('styleColors', ''), 'Colors:Window', 'BackgroundNormal'))
        if tone is not None:
            return tone, 'plasma-style'
        if 'dark' in style:
            return False, 'plasmarc'
        if 'light' in style:
            return True, 'plasmarc'
        for group in ('Colors:Header', 'Colors:Window'):
            tone = light_colour(ini_value(inputs.get('kdeglobals', ''), group, 'BackgroundNormal'))
            if tone is not None:
                return tone, 'kdeglobals'
        return True, 'kde-default'
    if 'x-cinnamon' in names or 'cinnamon' in names:
        theme = inputs.get('cinnamonTheme', '').lower()
        return (False, 'default') if not theme else ('dark' not in theme, 'cinnamon-theme')
    if any(name in names for name in ('gnome', 'ubuntu', 'unity', 'pantheon')):
        return False, 'gnome-shell'
    return False, 'default'


# Runs in the resident's session: lists the owned SNI's visible dbusmenu labels
# and, with a label, sends one "clicked" Event to the single matching item (an
# exact label, or a "Failed uploads (N)" count). No pointer or coordinates.
TRAY_MENU = r'''import json,os,re,subprocess
address=os.environ.get('DBUS_SESSION_BUS_ADDRESS') or 'unix:path=/run/user/%d/bus' % os.getuid()
def bus(*args):
    reply=subprocess.run(['busctl','--address='+address,'--json=short',*args],capture_output=True,text=True,timeout=20)
    if reply.returncode:
        print(json.dumps({'ok':False,'error':'dbus_call_failed'}));raise SystemExit(0)
    return json.loads(reply.stdout).get('data') if reply.stdout.strip() else []
service,path,wanted=SERVICE,PATH,LABEL
menu=bus('get-property',service,path,'org.kde.StatusNotifierItem','Menu')
menu=menu[0] if isinstance(menu,list) else menu
layout=bus('call',service,menu,'com.canonical.dbusmenu','GetLayout','iias','--','0','-1','0')
items,rows=[],[]
def walk(node,usable=True,depth=0):
    if isinstance(node,dict):node=node['data']
    ident,props,children=node
    values={key:(value.get('data') if isinstance(value,dict) else value) for key,value in props.items()}
    usable=usable and values.get('visible',True) and values.get('enabled',True)
    label=str(values.get('label','')).replace('&','').replace('_','').strip()
    if usable and label:
        items.append((ident,label))
        # Keyboard navigation visits the top level's visible, enabled rows.
        if depth==1 and values.get('type','standard')!='separator':rows.append({'id':ident,'label':label})
    for child in children:walk(child,usable,depth+1)
walk(layout[1])
result={'ok':True,'labels':[label for _,label in items],'rows':rows}
if wanted is not None:
    matches=[(ident,label) for ident,label in items if label==wanted or re.fullmatch(re.escape(wanted)+r' \([0-9]+\)',label)]
    if len(matches)!=1:
        print(json.dumps({'ok':False,'error':'attention_item_not_unique'}));raise SystemExit(0)
    bus('call',service,menu,'com.canonical.dbusmenu','Event','isvu',str(matches[0][0]),'clicked','s','','0')
    result.update(clicked=1,label=matches[0][1])
print(json.dumps(result))
'''


# Runs in the resident's own environment (Replay.session), so it reads the
# configuration cascade put.gd's PanelTone::refresh reads: XDG_CONFIG_HOME, its
# kdedefaults, then XDG_CONFIG_DIRS, first file that sets the key; the Plasma
# style's colors from the data dirs; Cinnamon's theme through gsettings.
PANEL_READER = inspect.getsource(ini_value) + r'''
import json, os, subprocess
home = os.path.expanduser('~')
def base(var, default):
    value = os.environ.get(var, '')
    return value if value.startswith('/') else os.path.join(home, default)
def dirs(var, default):
    return [d for d in (os.environ.get(var) or default).split(':') if d.startswith('/')]
config, config_dirs = base('XDG_CONFIG_HOME', '.config'), dirs('XDG_CONFIG_DIRS', '/etc/xdg')
data_dirs = [base('XDG_DATA_HOME', '.local/share')] + dirs('XDG_DATA_DIRS', '/usr/local/share:/usr/share')
def small(path):
    try:
        if os.path.isfile(path) and os.path.getsize(path) < (1 << 20):
            with open(path, 'rb') as stream:
                return stream.read().decode('utf-8', 'replace')
    except OSError:
        pass
    return ''
def cascade(name):
    files = [config + '/' + name, config + '/kdedefaults/' + name]
    for path in [d + '/' + name for d in [config] + config_dirs]:
        if os.path.isfile(path) and path not in files:
            files.append(path)
    return files
def first(name, group, key):
    for path in cascade(name):
        text = small(path)
        if ini_value(text, group, key):
            return text
    return ''
desktop = os.environ.get('XDG_CURRENT_DESKTOP', '')
names = [name for name in desktop.lower().split(':') if name]
inputs = {'desktop': desktop}
if 'kde' in names:
    inputs['plasmarc'] = first('plasmarc', 'Theme', 'name')
    inputs['kdeglobals'] = first('kdeglobals', 'Colors:Header', 'BackgroundNormal') or \
        first('kdeglobals', 'Colors:Window', 'BackgroundNormal')
    style = ini_value(inputs['plasmarc'], 'Theme', 'name')
    style = 'default' if not style or '/' in style or '..' in style else style
    inputs['plasmaStyle'] = style
    inputs['styleColors'] = next((small(p) for p in (d + '/plasma/desktoptheme/' + style + '/colors' for d in data_dirs)
                                  if os.path.isfile(p)), '')
if 'x-cinnamon' in names or 'cinnamon' in names:
    try:
        out = subprocess.run(['gsettings', 'get', 'org.cinnamon.theme', 'name'], capture_output=True, timeout=3).stdout
        inputs['cinnamonTheme'] = out[:512].decode('utf-8', 'replace').strip().replace("'", '')
    except (OSError, subprocess.TimeoutExpired):
        pass
print(json.dumps(inputs))
'''


def check_tooltip(snapshot, records, provider, pid, *, attention=None, tints=TRAY_TINTS['dark']):
    """Compare SNI bytes/phase with recent independent production CLI observations.

    The resident batches observations, and CLI and DBus calls are not atomic.
    Records bracket the SNI read and cover the previous two seconds. Displayed
    counters must fit a real observed range and their own rounded percentage;
    SDK counters may decrease. Source-size estimates must be explicitly labelled
    and bounded independently against observed bytes and the acquired file size.

    attention lists the labels a runner-created failure may latch. The resident
    keeps "Needs attention" with the error glyph until the tray menu's attention
    item is opened (TrayController::latchFailure); without a runner-created
    failure that state is fatal.
    """
    require(snapshot.get('pid') == pid and snapshot.get('hostRegistered') and snapshot.get('registered'),
            'SNI must be registered with the native host and owned by the expected app PID')
    text = snapshot.get('description', '')
    require(not re.search(r'https?://|file://|synthetic-[12]\.txt|/tmp/|/home/', text, re.IGNORECASE),
            'Tray tooltip exposes a source name, path or link')
    title = snapshot.get('title')
    require(title in (IDLE, UPLOADING, COMPLETE, ATTENTION), 'Unexpected tray title')
    icons = snapshot.get('icons', [])
    has = lambda icon, role: icon.get('opaqueColors', {}).get(tints[role], 0) > 0
    require(icons, 'SNI has no icon pixmaps')
    if title == ATTENTION:
        require(attention, 'Tray needs attention without a runner-created failure')
        first, _, rest = text.partition('\n')
        labels = [label for label in attention if first == label + REVIEW]
        require(labels, 'Attention text does not name the expected failure')
        # The error glyph and badge replace every progress or completion fill.
        require(all(has(icon, 'error') and not has(icon, 'upload') and not has(icon, 'encoding') for icon in icons),
                'Attention icon must be the error glyph without progress or completion')
        result = {'idle': False, 'attention': labels[0], 'storageActive': False, 'counter': False, 'phases': []}
        if rest:
            check_activity(rest.splitlines(), records, provider, result)
        return result
    require(not any(has(icon, 'error') for icon in icons), 'Error tint outside the attention state')
    if title == COMPLETE:
        require(text == COMPLETE, 'Completion title without completion text')
        observed(not records and all(has(icon, 'upload') for icon in icons), 'Completion fill must be green with no remaining work')
        require(all(not has(icon, 'encoding') and not has(icon, 'idle') for icon in icons),
                'Completion frame must be fully green without a progress fill')
        return {'idle': False, 'storageActive': False, 'counter': False, 'phases': []}
    require(not any(has(icon, 'upload') for icon in icons), 'Completion tint before a confirmed completion')
    # Partial progress keeps idle pixels beside the encoding fill; a nearly
    # complete fill covers the whole glyph. A sliver of fill reaches the large
    # sizes before the smallest, so every advertised size must contain the
    # fill before it earns storage coverage.
    filled = [has(icon, 'encoding') for icon in icons]
    require(all(has(icon, 'idle') for icon in icons) or all(filled),
            'SNI icon does not have the required idle/active brand color')
    if title == IDLE and not text:
        require(not any(filled) and all(has(icon, 'idle') for icon in icons), 'Idle tray retains a progress fill')
        observed(not records, 'Idle tray while production work remains active')
        return {'idle': True, 'storageActive': False, 'counter': False, 'phases': []}
    observed(text, 'Working tray has no activity text')
    result = {'idle': False, 'storageActive': all(filled), 'counter': False, 'phases': []}
    if text == FINISHING:
        # The 200-600 ms hold after the last transfer, before completion.
        require(title == UPLOADING, 'Finishing text outside an upload')
        result['finishing'] = True
        return result
    check_activity(text.splitlines(), records, provider, result)
    return result


def check_activity(lines, records, provider, result):
    """Each activity row must match an independently observed operation row."""
    labels = {LABELS.get(record['phase'], record['phase'].replace('_', ' ').capitalize()): record['phase']
              for record in records}
    for row_index, line in enumerate(lines):
        overflow = re.fullmatch(r'([0-9]+) more active operations', line)
        if overflow:
            observed(any(record.get('activeCount') == int(overflow[1]) + 3 for record in records),
                    'Tray overflow count differs from observed active operations')
            continue
        batch = BATCH.fullmatch(line)
        if batch:
            observed(batch[1] in labels, 'Tray phase has no matching production observation')
            phase = labels[batch[1]]
            result['phases'].append(phase)
            counts = tuple(int(batch[index] or 0) for index in (6, 7, 8, 9))
            applicable = [record for record in records if record['phase'] == phase and record.get('rowIndex', 0) == row_index
                          and (record.get('running'), record.get('waiting'), record.get('done'), record.get('failed')) == counts]
            observed(counts[0] > 1 and applicable, 'Tray batch row has no matching production observation')
            sent = batch[2] or batch[5]
            if sent:
                lower, upper = displayed_interval(sent)
                low = max(lower, min(record['batchTransferred'] for record in applicable))
                high = min(upper, max(record['batchTransferred'] for record in applicable))
                observed(low <= high, 'Tray batch bytes lie outside production observations')
                observed(not batch[3] or any(byte_summary(record['batchTotal']) == batch[3] for record in applicable),
                         'Tray batch total differs from production observations')
                result['counter'] = True
            continue
        match = COUNTER.fullmatch(line)
        prefix = match[1] if match else line.split(' · Source size: ', 1)[0]
        label = re.sub(r' file [0-9]+/[0-9]+$', '', prefix)
        observed(label in labels, 'Tray phase has no matching production observation')
        phase = labels[label]
        result['phases'].append(phase)
        applicable = [record for record in records if record['phase'] == phase and record.get('rowIndex', 0) == row_index]
        ordinal = re.search(r' file ([0-9]+)/([0-9]+)$', prefix)
        if ordinal:
            applicable = [record for record in applicable if record.get('fileIndex') == int(ordinal[1])
                          and record.get('fileCount') == int(ordinal[2])]
        observed(applicable, 'Tray row has no matching ordered production observation')
        if not match:
            source = line.split(' · Source size: ', 1)
            observed(len(source) == 1 or any(source[1] == byte_summary(record['sourceBytes']) for record in applicable),
                    'Tray source size differs from acquired artifact bytes')
            require(':' not in prefix and '%' not in line, 'Unrecognized tray counter')
            continue
        lower, upper = displayed_interval(match[2])
        numeric = [record for record in applicable if isinstance(record.get('bytesTransferred'), int)]
        observed(numeric, 'Tray reports transferred bytes without a production counter')
        low = max(lower, min(record['bytesTransferred'] for record in numeric))
        high = min(upper, max(record['bytesTransferred'] for record in numeric))
        observed(low <= high, 'Tray transferred bytes lie outside production observations')
        if match[5]:
            require(provider == 'proton-drive', 'S3 replay requires its real transfer total')
            observed(any(match[5] == byte_summary(record['sourceBytes']) for record in numeric),
                    'Source size differs from acquired artifact bytes')
            require(all(not record.get('bytesTotal') for record in numeric), 'Source fallback hides a known transfer total')
            if match[6] is not None:
                estimate = int(match[6])
                observed(any(record['sourceBytes'] > 0 and
                             (estimate == 100 if phase == 'finalizing' else
                              min(99, low * 100 // record['sourceBytes']) <= estimate <= min(99, high * 100 // record['sourceBytes']))
                             for record in numeric), 'Source-size estimate contradicts observed bytes')
        else:
            require(provider != 'proton-drive', 'Proton must keep encrypted bytes separate from source size')
            totals = {record['bytesTotal'] for record in numeric if record.get('bytesTotal', 0) > 0
                      and byte_summary(record['bytesTotal']) == match[3]}
            observed(totals and match[4] is not None, 'S3 needs a real matching transfer total and percentage')
            percent = int(match[4])
            observed(any(low <= total and low * 100 // total <= percent <= min(high, total) * 100 // total
                        for total in totals), 'Tray percentage contradicts its displayed transfer bytes')
        result['counter'] = True


def menu_events(lines, service):
    """What other bus clients asked of put.gd's dbusmenu, from `busctl monitor --json=short`.

    Returns (member, item id, event) in arrival order: AboutToShow/
    AboutToShowGroup as ('show', id, None); Event and EventGroup entries as
    ('event', id, name). Only calls addressed to the resident's unique name count.
    """
    found = []
    for line in lines:
        try:
            message = json.loads(line)
        except ValueError:
            continue
        if not isinstance(message, dict) or message.get('type') != 'method_call' or \
                message.get('destination') != service or message.get('interface') != 'com.canonical.dbusmenu':
            continue
        data = (message.get('payload') or {}).get('data') or []
        member = message.get('member')
        if member == 'AboutToShow' and data:
            found.append(('show', data[0], None))
        elif member == 'AboutToShowGroup' and data:
            found.extend(('show', ident, None) for ident in data[0])
        elif member == 'Event' and len(data) >= 2:
            found.append(('event', data[0], data[1]))
        elif member == 'EventGroup' and data:
            found.extend(('event', entry[0], entry[1]) for entry in data[0] if len(entry) >= 2)
    return found


def locate_tinted_icon(path, colour, near, reach=(240, 60), minimum=20):
    """Centre of the exact-colour pixels within reach of near, or None.

    Tray icons move when other items come and go, so the supplied position is
    only a hint; a latched icon carries the error tint the runner knows.
    """
    width, height, pixels = lab.ppm(path)
    target = bytes.fromhex(colour.lstrip('#'))
    x0, x1 = max(0, near[0] - reach[0]), min(width, near[0] + reach[0] + 1)
    y0, y1 = max(0, near[1] - reach[1]), min(height, near[1] + reach[1] + 1)
    found = [(x, y) for y in range(y0, y1) for x in range(x0, x1)
             if pixels[(y * width + x) * 3:(y * width + x) * 3 + 3] == target]
    if len(found) < minimum:
        return None
    xs, ys = [x for x, _ in found], [y for _, y in found]
    return (min(xs) + max(xs)) // 2, (min(ys) + max(ys)) // 2


def menu_presses(rows, label, first_row_selected):
    """(target row, Down presses) to reach the attention row, or (None, None)."""
    for index, row in enumerate(rows):
        if row['label'] == label or re.fullmatch(re.escape(label) + r' \([0-9]+\)', row['label']):
            return row, index if first_row_selected else index + 1
    return None, None


def records_from(operations):
    active = sorted((op for op in operations if op.get('state') not in TERMINAL),
                    key=lambda op: (op.get('created', ''), op['id']))
    records = []
    for row_index, op in enumerate(active):
        phase = op.get('phase', op['state'])
        items = op.get('items', [])
        index = next((i for i, item in enumerate(items) if item.get('uploadStatus') == 'uploading'),
                     next((i for i, item in enumerate(items) if item.get('phase') == phase), 0))
        item = items[index] if items else {}
        progress = item.get('uploadProgress') or {}
        counts = {'running': 0, 'waiting': 0, 'done': 0, 'failed': 0, 'batchTransferred': 0, 'batchTotal': 0}
        for each in items:
            status = each.get('uploadStatus')
            key = ('failed' if status in ('failed', 'unknown') else 'done' if status in ('uploaded', 'skipped', 'conflict')
                   else 'waiting' if each.get('phase') == 'queued' else 'running' if status == 'uploading' else None)
            if key:
                counts[key] += 1
            counts['batchTransferred'] += (each.get('uploadProgress') or {}).get('bytesTransferred', 0)
            counts['batchTotal'] += (each.get('uploadProgress') or {}).get('bytesTotal', 0)
        records.append({**counts, 'operationId': op['id'], 'artifactId': item.get('artifact', {}).get('id'),
                        'state': op['state'], 'phase': phase, 'fileIndex': index + 1, 'fileCount': len(items),
                        'sourceBytes': item.get('artifact', {}).get('bytes', 0),
                        'activeCount': len(active), 'rowIndex': row_index, **{key: progress[key] for key in
                         ('bytesTransferred', 'bytesTotal') if key in progress}})
    return records



def correlated_records(history, current):
    """Only the current operation/artifact may supply a row's batched counter.

    An operation moving up a row retains its history. A completed predecessor
    or previous file never supplies bounds for the new owner of that row.
    """
    result = []
    for owner in current:
        for row in history:
            if (row['operationId'], row.get('artifactId')) == (owner['operationId'], owner.get('artifactId')):
                result.append({**row, 'rowIndex': owner['rowIndex'], 'activeCount': owner['activeCount']})
    return result


def concurrent_admission(checked, records):
    running = [row for row in records if row.get('state') == 'running' and row['phase'] != 'queued']
    return len(running) >= 2 and len([phase for phase in checked['phases'] if phase != 'queued']) >= 2

def check_build_manifests(manifests, expected_commit):
    require(manifests and all(manifest.get('gitCommit') == expected_commit for manifest in manifests),
            'App/CLI do not have the expected source commit')
    require(all(manifest == manifests[0] for manifest in manifests), 'App/CLI embedded build manifests differ')
    require(all(manifest.get('dirty') is False or
                (manifest.get('dirty') is None and manifest.get('sourceProvenance') == 'supplied')
                for manifest in manifests), 'Dirty or unknown-source app/CLI identity')


def private_settings(path, profile_id, provider, marker):
    patch = lab.read_json(path)
    require(isinstance(patch, dict), '--settings must be a nonsecret settings patch')
    profiles = patch.get('profiles', [])
    require(isinstance(profiles, list) and len(profiles) == 1,
            'Replay settings must contain exactly one isolated profile')
    selected = [p for p in profiles if p.get('id') == profile_id and p.get('provider') == provider]
    require(len(selected) == 1, 'Selected provider/profile absent or not unique in settings patch')
    profile = selected[0]
    credential = profile.get('credentials', {})
    require(set(credential) == {'mode', 'reference'} and credential['mode'] == 'keychain'
            and isinstance(credential['reference'], str) and credential['reference'],
            'The scenario requires a pre-provisioned keychain reference')
    # Only provider-neutral settings and the selected profile are used. Reject
    # credential values before serializing a patch into any guest command.
    forbidden = {'password', 'accesskey', 'accesskeyid', 'secretkey', 'secretaccesskey',
                 'sessiontoken', 'accesstoken', 'refreshtoken', 'userkeypassword',
                 'cachepassword', 'snapshot', 'token', 'authorization', 'session'}
    def contains_secret(value):
        if isinstance(value, dict):
            names = {re.sub('[^a-z]', '', str(key).lower()) for key in value}
            return bool(names & forbidden) or any(contains_secret(child) for child in value.values())
        if isinstance(value, list):
            return any(contains_secret(child) for child in value)
        if isinstance(value, str) and value.casefold().startswith(('http://', 'https://')):
            try:
                url = urlsplit(value)
                return bool(url.username is not None or url.password is not None or url.query or url.fragment)
            except ValueError:
                return True
        return False
    require(not contains_secret(patch), 'Settings contain a credential value field')
    settings = profile.get('settings', {})
    scope = settings.get('objectPrefix' if provider == 's3' else 'folderName', '')
    root_only = provider == 's3' and scope == ''
    if root_only:
        naming = patch.get('naming', {})
        template = marker + '-{uuid}.{ext}'
        require(marker and naming.get('mode') == 'template' and naming.get('collisionPolicy') == 'rename'
                and all(naming.get(key) == template for key in
                        ('fileTemplate', 'captureTemplate', 'imageTemplate', 'textTemplate')),
                'Root-only S3 needs exact isolated UUID templates for every source kind')
    else:
        require(marker and marker in scope, 'Selected destination lacks the operator-provisioned unique isolation marker')
    require(provider != 'proton-drive' or settings.get('folderUid'), 'Proton isolated folder UID missing')
    return patch



def native_display_prefix():
    return lab.native_display_prefix()


class Replay:
    # setup() replaces this with the set for the panel the resident sees.
    tints = TRAY_TINTS['dark']
    # A runner-created failure the tray may latch (expect_attention).
    attention = None
    # The lab profile; its optional tray_menu entry calibrates the visible menu path.
    profile = {}

    def __init__(self, args):
        self.args = args
        self.run = Path(args.run).resolve(strict=True)
        self.output = Path(args.output).resolve()
        self.output.mkdir(mode=0o700, exist_ok=False)
        os.chmod(self.output, 0o700)
        self.info = lab.state(self.run)
        self.profile = lab.load_profile(self.info['profile'])
        self.token = uuid.uuid4().hex[:12]
        self.guest = '/tmp/tp-' + self.token
        self.ids = []
        self.pid = None
        self.history = []
        self.mismatch_since = None
        self.attention = None
        self.witness_number = 0
        self.result = {'schemaVersion': 1, 'protocolPassed': False,
                       'graphicalAcceptance': 'pending-human-inspection',
                       'profile': self.info['profile'], 'provider': args.provider,
                       'operationIds': self.ids, 'guestRoot': self.guest,
                       'vncPort': 5900 + self.info['vnc_display'],
                       'coverage': {}, 'witnesses': [],
                       'remoteCleanup': 'not-performed; exact preparation only; operator owns acknowledgement'}
        self.save('parameters.json', {'expectedCommit': args.expected_commit,
            'expectedPackageSha256': args.expected_package_sha256,
            'expectedProbeSha256': args.expected_probe_sha256,
            'profileId': args.profile_id, 'isolationMarker': args.isolation_marker,
            'payloadMiBPerFile': args.payload_mib, 'fileCount': 2,
            'trayPosition': [args.tray_x, args.tray_y], 'timeoutSeconds': args.timeout,
            'guestRoot': self.guest, 'controllerSha256': lab.hash_file(__file__),
            'arguments': vars(args)})

    def save(self, name, value):
        lab.write_json(self.output / name, value)

    def session(self, body):
        # Preserve a relative Wayland socket before switching the app's private
        # runtime root. The real desktop's bus and display remain in use.
        # lab.guest_session also exports the session's XDG_CONFIG_DIRS and
        # XDG_DATA_DIRS, which PanelTone reads (#229, #243).
        prefix = native_display_prefix()
        prefix += '\n'.join('export ' + name + '=' + shlex.quote(self.guest + suffix) for name, suffix in
                           [('XDG_CONFIG_HOME', '/c'), ('XDG_DATA_HOME', '/d'), ('XDG_RUNTIME_DIR', '/r')]) + '\n'
        return lab.guest_session(self.run, prefix + body)

    def cli(self, method, params=None, expected_error=None):
        value = json.loads(self.session('/usr/bin/putgd-cli ' + shlex.quote(method) + ' ' +
                                        shlex.quote(json.dumps(params or {})) + ' || test "$?" -eq 1\n'))
        if expected_error:
            require(not value.get('ok') and value.get('error', {}).get('code') == expected_error,
                    'Expected production refusal was not observed: ' + expected_error)
            return {'errorCode': expected_error}
        require(value.get('ok'), 'Production CLI failed: ' + value.get('error', {}).get('code', 'unknown'))
        return value['result']

    def patch(self, patch):
        before = self.cli('settings.get')
        self.cli('settings.patch', {'expectedRevision': before['metadata']['observedRevision'], 'patch': patch})
        after = self.cli('settings.get')
        require(after['metadata']['validation']['ok'], 'Settings patch not accepted')
        self.result['acceptedSettingsRevision'] = after['metadata']['acceptedRevision']

    def activate_credentials(self):
        if self.args.provider != 's3':
            return
        metadata = self.cli('settings.get')['metadata']
        accepted = self.cli('setup.unlockCredentials', {'profileId': self.args.profile_id,
                             'expectedRevision': metadata['observedRevision']})
        setup_id = accepted['setupId']
        self.result['credentialSetupId'] = setup_id
        self.save('results.json', self.result)
        def finished():
            job = self.cli('setup.inspect', {'id': setup_id})
            return job if job['state'] != 'running' else None
        job = self.wait(finished, 'Explicit S3 credential unlock did not settle')
        require(job['state'] == 'completed', 'Explicit S3 credential unlock failed: ' +
                job.get('error', {}).get('code', 'unknown'))

    def get_json(self, remote):
        return json.loads(lab.ssh(self.run, 'cat -- ' + shlex.quote(remote) + '\n'))

    def wait(self, predicate, message):
        deadline = time.monotonic() + self.args.timeout
        while time.monotonic() < deadline:
            value = predicate()
            if value:
                return value
            time.sleep(.1)
        raise TrayProgressError(message)

    def witness(self, label, hover=True):
        self.witness_number += 1
        if hover:
            lab.pointer(self.run, self.args.tray_x, self.args.tray_y, self.width, self.height)
            time.sleep(.8)
        pictures = []
        for head in range(self.profile.get('display_heads', 1)):
            value = lab.screenshot(self.run, f'tp-{self.token}-{self.witness_number}-{label}', head)
            name = f'{self.witness_number:03}-{label}-head{head}.ppm'
            shutil.copyfile(value['path'], self.output / name)
            pictures.append({'file': name, 'sha256': lab.hash_file(self.output / name), 'head': head})
        self.result['witnesses'].append({'label': label, 'hover': hover,
            'rendering': 'requires-human-inspection', 'pictures': pictures})
        self.save('results.json', self.result)
        return self.output / pictures[0]['file']

    def snapshot(self):
        value = json.loads(self.session(shlex.quote(self.args.probe) + ' --pid ' + str(self.pid) + ' || test "$?" -eq 1\n'))
        require(value.get('ok'), 'Real tray probe failed: ' + value.get('error', 'unknown'))
        return value

    def read_observation(self):
        # One SSH session preserves independent ordered CLI/SNI/CLI reads
        # without three network round trips between them.
        commands = [['/usr/bin/putgd-cli', 'operation.recent', '{}'],
                    [self.args.probe, '--pid', str(self.pid)],
                    ['/usr/bin/putgd-cli', 'operation.recent', '{}']]
        script = "import json,subprocess\nrows=[]\n"
        script += 'for command in ' + repr(commands) + ':\n'
        script += " result=subprocess.run(command,capture_output=True,text=True,timeout=5)\n"
        script += " rows.append(json.loads(result.stdout))\n"
        script += "print(json.dumps(rows))\n"
        before, snapshot, after = json.loads(self.session('python3 -c ' + shlex.quote(script) + '\n'))
        require(before.get('ok') and after.get('ok'), 'Bracketed production CLI read failed')
        require(snapshot.get('ok'), 'Real tray probe failed: ' + snapshot.get('error', 'unknown'))
        return before['result']['operations'], snapshot, after['result']['operations']

    def read_panel_tone(self):
        """Pick the tint set from the panel tone the resident's environment gives.

        The lab's private XDG_CONFIG_HOME is part of that environment, so this
        can differ from the desktop's own panel. put.gd reports its own reading
        (ui.snapshot diagnostics.trayTint, #60); both must agree before any
        colour check. A build without that report is checked on the lab's read alone.
        """
        inputs = json.loads(self.session("python3 - <<'PY'\n" + PANEL_READER + 'PY\n'))
        light, source = panel_tone(inputs)
        tone = {'panel': 'light' if light else 'dark', 'source': source,
                'desktop': inputs.get('desktop', ''), 'plasmaStyle': inputs.get('plasmaStyle'),
                'cinnamonTheme': inputs.get('cinnamonTheme')}
        last = {}
        def agreed():
            last['reported'] = self.cli('ui.snapshot').get('diagnostics', {}).get('trayTint')
            reported = last['reported']
            if reported is None or (reported.get('panel'), reported.get('source')) == (tone['panel'], tone['source']):
                return {'reported': reported}
            return None
        # PanelTone settles asynchronously (gsettings on Cinnamon, a debounced rescan on Plasma).
        deadline = time.monotonic() + self.args.timeout
        while not (found := agreed()):
            require(time.monotonic() < deadline, 'put.gd reports a ' + json.dumps(last['reported']) +
                    ' tray panel; the lab reads ' + json.dumps(tone))
            time.sleep(.1)
        reported = found['reported']
        self.tints = TRAY_TINTS[tone['panel']]
        self.result['panelTone'] = {**tone, 'reportedByApp': reported, 'tints': self.tints}
        self.save('panel-tone.json', self.result['panelTone'])
        return self.result['panelTone']

    def expect_attention(self, ids, kind, required=False):
        # Call only for a runner-created failure or the bounded original-ID
        # Proton busy scenario. A declaration alone never authorizes red. A
        # retry of a still-latched failure keeps the original latch.
        if self.attention and self.attention['kind'] == kind and 'clearedAt' not in self.attention:
            self.attention['operationIds'] = sorted(set(self.attention['operationIds']) | set(ids))
            self.attention['required'] = self.attention['required'] or required
            return
        self.attention = {'operationIds': list(ids), 'kind': kind, 'labels': list(ATTENTION_LABELS[kind]),
                          'required': required}

    def attention_labels(self, operations, now):
        """The labels the tray may show, once the runner's failure is independently observed."""
        expected = self.attention
        if not expected or 'clearedAt' in expected:
            return None
        failed = [op for op in operations if op['id'] in expected['operationIds']
                  and op['state'] in {'failed', 'partial', 'interrupted'}]
        for op in failed:
            items = op.get('items', [])
            if expected['kind'] == 'local-save':
                valid = items and all(item.get('uploadStatus') == 'local_only' and
                    item.get('local', {}).get('status') == 'failed' for item in items)
            else:
                valid = expected['kind'] == 'proton-busy' and self.args.provider == 'proton-drive' and items and all(
                    (item.get('uploadStatus') == 'uploaded' and item.get('shareStatus') == 'shared') or
                    (item.get('uploadStatus') != 'unknown' and item.get('shareStatus') != 'unknown' and
                     item.get('error', {}).get('code') == 'proton_account_busy') for item in items) and any(
                    item.get('error', {}).get('code') == 'proton_account_busy' for item in items)
            require(valid, 'Expected error does not match independently observed failed operation')
        if failed:
            expected.setdefault('failureObservedAt', now)
            expected['failedOperationIds'] = sorted(set(expected.get('failedOperationIds', [])) | {op['id'] for op in failed})
        return expected['labels'] if 'failureObservedAt' in expected else None

    def attention_evidence(self, checked, now, label):
        expected = self.attention
        if not expected or 'clearedAt' in expected:
            return
        if checked.get('attention'):
            if 'attentionObservedAt' not in expected:
                expected['attentionObservedAt'] = now
                expected['attentionLabel'] = checked['attention']
                self.result.setdefault('attention', []).append(expected)
                self.witness(label + '-attention', hover=False)
            expected['lastAttentionAt'] = now
        elif 'attentionObservedAt' in expected:
            # Only opening the attention item clears a local failure; a Proton
            # busy latch also clears once its retried items have uploaded.
            require('acknowledgedAt' in expected or expected['kind'] == 'proton-busy',
                    'Tray cleared a latched failure before its attention item was opened')
            expected['clearedAt'] = now

    def tray_menu(self, snapshot, label=None):
        """The owned SNI's visible menu labels; with label, open the one item that matches."""
        script = TRAY_MENU.replace('SERVICE', repr(snapshot['service'])).replace('PATH', repr(snapshot['path'])) \
                          .replace('LABEL', repr(label))
        value = json.loads(self.session("python3 - <<'PY'\n" + script + 'PY\n'))
        require(value.get('ok'), 'Tray menu read failed: ' + value.get('error', 'unknown'))
        return value

    def menu_monitor(self, service):
        """Record what the desktop's tray sends put.gd's menu; returns (pid, file, unique owner).

        The SNI may be registered under a well-known name, while the tray
        addresses its menu calls to the resident's unique connection name.
        """
        path = f'{self.guest}/menu-monitor-{uuid.uuid4().hex[:8]}.jsonl'
        output = self.session('addr="${DBUS_SESSION_BUS_ADDRESS:-unix:path=/run/user/$(id -u)/bus}"\n'
                              'busctl --address="$addr" --json=short call org.freedesktop.DBus /org/freedesktop/DBus '
                              'org.freedesktop.DBus GetNameOwner s ' + shlex.quote(service) + '\n'
                              '(setsid stdbuf -oL busctl --address="$addr" --json=short monitor ' + shlex.quote(service) +
                              ' > ' + shlex.quote(path) + ' 2>/dev/null < /dev/null & echo $!)\n')
        lines = output.strip().splitlines()
        owner = json.loads(lines[0])['data'][0]
        require(isinstance(owner, str) and owner.startswith(':'), 'The tray item has no unique bus owner')
        time.sleep(.5)
        return int(lines[-1]), path, owner

    def menu_monitor_events(self, monitor):
        text = self.session('cat -- ' + shlex.quote(monitor[1]) + ' 2>/dev/null || true\n')
        return menu_events(text.splitlines(), monitor[2])

    def stop_menu_monitor(self, monitor):
        # Only the recorded monitor: its stdout must still be this run's file.
        pid, path, _ = monitor
        self.session(f'[ "$(readlink /proc/{pid}/fd/1 2>/dev/null)" = {shlex.quote(path)} ] && kill {pid} || true\n')

    def open_visibly(self, snapshot, item, label):
        """Right-click the real tray icon, reach item with the keyboard, press Enter.

        The desktop's own tray must open put.gd's menu and send "clicked" for
        exactly that item. Returns the attempt; raises VisibleMenuFailed when
        the desktop did not, after Escape closes whatever opened.
        """
        settings = {'button': 'right', 'openSeconds': .8, 'firstRowSelected': False,
                    **self.profile.get('tray_menu', {})}
        attempt = {'method': 'visible-menu', 'settings': settings, 'calibrated': 'tray_menu' in self.profile}
        self.result.setdefault('trayMenu', []).append(attempt)
        row, presses = menu_presses(self.tray_menu(snapshot)['rows'], item, settings['firstRowSelected'])
        if row is None:
            attempt['status'] = 'failed'
            attempt['reason'] = 'attention item is not a top-level menu row'
            raise VisibleMenuFailed(attempt['reason'])
        attempt.update(target=row, downPresses=presses)
        monitor = self.menu_monitor(snapshot['service'])
        attempt['busOwner'] = monitor[2]
        try:
            x, y = self.args.tray_x, self.args.tray_y
            before = self.witness(label + '-menu-before', hover=False)
            found = locate_tinted_icon(before, self.tints['error'], (x, y))
            if found is None:
                raise VisibleMenuFailed('the latched tray icon is not near the supplied position')
            x, y = attempt['iconAt'] = found
            lab.pointer(self.run, x, y, self.width, self.height)
            time.sleep(.3)
            lab.pointer(self.run, x, y, self.width, self.height, True, settings['button'])
            time.sleep(.05)
            lab.pointer(self.run, x, y, self.width, self.height, False, settings['button'])
            time.sleep(settings['openSeconds'])
            self.witness(label + '-menu-open', hover=False)
            events = self.menu_monitor_events(monitor)
            if not any(kind == 'show' or (kind == 'event' and name == 'opened') for kind, _, name in events):
                raise VisibleMenuFailed('the desktop did not open put.gd\'s menu')
            for _ in range(presses):
                lab.key(self.run, ['down'])
                time.sleep(.15)
            time.sleep(.3)
            self.witness(label + '-menu-highlight', hover=False)
            hovered = [ident for kind, ident, name in self.menu_monitor_events(monitor)
                       if kind == 'event' and name == 'hovered']
            attempt['hoverReported'] = bool(hovered)
            if hovered and hovered[-1] != row['id']:
                raise VisibleMenuFailed('the keyboard highlight is not on the attention row')
            lab.key(self.run, ['ret'])
            deadline = time.monotonic() + 5
            while True:
                clicked = [ident for kind, ident, name in self.menu_monitor_events(monitor)
                           if kind == 'event' and name == 'clicked']
                if clicked:
                    break
                if time.monotonic() >= deadline:
                    raise VisibleMenuFailed('no click from the desktop reached put.gd')
                time.sleep(.2)
            # Another item may have started a capture or quit the resident.
            require(clicked == [row['id']], 'The visible tray menu activated another item: ' + json.dumps(clicked))
            attempt.update(status='passed', label=row['label'])
            return attempt
        except VisibleMenuFailed as error:
            attempt.update(status='failed', reason=str(error))
            lab.key(self.run, ['esc'])
            time.sleep(.3)
            self.witness(label + '-menu-failed', hover=False)
            raise
        finally:
            self.stop_menu_monitor(monitor)

    def acknowledge_attention(self, label):
        """Hold a runner-created latch for LATCH_SECONDS, then open it from the real tray menu."""
        expected = self.attention
        def latched():
            _, snapshot, checked = self.observe(label)
            return snapshot if checked['matched'] and checked.get('attention') else None
        snapshot = self.wait(latched, 'Tray did not latch the expected failure: ' + label)
        while time.monotonic() - expected['attentionObservedAt'] < LATCH_SECONDS:
            time.sleep(.25)
            _, snapshot, checked = self.observe(label)
            require(checked['matched'] and checked.get('attention'), 'Latched failure left the tray: ' + label)
        self.witness(label + '-attention-latched')
        menu = self.tray_menu(snapshot)
        item = expected['attentionLabel']
        require(any(entry == item or re.fullmatch(re.escape(item) + r' \([0-9]+\)', entry) for entry in menu['labels']),
                'Tray menu does not offer the attention item: ' + item)
        try:
            opened = self.open_visibly(snapshot, item, label)
            expected['acknowledgedAt'] = time.monotonic()
            expected['menuLabel'], expected['openedBy'] = opened['label'], 'visible-menu'
        except VisibleMenuFailed as error:
            # The run continues, but never as a visible pass: the summary
            # reports the desktop's failure for investigation.
            self.result.setdefault('visibleMenuFallbacks', []).append({'scenario': label, 'reason': str(error)})
            opened = self.tray_menu(snapshot, item)
            require(opened.get('clicked') == 1, 'Tray attention item was not opened exactly once')
            expected['acknowledgedAt'] = time.monotonic()
            expected['menuLabel'], expected['openedBy'] = opened['label'], 'dbus-event-fallback'
        self.witness(label + '-attention-opened', hover=False)

    def observe(self, label):
        before, snapshot, after = self.read_observation()
        now = time.monotonic()
        before_rows, after_rows = records_from(before), records_from(after)
        self.history.append((now, before_rows + after_rows))
        self.history = [(stamp, rows) for stamp, rows in self.history if now - stamp <= 2]
        recent = correlated_records([row for _, rows in self.history for row in rows], after_rows)
        try:
            labels = self.attention_labels(before + after, now)
            # The resident latches on its own result; a CLI read bracketing that
            # moment may not show the failure yet. Only an expected failure gets
            # this batching grace.
            observed(labels or snapshot.get('title') != ATTENTION or not self.attention
                     or 'clearedAt' in self.attention, 'Attention shown before its failure is observed')
            checked = {**check_tooltip(snapshot, recent, self.args.provider, self.pid,
                                      attention=labels, tints=self.tints), 'matched': True}
            self.attention_evidence(checked, now, label)
            self.mismatch_since = None
        except ObservationMismatch as error:
            # Separate DBus/CLI reads may straddle a queued-row, file or phase
            # transition. Preserve the sample, grant no coverage, and bound the
            # entire consecutive mismatch period; persistent stale/idle fails.
            checked = {'idle': False, 'counter': False, 'phases': [], 'matched': False, 'error': str(error)}
            if self.mismatch_since is None:
                self.mismatch_since = now
        except TrayProgressError as error:
            # A privacy/identity/protocol violation is fatal, never a batching
            # race. Retain only bounded redacted evidence, not rejected text.
            with (self.output / 'observations.jsonl').open('a') as stream:
                stream.write(json.dumps({'monotonicSeconds': now, 'scenario': label,
                    'sni': {'redacted': True}, 'cliBefore': before_rows, 'cliAfter': after_rows,
                    'assertions': {'matched': False, 'fatal': True, 'error': str(error)}}) + '\n')
            raise
        entry = {'monotonicSeconds': now, 'scenario': label, 'sni': snapshot,
                 'cliBefore': before_rows, 'cliAfter': after_rows, 'assertions': checked}
        with (self.output / 'observations.jsonl').open('a') as stream:
            stream.write(json.dumps(entry) + '\n')
        if not checked['matched']:
            require(now - self.mismatch_since <= 2, 'Tray mismatch persisted beyond batching grace: ' + checked['error'])
            return after, snapshot, checked
        flags = self.result['coverage'].setdefault(label, {'active': False, 'counter': False,
                  'storageActive': False, 'queued': False, 'concurrentAdmission': False, 'finalizing': False, 'sharing': False})
        new = []
        for key, value in [('active', not checked['idle'] and 'attention' not in checked), ('counter', checked['counter']),
                           ('storageActive', checked['storageActive']),
                           ('queued', 'queued' in checked['phases']),
                           ('concurrentAdmission', concurrent_admission(checked, after_rows)),
                           ('finalizing', 'finalizing' in checked['phases']), ('sharing', 'sharing' in checked['phases'])]:
            if value and not flags[key]:
                flags[key] = True
                new.append(key)
        if new:
            self.witness(label + '-' + '-'.join(new))
        self.save('results.json', self.result)
        return after, snapshot, checked

    def operations(self, ids, label):
        current, _, _ = self.observe(label)
        found = {op['id']: op for op in current if op['id'] in ids}
        for operation_id in ids:
            if operation_id not in found:
                found[operation_id] = self.cli('operation.inspect', {'id': operation_id})
        return found

    def settled(self, ids, label):
        def terminal():
            found = self.operations(ids, label)
            if len(found) == len(ids) and all(op['state'] in TERMINAL and
                    op.get('copyStatus') in COPY_SETTLED for op in found.values()):
                return found
        found = self.wait(terminal, 'Operations did not settle: ' + label)
        expected = self.attention
        require(not expected or not expected['required'] or 'failureObservedAt' in expected,
                'Expected failed operation was not independently observed')
        self.save(label + '-outcomes.json', {key: {
            'state': op['state'], 'copyStatus': op['copyStatus'], 'items': [{
                'historyId': item['artifact']['id'], 'sourceBytes': item['artifact']['bytes'],
                'sha256': item['artifact']['sha256'], 'uploadStatus': item['uploadStatus'],
                'localStatus': item['local']['status'], 'shareStatus': item.get('shareStatus'),
                'errorCode': item.get('error', {}).get('code')}
                for item in op['items']]} for key, op in found.items()})
        if expected and expected['required'] and 'acknowledgedAt' not in expected:
            self.acknowledge_attention(label)
        elif expected and expected['kind'] == 'proton-busy' and 'attentionObservedAt' in expected \
                and 'clearedAt' not in expected:
            # Busy failures stay latched until their retried items upload.
            return found
        self.returned_to_idle(label)
        return found

    def returned_to_idle(self, label):
        def idle():
            _, snapshot, checked = self.observe(label)
            return snapshot if checked['matched'] and checked['idle'] else None
        snapshot = self.wait(idle, 'Tray did not return to idle: ' + label)
        require([icon['sha256'] for icon in snapshot['icons']] == self.idle_hashes,
                'Settled tray did not restore the exact original idle icon')
        require(not self.attention or 'attentionObservedAt' not in self.attention or 'clearedAt' in self.attention,
                'Idle tray while a failure is still latched')
        self.witness(label + '-idle')
        self.attention = None

    def clipboard(self):
        image = self.witness('source-fixture', hover=False)
        paint = lab.locate_fixture_paint(image)
        require(paint, 'Independent native synthetic clipboard fixture is not visibly unobscured')
        # Let the visible guest process the move before pressing; a combined
        # tablet move/button packet can click the previous tray position.
        lab.pointer(self.run, paint['x'] + 100, paint['y'] + 100, self.width, self.height)
        time.sleep(.15)
        lab.pointer(self.run, paint['x'] + 100, paint['y'] + 100, self.width, self.height, True)
        time.sleep(.05)
        lab.pointer(self.run, paint['x'] + 100, paint['y'] + 100, self.width, self.height, False)
        lab.ssh(self.run, 'rm -f -- ' + shlex.quote(self.guest + '/f/source.json') + '\n')
        lab.key(self.run, ['ctrl', 'alt', 'shift', 'f'])
        def acknowledged():
            try:
                value = self.get_json(self.guest + '/f/source.json')
                return value if value.get('synthetic') and value.get('key') == 70 else None
            except lab.LabError:
                return None
        self.wait(acknowledged, 'Native guest clipboard fixture did not acknowledge synthetic files')

    def invoke(self):
        accepted = self.cli('action.invoke', {'action': 'clipboard'})
        operation_id = accepted['operationId']
        self.ids.append(operation_id)
        self.save('results.json', self.result)
        return operation_id

    def setup(self):
        require(self.info['profile'] in ('cinnamon-x11', 'gnome-wayland', 'kde-wayland', 'fedora-gnome-wayland'),
                'Unsupported native desktop; no substitute desktop evidence')
        patch = private_settings(self.args.settings, self.args.profile_id, self.args.provider, self.args.isolation_marker)
        package = lab.read_json(self.run / 'package.json')
        require(package.get('sha256') == self.args.expected_package_sha256, 'Package receipt differs from requested package')
        binaries = package.get('installedBinarySha256', {})
        require(all(path in binaries for path in ('/usr/bin/putgd', '/usr/bin/putgd-cli', '/usr/bin/putgd-desktop-fixture')),
                'Package receipt must include app, CLI and native fixture hashes')
        require(all(Path(path).is_absolute() and re.fullmatch(r'[0-9a-f]{64}', digest) for path, digest in binaries.items()),
                'Package binary receipt malformed')
        hashes = lab.ssh(self.run, 'sha256sum -- ' + ' '.join(shlex.quote(path) for path in binaries) + '\n')
        actual = {path: digest for digest, path in (line.split(maxsplit=1) for line in hashes.splitlines())}
        require(actual == binaries, 'Installed bytes differ from the package receipt')
        probe_hash = lab.ssh(self.run, 'sha256sum -- ' + shlex.quote(self.args.probe) + '\n').split()[0]
        require(probe_hash == self.args.expected_probe_sha256, 'Installed probe hash mismatch')
        manifests = [json.loads(lab.ssh(self.run, path + ' --version-json\n'))
                     for path in ('/usr/bin/putgd', '/usr/bin/putgd-cli')]
        check_build_manifests(manifests, self.args.expected_commit)
        native = json.loads(lab.guest_session(self.run, native_display_prefix() + '''python3 - <<'PY'
import json, os, urllib.parse
paths = []
wayland = os.environ.get('WAYLAND_DISPLAY', '')
if wayland:
    paths.append(wayland if wayland.startswith('/') else os.path.join(os.environ['XDG_RUNTIME_DIR'], wayland))
for address in os.environ.get('DBUS_SESSION_BUS_ADDRESS', '').split(';'):
    if address.startswith('unix:'):
        for field in address[5:].split(','):
            if field.startswith('path='):
                paths.append(urllib.parse.unquote(field[5:]))
print(json.dumps({'paths': [os.path.realpath(path) for path in paths]}))
PY
'''))
        resolved = lab.ssh(self.run, 'python3 -c ' + shlex.quote('import os; print(os.path.realpath(' + repr(self.guest) + '))') + '\n').strip()
        checks = socket_paths(resolved, self.run / 'qmp.sock', native['paths'])
        self.save('socket-preflight.json', {'limitBytesExcludingNul': 107, 'paths': checks})
        require(resolved == self.guest, 'Guest private runtime root unexpectedly resolves elsewhere')
        require(not lab.ssh(self.run, 'pgrep -u "$(id -u)" -x putgd || true\n').strip(),
                'Existing lab resident must be shut down explicitly before this isolated scenario')
        lab.ssh(self.run, 'umask 077\nmkdir -m 700 -- ' + shlex.quote(self.guest) + '\nmkdir -- ' +
                ' '.join(shlex.quote(self.guest + suffix) for suffix in ('/c', '/d', '/r', '/f', '/archive')) + '\n')
        self.session('nohup /usr/bin/putgd --background > ' + shlex.quote(self.guest + '/app.log') +
                     ' 2>&1 < /dev/null &\n')
        def ready():
            try:
                value = self.cli('diagnostics')
                return value if value.get('journalReady') else None
            except (lab.LabError, TrayProgressError):
                return None
        diagnostics = self.wait(ready, 'Isolated resident did not become ready')
        self.patch({**patch, 'actions': {name: {'destination': 'upload', 'profileId': self.args.profile_id,
                                             'shortcut': '', 'publicationMode': 'share'}
                    for name in ('region', 'window', 'desktop', 'clipboard')},
                    'archive': {'root': self.guest + '/archive', 'keepUploads': False,
                                'folders': {'images': None, 'videos': None, 'text': None, 'other': None}},
                    'naming': patch['naming'] if self.args.provider == 's3' and
                              patch['profiles'][0]['settings'].get('objectPrefix') == '' else
                              {'mode': 'generated', 'collisionPolicy': 'rename'},
                    'limits': {'concurrentUploads': 1}, 'retention': {'recentActivityLimit': 100}, 'preferences': {'notifications': False, 'launchAtLogin': False}})
        self.activate_credentials()
        self.read_panel_tone()
        identity = lab.verify_running_app(self.run)
        self.pid = identity['pid']
        self.result['runningApp'] = identity
        self.result['build'] = manifests[0]
        self.save('package-identity.json', package)
        self.save('initial-diagnostics.json', diagnostics)
        self.save('capabilities.json', self.cli('capabilities'))
        self.session('nohup /usr/bin/putgd-desktop-fixture --output ' + shlex.quote(self.guest + '/f') +
                     ' > ' + shlex.quote(self.guest + '/fixture.log') + ' 2>&1 < /dev/null &\n')
        def fixture_ready():
            try:
                return self.get_json(self.guest + '/f/ready.json')
            except lab.LabError:
                return None
        ready = self.wait(fixture_ready, 'Native fixture did not become ready')
        self.result.update(lab.validate_readiness(self.profile, ready, self.info['acceleration']))
        self.save('native-session.json', ready)
        layout = ready['display_layout']
        require(len(layout) == 1, 'This replay requires the provisioned single-head native profile')
        self.width, self.height = layout[0]['nativeWidth'], layout[0]['nativeHeight']
        require(0 <= self.args.tray_x < self.width and 0 <= self.args.tray_y < self.height, 'Tray hover lies outside native head')
        generate = """import hashlib,json,pathlib
root=pathlib.Path(ROOT)
chunk=bytes(range(256))*4096
results=[]
for number in (1,2):
    path=root/('synthetic-%d.txt'%number)
    digest=hashlib.sha256()
    with path.open('wb') as stream:
        for _ in range(MIB):
            stream.write(chunk);digest.update(chunk)
    results.append({'bytes':MIB*1048576,'sha256':digest.hexdigest()})
print(json.dumps(results))
""".replace('ROOT', repr(self.guest + '/f')).replace('MIB', str(self.args.payload_mib))
        self.sources = json.loads(lab.ssh(self.run, 'python3 - <<\'PY\'\n' + generate + 'PY\n'))
        self.save('synthetic-source-identities.json', self.sources)
        idle = self.snapshot()
        require(check_tooltip(idle, [], self.args.provider, self.pid, tints=self.tints)['idle'], 'Initial tray is not idle')
        self.idle_hashes = [icon['sha256'] for icon in idle['icons']]
        self.save('initial-tray.json', idle)
        self.witness('initial-idle')

    def successful_uploads(self, found):
        for op in found.values():
            require(op['state'] == 'completed' and len(op['items']) == 2 and
                    all(item['uploadStatus'] == 'uploaded' for item in op['items']), 'Synthetic upload did not complete')
            require([{'bytes': item['artifact']['bytes'], 'sha256': item['artifact']['sha256']} for item in op['items']] == self.sources,
                    'Acquired upload artifacts differ from the native synthetic file source')
            require(all(item.get('receipt', {}).get('directUrl', '').startswith(('http://', 'https://'))
                        for item in op['items']), 'Completed shared upload has no confirmed link')

    def independent_paste(self, operation, label):
        self.cli('operation.copy', {'id': operation['id']})
        picture = self.witness(label + '-paste-target', hover=False)
        paint = lab.locate_fixture_paint(picture)
        require(paint, 'Independent native paste fixture is not visibly unobscured')
        # Let the visible guest process the move before pressing; a combined
        # tablet move/button packet can click the previous tray position.
        lab.pointer(self.run, paint['x'] + 100, paint['y'] + 100, self.width, self.height)
        time.sleep(.15)
        lab.pointer(self.run, paint['x'] + 100, paint['y'] + 100, self.width, self.height, True)
        time.sleep(.05)
        lab.pointer(self.run, paint['x'] + 100, paint['y'] + 100, self.width, self.height, False)
        lab.ssh(self.run, 'rm -f -- ' + shlex.quote(self.guest + '/f/paste.json') + '\n')
        lab.key(self.run, ['ctrl', 'v'])
        def pasted():
            try:
                return self.get_json(self.guest + '/f/paste.json')
            except lab.LabError:
                return None
        actual = self.wait(pasted, 'Independent native clipboard paste not acknowledged')
        expected = '\n'.join(item['receipt']['directUrl'] for item in operation['items'])
        require(actual.get('text') == expected, 'Independent pasted links differ from original upload receipts')
        # Link values/access fragments stay in guest memory and the app journal.
        self.save(label + '-paste.json', {'matchedOriginalReceipts': True, 'linkCount': len(operation['items'])})
        self.witness(label + '-paste-completed', hover=False)

    def s3_public_bytes(self, operation):
        # Read original URLs inside guest memory through the production CLI, so
        # no link or account data is interpolated into SSH/process arguments.
        script = """import hashlib,json,subprocess,urllib.request
reply=json.loads(subprocess.check_output(['/usr/bin/putgd-cli','operation.inspect',json.dumps({'id':OPERATION})],text=True))
assert reply['ok']
results=[]
for item in reply['result']['items']:
    expected=item['artifact']['bytes']; received=0; digest=hashlib.sha256()
    with urllib.request.urlopen(item['receipt']['directUrl'],timeout=30) as response:
        while True:
            chunk=response.read(1048576)
            if not chunk:break
            received+=len(chunk)
            assert received<=expected
            digest.update(chunk)
    assert received==expected and digest.hexdigest()==item['artifact']['sha256']
    results.append({'bytes':received,'sha256':digest.hexdigest()})
print(json.dumps(results))
""".replace('OPERATION', repr(operation['id']))
        self.save('s3-public-byte-verification.json', json.loads(self.session("python3 - <<'PY'\n" + script + 'PY\n')))

    def recover_proton_busy(self, initial):
        if self.args.provider != 'proton-drive':
            return initial
        # Same-account SDK calls are serialized by the production account lock.
        # Keep concurrent outcomes, then recover only explicit busy failures on
        # the original IDs with a single worker and the same credential reference.
        self.save('concurrent-original-outcomes-private.json', initial)
        def complete(op):
            return op['state'] == 'completed' and all(item['uploadStatus'] == 'uploaded'
                and item.get('shareStatus') == 'shared' for item in op['items'])
        def allowed(op):
            require(op['items'], 'Concurrent Proton operation has no retained artifacts')
            for item in op['items']:
                if item['uploadStatus'] == 'uploaded' and item.get('shareStatus') == 'shared':
                    continue
                require(item['uploadStatus'] != 'unknown' and item.get('shareStatus') != 'unknown'
                        and item.get('error', {}).get('code') == 'proton_account_busy',
                        'Concurrent Proton outcome requires separate recovery')
        pending = [operation_id for operation_id, op in initial.items() if not complete(op)]
        for operation_id in pending:
            allowed(initial[operation_id])
        if not pending:
            return initial
        self.patch({'limits': {'concurrentUploads': 1}})
        recovered = dict(initial)
        for attempt in range(1, 4):
            for operation_id in pending:
                accepted = self.cli('operation.retry', {'id': operation_id})
                require(accepted.get('operationId') == operation_id and accepted.get('retryAccepted'),
                        'Busy recovery did not retain original operation ID')
                self.expect_attention([operation_id], 'proton-busy')
                op = self.settled([operation_id], 'concurrent-recovery-' + str(attempt))[operation_id]
                old_items, items = initial[operation_id]['items'], op['items']
                require([item['artifact']['id'] for item in items] == [item['artifact']['id'] for item in old_items]
                        and [item.get('publicationId') for item in items] == [item.get('publicationId') for item in old_items],
                        'Busy recovery changed retained artifact/publication identity')
                require(all(old.get('receipt') == item.get('receipt') for old, item in zip(recovered[operation_id]['items'], items)
                            if old['uploadStatus'] == 'uploaded' and old.get('shareStatus') == 'shared'),
                        'Busy recovery changed an already shared publication receipt')
                recovered[operation_id] = op
                if not complete(op):
                    allowed(op)
            pending = [operation_id for operation_id, op in recovered.items() if not complete(op)]
            if not pending:
                if self.attention and 'attentionObservedAt' in self.attention and 'clearedAt' not in self.attention:
                    self.returned_to_idle('concurrent-recovered')
                return recovered
        raise TrayProgressError('Bounded original-ID Proton busy recovery did not complete')

    def execute(self):
        self.setup()
        self.clipboard()
        queue_ids = [self.invoke(), self.invoke()]
        queued = self.settled(queue_ids, 'queue')
        self.successful_uploads(queued)
        require(self.result['coverage']['queue']['active'] and self.result['coverage']['queue']['counter']
                and self.result['coverage']['queue']['queued'], 'Queued active work/counters were not observed; preserve this run')
        require(self.result['coverage']['queue']['storageActive'],
                'Queued upload storage progress fill was not observed; preserve this run')
        self.independent_paste(queued[queue_ids[-1]], 'upload')
        if self.args.provider == 's3':
            self.s3_public_bytes(queued[queue_ids[-1]])
        self.patch({'limits': {'concurrentUploads': 2}})
        self.clipboard()
        concurrent_ids = [self.invoke(), self.invoke()]
        if self.args.provider == 'proton-drive':
            self.expect_attention(concurrent_ids, 'proton-busy')
        concurrent = self.settled(concurrent_ids, 'concurrent')
        concurrent = self.recover_proton_busy(concurrent)
        self.successful_uploads(concurrent)
        require(self.result['coverage']['concurrent']['concurrentAdmission'], 'Two active admitted operations were not observed')
        self.patch({'limits': {'concurrentUploads': 1}})
        self.clipboard()
        cancelled_id = self.invoke()
        def cancellable():
            found = self.operations([cancelled_id], 'cancel')
            op = found.get(cancelled_id)
            if op and any(item.get('uploadStatus') == 'uploading' and
                          (item.get('uploadProgress') or {}).get('bytesTransferred', 0) > 0 for item in op['items']):
                return op
        original = self.wait(cancellable, 'Upload counter not observed in time to cancel')
        self.cli('operation.cancel', {'id': cancelled_id})
        cancelled = self.settled([cancelled_id], 'cancel')[cancelled_id]
        require(cancelled['state'] == 'cancelled', 'Upload did not settle as cancelled')
        accepted = self.cli('operation.retry', {'id': cancelled_id})
        require(accepted.get('operationId') == cancelled_id and accepted.get('retryAccepted'), 'Retry did not retain operation ID')
        retried = self.settled([cancelled_id], 'retry')
        self.successful_uploads(retried)
        require(self.result['coverage']['retry']['storageActive'],
                'Retry storage progress fill was not observed; preserve this run')
        require([item['artifact']['id'] for item in original['items']] ==
                [item['artifact']['id'] for item in retried[cancelled_id]['items']], 'Retry changed retained artifact identity')
        require([item['publicationId'] for item in original['items']] ==
                [item['publicationId'] for item in retried[cancelled_id]['items']], 'Retry changed original publication IDs')
        # A regular file blocks mkdir/copy deterministically, inside this new
        # private synthetic root. This never touches an existing user archive.
        blocked = self.guest + '/blocked'
        lab.ssh(self.run, 'umask 077\nprintf synthetic > ' + shlex.quote(blocked) + '\n')
        self.patch({'actions': {'clipboard': {'destination': 'local', 'profileId': None}},
                    'archive': {'root': blocked, 'keepUploads': True}})
        self.clipboard()
        local_id = self.invoke()
        self.expect_attention([local_id], 'local-save', required=True)
        failed = self.settled([local_id], 'local-failure')[local_id]
        require(failed['items'] and all(item['local']['status'] == 'failed' and item['uploadStatus'] == 'local_only'
                for item in failed['items']), 'Synthetic local-save failure was not observed')
        lab.ssh(self.run, 'rm -- ' + shlex.quote(blocked) + '\nmkdir -m 700 -- ' + shlex.quote(blocked) + '\n')
        self.cli('operation.retryLocalSave', {'id': local_id})
        local = self.settled([local_id], 'local-retry')[local_id]
        require(all(item['local']['status'] == 'saved' and item['uploadStatus'] == 'local_only' for item in local['items']),
                'Local-save retry did not retain local-only behavior')
        for index, item in enumerate(local['items']):
            target = self.output / ('local-result-' + str(index))
            require(item['local']['path'].startswith(blocked + '/'), 'Saved synthetic result escaped the owned archive')
            lab.get(self.run, item['local']['path'], target)
            require(target.stat().st_size == self.sources[index]['bytes'] and lab.hash_file(target) == self.sources[index]['sha256'],
                    'Actual saved local bytes differ from independent synthetic source')
        upload_ids = queue_ids + concurrent_ids + [cancelled_id]
        retained = [self.cli('operation.inspect', {'id': operation_id}) for operation_id in upload_ids]
        history_ids = [item['artifact']['id'] for op in retained for item in op['items']
                       if item['uploadStatus'] == 'uploaded']
        self.result['historyIds'] = history_ids
        before_restart = {history_id: self.cli('history.detail', {'id': history_id}) for history_id in history_ids}
        require(self.cli('ui.quit').get('quitting'), 'Resident quit not acknowledged')
        self.wait(lambda: not lab.ssh(self.run, 'pgrep -u "$(id -u)" -x putgd || true\n').strip(), 'Resident did not exit')
        self.session('nohup /usr/bin/putgd --background > ' + shlex.quote(self.guest + '/restart.log') + ' 2>&1 < /dev/null &\n')
        def restarted():
            try:
                value = self.cli('diagnostics')
                return value if value.get('journalReady') else None
            except (lab.LabError, TrayProgressError):
                return None
        self.wait(restarted, 'Resident restart did not become ready')
        identity = lab.verify_running_app(self.run)
        require(identity != self.result['runningApp'], 'Resident process identity did not change after restart')
        self.pid = identity['pid']
        self.result['runningAppAfterRestart'] = identity
        self.history = []
        self.mismatch_since = None
        require(all(self.cli('history.detail', {'id': history_id})['receipt'] == old['receipt']
                    for history_id, old in before_restart.items()), 'Restart changed original publication receipts')
        snapshot = self.snapshot()
        require(check_tooltip(snapshot, [], self.args.provider, self.pid, tints=self.tints)['idle'] and
                [icon['sha256'] for icon in snapshot['icons']] == self.idle_hashes, 'Restart did not restore exact idle tray state')
        self.save('restarted-tray.json', snapshot)
        self.witness('restarted-idle')
        # Preparation is read-only. Do not call delete.confirm or any sharing
        # mutation; Proton safely rejects guarded deletion and retains files.
        preparation = self.cli('delete.prepare', {'ids': history_ids},
                               expected_error='unsupported_capability' if self.args.provider == 'proton-drive' else None)
        preparation['selectedHistoryIds'] = history_ids
        self.save('cleanup-preparation-private.json', preparation)
        require(lab.verify_running_app(self.run) == identity, 'Resident identity changed during final evidence collection')
        self.result['protocolPassed'] = True
        self.result['hostRestart'] = 'not automated; native host remains unchanged; see manual current-state gate'
        self.result['transientPhases'] = {phase: 'observed' if any(flags[phase] for flags in self.result['coverage'].values())
                                        else 'not-observed' for phase in ('finalizing', 'sharing')}
        self.result['status'] = 'protocol-passed-rendering-pending'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('run', 'output', 'settings', 'profile-id', 'isolation-marker', 'expected-commit',
                 'expected-package-sha256', 'probe', 'expected-probe-sha256'):
        parser.add_argument('--' + name, required=True)
    parser.add_argument('--provider', choices=['s3', 'proton-drive'], required=True)
    parser.add_argument('--tray-x', type=int, required=True)
    parser.add_argument('--tray-y', type=int, required=True)
    parser.add_argument('--payload-mib', type=int, default=32, help='Synthetic bytes per file; preserve failed fast runs before retrying larger')
    parser.add_argument('--timeout', type=float, default=180, help='Per-stage observation deadline')
    args = parser.parse_args()
    require(args.payload_mib > 0 and args.timeout > 0, 'Payload and timeout must be positive')
    require(re.fullmatch(r'[0-9a-f]{40}', args.expected_commit), 'Expected commit must be a full SHA')
    require(Path(args.probe).is_absolute(), 'Guest probe must be an absolute path')
    replay = None
    try:
        replay = Replay(args)
        replay.execute()
        return_code = 0
    except (TrayProgressError, lab.LabError, OSError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
        if replay:
            replay.result['status'] = 'failed-preserved'
            replay.result['error'] = str(error)
        else:
            print(json.dumps({'protocolPassed': False, 'error': str(error)}))
        return_code = 1
    finally:
        if replay:
            replay.save('results.json', replay.result)
            replay.save('manifest.json', {'schemaVersion': 1, 'files': {path.name: lab.hash_file(path)
                for path in sorted(replay.output.iterdir()) if path.is_file() and path.name != 'manifest.json'}})
            print(json.dumps({'protocolPassed': replay.result['protocolPassed'],
                'graphicalAcceptance': replay.result['graphicalAcceptance'], 'output': str(replay.output),
                'operationIds': replay.ids, 'status': replay.result.get('status'), 'error': replay.result.get('error'),
                'visibleMenuFallbacks': replay.result.get('visibleMenuFallbacks', [])}))
    return return_code


if __name__ == '__main__':
    raise SystemExit(main())
