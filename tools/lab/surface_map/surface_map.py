#!/usr/bin/env python3
"""Surface map: every surface and interaction of put.gd, coloured by acceptance coverage.

  surface_map.py build --catalog DIR --coverage FILE... --output DIR [--label TEXT] [--index] [--open] [--watch]
  surface_map.py mark INTERACTION --coverage FILE --desktop ID --package deb|rpm|appimage
                 [--functional passed|failed|blocked] [--visual frames|reviewed-ok|reviewed-wrong]
                 [--receipt PATH] [--issue N] [--note TEXT]
  surface_map.py import --coverage FILE --desktop ID --package PKG RECEIPT...
  surface_map.py checklist --coverage FILE... [--desktop ID]
  surface_map.py check [--catalog DIR]

The inventory is `model.json` beside this file: desktops, surfaces, interactions
(buttons, menu items, keys, global shortcuts, CLI/MCP entries, OS events, edges
between surfaces) and how lab receipts map onto them. Pictures and hotspot
rectangles come from a `putgd-ui-catalog` output directory (`*.controls.json`);
coverage comes from campaign files written by `mark` and `import`. The page is
a single offline HTML file next to copies of the captures it shows. Nothing
here touches a desktop, a resident or a network; it reads receipts and draws.
"""
import argparse
import base64
import datetime as dt
import fnmatch
import hashlib
import html
import json
import os
from pathlib import Path
import re
import shutil
import sys
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
MODEL_PATH = HERE / 'model.json'
TEMPLATE_PATH = HERE / 'surface_map.html'

KINDS = ('button', 'menu-item', 'field', 'key', 'shortcut', 'gesture', 'state', 'cli', 'mcp', 'event', 'edge')
FUNCTIONAL = ('passed', 'failed', 'blocked')
VISUAL = ('frames', 'reviewed-ok', 'reviewed-wrong')
PACKAGES = ('deb', 'rpm', 'appimage')
# Synthetic captures putgd-ui-catalog draws only offscreen (tests/gui/ui_catalog.cpp).
OFFSCREEN_ONLY_CAPTURES = {'00-tray-upload-status'}


class ModelError(ValueError):
    pass


# ---------------------------------------------------------------- model

def load_model(path=MODEL_PATH):
    model = json.loads(Path(path).read_text(encoding='utf-8'))
    validate_model(model)
    return model


def validate_model(model):
    problems = []
    if model.get('schemaVersion') != 1:
        problems.append('schemaVersion must be 1')
    desktops = {d['id'] for d in model.get('desktops', [])}
    surfaces = {}
    for surface in model.get('surfaces', []):
        if surface['id'] in surfaces:
            problems.append('duplicate surface ' + surface['id'])
        surfaces[surface['id']] = surface
    ids = set()
    for item in model.get('interactions', []):
        iid = item.get('id', '')
        if not re.fullmatch(r'[a-z0-9]+(?:[.-][a-z0-9]+)*', iid):
            problems.append('bad interaction id %r' % iid)
        if iid in ids:
            problems.append('duplicate interaction ' + iid)
        ids.add(iid)
        if item.get('surface') not in surfaces:
            problems.append('%s: unknown surface %r' % (iid, item.get('surface')))
        if item.get('kind') not in KINDS:
            problems.append('%s: unknown kind %r' % (iid, item.get('kind')))
        if item.get('to') and item['to'] not in surfaces:
            problems.append('%s: unknown target surface %r' % (iid, item['to']))
        if not item.get('label'):
            problems.append(iid + ': missing label')
        if 'visual' in item and not isinstance(item['visual'], bool):
            problems.append(iid + ': visual must be a boolean')
        for key in item.get('keys', []):
            if not isinstance(key, str) or not key:
                problems.append(iid + ': keys must be non-empty strings')
        for selector in selectors(item):
            if not isinstance(selector, str) or not selector:
                problems.append(iid + ': control selectors must be non-empty strings')
    for source, mapping in model.get('receipts', {}).items():
        for selector, targets in mapping.items():
            for target in targets:
                if target not in ids:
                    problems.append('receipts.%s.%s: unknown interaction %s' % (source, selector, target))
    if not desktops:
        problems.append('no desktops')
    if problems:
        raise ModelError('\n'.join(problems))


def drivers_by_interaction(model):
    """interaction id -> sorted receipt sources (lab drivers) that can report it."""
    out = {}
    for source, mapping in model.get('receipts', {}).items():
        for targets in mapping.values():
            for target in targets:
                out.setdefault(target, set()).add(source)
    return {k: sorted(v) for k, v in out.items()}


def interaction_visual(item):
    """Whether a visual (frames + human review) result applies to this interaction."""
    if 'visual' in item:
        return item['visual']
    return item['kind'] not in ('cli', 'mcp')


# ------------------------------------------------------------- coverage

def load_coverage(paths):
    entries = []
    for path in paths:
        path = Path(path)
        if not path.exists():
            continue
        doc = json.loads(path.read_text(encoding='utf-8'))
        if doc.get('schemaVersion') != 1:
            raise ModelError('%s: schemaVersion must be 1' % path)
        for entry in doc.get('entries', []):
            entry = dict(entry)
            entry.setdefault('campaign', doc.get('campaign', path.stem))
            entry['_file'] = path.as_posix()
            entries.append(entry)
    return entries


def write_coverage(path, campaign, entries):
    path = Path(path)
    doc = {'schemaVersion': 1, 'campaign': campaign,
           'entries': [{k: v for k, v in e.items() if not k.startswith('_')} for e in entries]}
    tmp = path.with_suffix(path.suffix + '.tmp')
    tmp.write_text(json.dumps(doc, indent=1, sort_keys=True) + '\n', encoding='utf-8')
    os.replace(tmp, path)


def make_entry(interaction, desktop, package, functional=None, visual=None, receipt=None,
               issue=None, note=None, source='mark', at=None):
    if functional is not None and functional not in FUNCTIONAL:
        raise ModelError('functional must be one of ' + ', '.join(FUNCTIONAL))
    if visual is not None and visual not in VISUAL:
        raise ModelError('visual must be one of ' + ', '.join(VISUAL))
    if package not in PACKAGES:
        raise ModelError('package must be one of ' + ', '.join(PACKAGES))
    entry = {'interaction': interaction, 'desktop': desktop, 'package': package, 'source': source,
             'at': at or dt.datetime.now(dt.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')}
    for key, value in (('functional', functional), ('visual', visual), ('receipt', receipt),
                       ('issue', issue), ('note', note)):
        if value not in (None, ''):
            entry[key] = value
    return entry


def latest_per_cell(entries):
    """interaction -> desktop -> {functional, visual, receipts[], issues[]} from the newest entries."""
    cells = {}
    for entry in sorted(entries, key=lambda e: e.get('at', '')):
        cell = cells.setdefault(entry['interaction'], {}).setdefault(entry['desktop'], {
            'functional': None, 'visual': None, 'packages': set(), 'receipts': [], 'issues': [], 'notes': []})
        if entry.get('functional'):
            cell['functional'] = entry['functional']
        if entry.get('visual'):
            cell['visual'] = entry['visual']
        cell['packages'].add(entry['package'])
        if entry.get('receipt'):
            cell['receipts'].append(entry['receipt'])
        if entry.get('issue'):
            cell['issues'].append(entry['issue'])
        if entry.get('note'):
            cell['notes'].append(entry['note'])
    return cells


def aggregate_functional(states):
    states = [s for s in states if s]
    if not states:
        return 'untested'
    if 'failed' in states:
        return 'failed'
    if all(s == 'passed' for s in states):
        return 'passed'
    if 'blocked' in states and 'passed' not in states:
        return 'blocked'
    return 'partial'


def aggregate_visual(states, applies):
    if not applies:
        return 'n/a'
    states = [s for s in states if s]
    if not states:
        return 'none'
    if 'reviewed-wrong' in states:
        return 'reviewed-wrong'
    if all(s == 'reviewed-ok' for s in states):
        return 'reviewed-ok'
    return 'frames'


def summarise(model, entries):
    """Per interaction: per-desktop cells and an aggregate across required desktops."""
    cells = latest_per_cell(entries)
    required = [d['id'] for d in model['desktops'] if d.get('required', True)]
    out = {}
    for item in model['interactions']:
        per_desktop = cells.get(item['id'], {})
        applies = interaction_visual(item)
        # A required desktop without a result for a dimension counts as missing for that
        # dimension, even when it has an entry for the other one.
        functional_states = [per_desktop.get(d, {}).get('functional') for d in required]
        functional = aggregate_functional(functional_states)
        if functional == 'passed' and not all(functional_states):
            functional = 'partial'
        visual_states = [per_desktop.get(d, {}).get('visual') for d in required]
        visual = aggregate_visual(visual_states, applies)
        if visual == 'reviewed-ok' and not all(visual_states):
            visual = 'frames'
        out[item['id']] = {
            'functional': functional, 'visual': visual,
            'desktops': {d: {**c, 'packages': sorted(c['packages'])} for d, c in per_desktop.items()}}
    return out


# -------------------------------------------------------------- catalog

def load_controls(catalog, capture):
    if not catalog or not capture:
        return None
    path = Path(catalog) / (capture + '.controls.json')
    if not path.exists():
        return None
    return json.loads(path.read_text(encoding='utf-8'))


def plain_label(text):
    """A Qt label without mnemonics: `&&` is a literal ampersand, a lone `&` marks a mnemonic."""
    return (text or '').replace('&&', '\0').replace('&', '').replace('\0', '&')


def selectors(item):
    """An interaction's control selectors: `control` may be one string or a list."""
    control = item.get('control')
    if not control:
        return []
    return list(control) if isinstance(control, list) else [control]


def control_matches(control, selector):
    """Selector forms: objectName, `text:LABEL`, `text-glob:LABEL*`, `class:QtClass`, `glob:objectName*`."""
    if selector.startswith('text:'):
        return plain_label(control.get('text')) == selector[5:]
    if selector.startswith('text-glob:'):
        return fnmatch.fnmatchcase(plain_label(control.get('text')), selector[10:])
    if selector.startswith('class:'):
        return control.get('class') == selector[6:]
    if selector.startswith('glob:'):
        return bool(control.get('name')) and fnmatch.fnmatchcase(control['name'], selector[5:])
    return control.get('name') == selector


def match_control(controls, selector):
    """The first control a selector names, or None."""
    if not controls or not selector:
        return None
    for control in controls.get('controls', []):
        if control_matches(control, selector):
            return control
    return None


# Key names Qt reports for the same binding, or platform StandardKey aliases the
# app registers beside the explicit chord (QKeySequence::Undo beside Ctrl+Z).
KEY_ALIASES = {
    'Enter': 'Return', 'Ctrl+Enter': 'Ctrl+Return', 'Ctrl++': 'Ctrl+=', 'Zoom In': 'Ctrl+=', 'Zoom Out': 'Ctrl+-',
    'Undo': 'Ctrl+Z', 'Alt+Backspace': 'Ctrl+Z', 'Redo': 'Ctrl+Shift+Z', 'Alt+Shift+Backspace': 'Ctrl+Shift+Z',
    'Save': 'Ctrl+S', 'Del': 'Delete', 'Backtab': 'Shift+Tab', 'Shift+Backtab': 'Shift+Tab',
}


def canonical_key(key):
    return KEY_ALIASES.get(key, key)


def unmodelled(controls, interactions):
    """Controls and keys the build shows that no interaction (of any surface) claims.

    Claims are global: the main window's chrome appears in every page capture and
    is claimed once, by the shell. Arrow families (`Arrows`, `Ctrl+Arrows`) claim
    the four arrow keys with that modifier.
    """
    claimed_selectors, claimed_keys = [], set()
    for item in interactions:
        claimed_selectors.extend(selectors(item))
        for key in item.get('keys', []):
            if key.endswith('Arrows'):
                prefix = key[:-len('Arrows')]
                claimed_keys.update(prefix + arrow for arrow in ('Left', 'Right', 'Up', 'Down'))
            claimed_keys.add(canonical_key(key))
    missing_controls = []
    for control in controls.get('controls', []) if controls else []:
        name = control.get('name') or ''
        text = plain_label(control.get('text'))
        if control.get('kind') in ('list', 'focusable', 'text-area') and not name:
            continue
        if not name and not text:
            continue
        if any(control_matches(control, selector) for selector in claimed_selectors):
            continue
        missing_controls.append({'name': name, 'text': text, 'kind': control.get('kind'), 'rect': control.get('rect')})
    missing_keys = []
    for binding in controls.get('keys', []) if controls else []:
        keys = binding.get('keys', [])
        if any(canonical_key(key) in claimed_keys for key in keys):
            continue
        missing_keys.append({'keys': ' / '.join(keys), 'text': binding.get('text') or binding.get('name') or ''})
    return missing_controls, missing_keys


# ----------------------------------------------------------------- page

def build_page_data(model, entries, catalog, output, embed=False, label=''):
    summary = summarise(model, entries)
    by_surface = {}
    for item in model['interactions']:
        by_surface.setdefault(item['surface'], []).append(item)
    captures_dir = Path(output) / 'captures'
    surfaces = []
    for surface in model['surfaces']:
        items = by_surface.get(surface['id'], [])
        controls = load_controls(catalog, surface.get('capture'))
        image = None
        if catalog and surface.get('capture'):
            source = Path(catalog) / (surface['capture'] + '.png')
            if source.exists():
                if embed:
                    image = 'data:image/png;base64,' + base64.b64encode(source.read_bytes()).decode('ascii')
                else:
                    captures_dir.mkdir(parents=True, exist_ok=True)
                    # Content-hashed name: a changed capture gets a new URL, so a cached
                    # old picture never sits under new hotspots.
                    digest = hashlib.sha256(source.read_bytes()).hexdigest()[:12]
                    name = '%s.%s.png' % (source.stem, digest)
                    shutil.copyfile(source, captures_dir / name)
                    image = 'captures/' + name
        hotspots = []
        for item in items:
            for selector in selectors(item):
                control = match_control(controls, selector)
                if control:
                    hotspots.append({'interaction': item['id'], 'rect': control['rect'], 'text': control.get('text', '')})
                    break
        missing_controls, missing_keys = unmodelled(controls, model['interactions'])
        surfaces.append({
            **{k: v for k, v in surface.items()},
            'image': image,
            'imageSize': [controls['width'], controls['height']] if controls else None,
            'hotspots': hotspots,
            'unmodelledControls': missing_controls,
            'unmodelledKeys': missing_keys,
            'hasControls': controls is not None,
        })
    drivers = drivers_by_interaction(model)
    interactions = []
    for item in model['interactions']:
        interactions.append({**item, 'drivers': drivers.get(item['id'], []),
                             'visualApplies': interaction_visual(item), **summary[item['id']]})
    counts = {'total': len(interactions)}
    for state in ('passed', 'partial', 'failed', 'blocked', 'untested'):
        counts[state] = sum(1 for i in interactions if i['functional'] == state)
    for state in ('frames', 'reviewed-ok', 'reviewed-wrong', 'none'):
        counts['visual-' + state] = sum(1 for i in interactions if i['visual'] == state)
    return {
        'generated': dt.datetime.now(dt.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'),
        'label': label,
        'catalog': str(catalog) if catalog else None,
        'desktops': model['desktops'],
        'packages': list(PACKAGES),
        'surfaces': surfaces,
        'interactions': interactions,
        'counts': counts,
        'campaigns': sorted({e.get('campaign', '') for e in entries}),
    }


def render(data, template_path=TEMPLATE_PATH):
    template = Path(template_path).read_text(encoding='utf-8')
    payload = json.dumps(data, separators=(',', ':')).replace('</', '<\\/')
    return template.replace('/*__DATA__*/null', payload).replace('__GENERATED__', html.escape(data['generated'])) \
        .replace('__LABEL__', html.escape(data.get('label') or ''))


def build(args):
    model = load_model(args.model)
    entries = load_coverage(args.coverage)
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    data = build_page_data(model, entries, args.catalog, output, embed=args.embed, label=getattr(args, 'label', '') or '')
    page = output / ('index.html' if getattr(args, 'index', False) else 'map.html')
    page.write_text(render(data), encoding='utf-8')
    (output / 'map.json').write_text(json.dumps(data, indent=1, sort_keys=True), encoding='utf-8')
    if getattr(args, 'open', False) and not getattr(args, '_opened', False):
        open_in_browser(page)
        args._opened = True
    return data


def open_in_browser(path):
    """Open the page in the desktop's browser; file:// is enough, nothing is served."""
    import subprocess
    import webbrowser
    url = Path(path).resolve().as_uri()
    try:
        subprocess.Popen(['xdg-open', url], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except OSError:
        webbrowser.open(url)


def watch(args):
    """Rebuild whenever a coverage file, the model or the catalog changes. Polling, no daemon."""
    tracked = [Path(args.model), TEMPLATE_PATH] + [Path(p) for p in args.coverage]
    if args.catalog:
        tracked.append(Path(args.catalog) / 'manifest.json')

    def stamp():
        return tuple((p.stat().st_mtime_ns if p.exists() else None) for p in tracked)

    last = None
    while True:
        current = stamp()
        if current != last:
            try:
                data = build(args)
                counts = data['counts']
                print('%s rebuilt: %d interactions, %d passed, %d partial, %d failed, %d untested' % (
                    data['generated'], counts['total'], counts['passed'], counts['partial'],
                    counts['failed'], counts['untested']), flush=True)
            except (ModelError, json.JSONDecodeError) as error:
                print('not rebuilt: %s' % error, file=sys.stderr, flush=True)
            last = current
        time.sleep(args.interval)


# -------------------------------------------------------------- receipts

def classify_receipt(doc):
    """Which lab driver wrote this receipt, from its content alone."""
    if not isinstance(doc, dict):
        return None
    if doc.get('scenario') == 'edit-region-independent-paste' and isinstance(doc.get('checks'), dict):
        return 'edit_acceptance'
    if doc.get('scenario') == 'mcp-editor-e2e' and isinstance(doc.get('checks'), dict):
        return 'mcp_editor_e2e'
    if 'testId' in doc and str(doc.get('scenario', '')).endswith('-independent-paste'):
        return 'lab_run'
    if 'protocolPassed' in doc and ('coverage' in doc or 'witnesses' in doc):
        return 'tray_progress'
    if 'driverSha256' in doc and 'command' in doc:
        return 'native_acceptance'
    if isinstance(doc.get('rounds'), list) and 'builds' in doc:
        return 'record_run'
    if isinstance(doc.get('interactions'), list):
        return 'surface_map'
    return None


def import_receipt(model, path, desktop, package):
    """Coverage entries for one receipt file, through the model's receipt mapping."""
    path = Path(path)
    doc = json.loads(path.read_text(encoding='utf-8'))
    source = classify_receipt(doc)
    if source is None:
        raise ModelError('%s: not a receipt this importer knows' % path)
    mapping = model.get('receipts', {}).get(source, {})
    at = doc.get('finishedAt') or doc.get('at') or dt.datetime.fromtimestamp(
        path.stat().st_mtime, dt.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')
    entries = []
    if source in ('edit_acceptance', 'mcp_editor_e2e'):
        frames = 'frames' if doc.get('witnesses') and source == 'edit_acceptance' else None
        for check, result in doc.get('checks', {}).items():
            for target in mapping.get('checks.' + check, []):
                ok = result.get('ok') if isinstance(result, dict) else bool(result)
                entries.append(make_entry(target, desktop, package, 'passed' if ok else 'failed', frames,
                                          path.as_posix(), source=source, at=at))
    elif source == 'lab_run':
        scenario = str(doc.get('scenario', ''))[:-len('-independent-paste')]
        state = 'passed' if doc.get('ok') else 'failed'
        for target in mapping.get('scenario:' + scenario, []):
            entries.append(make_entry(target, desktop, package, state, 'frames' if doc.get('witness') else None,
                                      path.as_posix(), source='lab_run', at=at))
    elif source == 'tray_progress':
        passed = doc.get('protocolPassed')
        state = 'passed' if passed else 'failed'
        for target in mapping.get('protocolPassed', []):
            entries.append(make_entry(target, desktop, package, state, 'frames', path.as_posix(),
                                      source='tray_progress', at=at))
        coverage = doc.get('coverage') or {}
        for scenario in (coverage.keys() if isinstance(coverage, dict) else coverage):
            for target in mapping.get('scenario:' + str(scenario), []):
                entries.append(make_entry(target, desktop, package, state, 'frames', path.as_posix(),
                                          source='tray_progress', at=at))
        for item in doc.get('attention', []) or []:
            opened = item.get('openedBy')
            if not opened:
                continue
            blocked = opened != 'visible-menu'
            for target in mapping.get('attention:' + opened, []):
                entries.append(make_entry(target, desktop, package, 'blocked' if blocked else state, 'frames',
                                          path.as_posix(), note='opened through the D-Bus fallback' if blocked else None,
                                          source='tray_progress', at=at))
    elif source == 'native_acceptance':
        command = str(doc.get('command') or '')
        result = doc.get('result') if isinstance(doc.get('result'), dict) else {}
        candidates = {command}
        if result.get('action'):
            candidates.add(command + ' ' + str(result['action']))
        if isinstance(doc.get('argv'), list):
            candidates.add(' '.join(str(a) for a in doc['argv']))
        ok = bool(doc.get('ok')) and not doc.get('error')
        for candidate in candidates:
            for target in mapping.get('command:' + candidate, []):
                entries.append(make_entry(target, desktop, package, 'passed' if ok else 'failed', None,
                                          path.as_posix(), source='native_acceptance', at=at))
    elif source == 'record_run':
        rounds = doc.get('rounds') or []
        ok = bool(rounds) and not doc.get('error') and all(not r.get('error') for r in rounds)
        for target in mapping.get('rounds', []):
            entries.append(make_entry(target, desktop, package, 'passed' if ok else 'failed',
                                      'frames' if any(r.get('frame') for r in rounds) else None,
                                      path.as_posix(), source='record_run', at=at))
    elif source == 'surface_map':
        # A driver that already speaks this tool's language.
        for row in doc['interactions']:
            entries.append(make_entry(row['interaction'], row.get('desktop', desktop), row.get('package', package),
                                      row.get('functional'), row.get('visual'), row.get('receipt', path.as_posix()),
                                      row.get('issue'), row.get('note'), source=row.get('source', 'driver'), at=row.get('at', at)))
    return entries


def do_import(args):
    model = load_model(args.model)
    existing = load_coverage([args.coverage])
    campaign = args.campaign or (existing[0]['campaign'] if existing else Path(args.coverage).stem)
    added = []
    for receipt in args.receipts:
        added.extend(import_receipt(model, receipt, args.desktop, args.package))
    if not added:
        print('no interactions mapped; check model receipts for these receipt kinds', file=sys.stderr)
        return 1
    write_coverage(args.coverage, campaign, existing + added)
    print('%d entries added to %s' % (len(added), args.coverage))
    return 0


def do_mark(args):
    model = load_model(args.model)
    ids = {i['id'] for i in model['interactions']}
    desktops = {d['id'] for d in model['desktops']}
    if args.interaction not in ids:
        raise ModelError('unknown interaction %s' % args.interaction)
    if args.desktop not in desktops:
        raise ModelError('unknown desktop %s; known: %s' % (args.desktop, ', '.join(sorted(desktops))))
    if not args.functional and not args.visual:
        raise ModelError('give --functional and/or --visual')
    existing = load_coverage([args.coverage])
    campaign = args.campaign or (existing[0]['campaign'] if existing else Path(args.coverage).stem)
    entry = make_entry(args.interaction, args.desktop, args.package, args.functional, args.visual,
                       args.receipt, args.issue, args.note)
    write_coverage(args.coverage, campaign, existing + [entry])
    print(json.dumps(entry))
    return 0


def checklist(args):
    model = load_model(args.model)
    entries = load_coverage(args.coverage)
    summary = summarise(model, entries)
    required = [d for d in model['desktops'] if d.get('required', True)]
    if args.desktop:
        required = [d for d in required if d['id'] == args.desktop]
    surfaces = {s['id']: s for s in model['surfaces']}
    drivers = drivers_by_interaction(model)
    lines = ['# Surface map checklist', '']
    for desktop in required:
        rows = []
        for item in model['interactions']:
            cell = summary[item['id']]['desktops'].get(desktop['id'], {})
            functional = cell.get('functional') or 'untested'
            visual = cell.get('visual') or ('n/a' if not interaction_visual(item) else 'none')
            if functional == 'passed' and visual in ('reviewed-ok', 'n/a'):
                continue
            rows.append('- [ ] %s · %s (%s) — functional: %s, visual: %s%s' % (
                surfaces[item['surface']]['label'], item['label'], item['kind'], functional, visual,
                ' · driver: ' + ', '.join(drivers[item['id']]) if drivers.get(item['id']) else ' · manual'))
        lines += ['## %s (%d open)' % (desktop['label'], len(rows)), ''] + (rows or ['- nothing open']) + ['']
    text = '\n'.join(lines)
    if args.output:
        Path(args.output).write_text(text, encoding='utf-8')
    else:
        print(text)
    return 0


def check(args):
    model = load_model(args.model)
    print('model ok: %d desktops, %d surfaces, %d interactions' % (
        len(model['desktops']), len(model['surfaces']), len(model['interactions'])))
    if args.catalog:
        # putgd-ui-catalog always finishes by writing manifest.json; without it the run did
        # not complete (or this is not a catalog), so nothing below can be trusted.
        manifest = Path(args.catalog) / 'manifest.json'
        if not manifest.is_file():
            print('not a complete putgd-ui-catalog output (no manifest.json): ' + args.catalog, file=sys.stderr)
            return 1
        meta = json.loads(manifest.read_text(encoding='utf-8'))
        if meta.get('kind') != 'synthetic-ui-catalog' or not meta.get('captures'):
            print('catalog manifest lists no captures: ' + args.catalog, file=sys.stderr)
            return 1
        # Every capture the producer declares with a controls sidecar must have it: without the
        # sidecar, selector validation and hotspots silently disappear for that capture.
        lost = [c['controls'] for c in meta['captures'] if c.get('controls')
                and not (Path(args.catalog) / c['controls']).is_file()]
        if lost:
            print('declared controls files missing from catalog: ' + ', '.join(lost), file=sys.stderr)
            return 1
        platform = meta.get('platform')
        missing = [s['capture'] for s in model['surfaces'] if s.get('capture')
                   and not (Path(args.catalog) / (s['capture'] + '.png')).exists()]
        # Only the documented offscreen-only synthetic states may be absent, and only from a
        # native (PUTGD_CATALOG_HANDSHAKE) catalog; the map shows them as placeholders.
        allowed = OFFSCREEN_ONLY_CAPTURES if platform != 'offscreen' else set()
        unexpected = [c for c in missing if c not in allowed]
        if unexpected:
            print('captures missing from the %s catalog: %s' % (platform, ', '.join(unexpected)), file=sys.stderr)
            return 1
        if missing:
            print('offscreen-only captures not in this %s catalog (placeholders): %s' % (platform, ', '.join(missing)))
        # A control missing from the surface's own capture but present in another
        # state is fine (no hotspot, listed as an action); one found nowhere is drift.
        everywhere = [json.loads(p.read_text(encoding='utf-8')) for p in sorted(Path(args.catalog).glob('*.controls.json'))]
        elsewhere, nowhere = [], []
        for surface in model['surfaces']:
            controls = load_controls(args.catalog, surface.get('capture'))
            for item in model['interactions']:
                if item['surface'] != surface['id'] or controls is None:
                    continue
                for selector in selectors(item):
                    if match_control(controls, selector):
                        continue
                    if any(match_control(other, selector) for other in everywhere):
                        elsewhere.append('%s -> %s' % (item['id'], selector))
                    else:
                        nowhere.append('%s -> %s' % (item['id'], selector))
        if elsewhere:
            print('controls only in other states (no hotspot): ' + ', '.join(elsewhere))
        if nowhere:
            print('controls found in no capture:\n  ' + '\n  '.join(nowhere), file=sys.stderr)
            return 1
        print('catalog ok: %d captures with controls' % len(everywhere))
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--model', default=str(MODEL_PATH))
    sub = parser.add_subparsers(dest='command', required=True)

    p = sub.add_parser('build', help='write map.html and map.json')
    p.add_argument('--catalog', help='putgd-ui-catalog output directory')
    p.add_argument('--coverage', nargs='*', default=[], help='campaign coverage files')
    p.add_argument('--output', required=True)
    p.add_argument('--embed', action='store_true', help='inline the pictures (one portable file)')
    p.add_argument('--open', action='store_true', help='open the page in the browser after building')
    p.add_argument('--label', default='', help='what this map describes, shown in the header (a nightly tag)')
    p.add_argument('--index', action='store_true', help='name the page index.html (for a web root)')
    p.add_argument('--watch', action='store_true', help='rebuild when the inputs change')
    p.add_argument('--interval', type=float, default=2.0)

    p = sub.add_parser('mark', help='record a manual result')
    p.add_argument('interaction')
    p.add_argument('--coverage', required=True)
    p.add_argument('--campaign')
    p.add_argument('--desktop', required=True)
    p.add_argument('--package', required=True, choices=PACKAGES)
    p.add_argument('--functional', choices=FUNCTIONAL)
    p.add_argument('--visual', choices=VISUAL)
    p.add_argument('--receipt')
    p.add_argument('--issue', type=int)
    p.add_argument('--note')

    p = sub.add_parser('import', help='map lab receipts onto interactions')
    p.add_argument('receipts', nargs='+')
    p.add_argument('--coverage', required=True)
    p.add_argument('--campaign')
    p.add_argument('--desktop', required=True)
    p.add_argument('--package', required=True, choices=PACKAGES)

    p = sub.add_parser('checklist', help='what is still open, per desktop, as markdown')
    p.add_argument('--coverage', nargs='*', default=[])
    p.add_argument('--desktop')
    p.add_argument('--output')

    p = sub.add_parser('check', help='validate the model, and the catalog against it')
    p.add_argument('--catalog')

    args = parser.parse_args(argv)
    try:
        if args.command == 'build':
            if args.watch:
                watch(args)
                return 0
            data = build(args)
            counts = data['counts']
            print('%s/%s: %d interactions, %d passed, %d partial, %d failed, %d blocked, %d untested' % (
                args.output, 'index.html' if args.index else 'map.html', counts['total'], counts['passed'], counts['partial'], counts['failed'],
                counts['blocked'], counts['untested']))
            return 0
        if args.command == 'mark':
            return do_mark(args)
        if args.command == 'import':
            return do_import(args)
        if args.command == 'checklist':
            return checklist(args)
        if args.command == 'check':
            return check(args)
    except ModelError as error:
        print('error: %s' % error, file=sys.stderr)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())
