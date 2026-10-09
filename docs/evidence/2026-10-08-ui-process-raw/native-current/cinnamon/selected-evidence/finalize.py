#!/usr/bin/env python3
"""Bounded native receipt and selected export; excludes configs, keys and payloads."""
import collections
import json
from pathlib import Path
import shutil
import xml.etree.ElementTree as ET

from PIL import Image
import native as n


def read(name):
    return json.loads((n.OUT / name).read_text())


source = read('candidate-source.json')
installed = read('installed-executables.json')
host = json.loads((n.OUT.parent / 'host/receipt.json').read_text())
data = n.snapshot()
resident = n.lab.verify_running_app(n.RUN)
assert resident == read('start.json')['resident']
assert n.helper_pid(data) == 0 and data['diagnostics']['ui']['lastExit']['code'] == 0
assert data['diagnostics']['ui']['lastExit']['status'] == 'normal'
assert not any(w['pid'] == resident['pid'] for w in n.windows())
operations = n.lab.cli(n.RUN, 'operation.recent')['operations']
assert all(o['state'] in ('completed', 'failed', 'cancelled') for o in operations)
with n.lab.QMP(n.RUN / 'qmp.sock') as q:
    vnc = q.execute('query-vnc')
assert vnc['enabled'] and vnc['host'] == '127.0.0.1' and vnc['service'] == '5938'
matrix = read('matrix.json')['attempts']
tray = [read('tray-' + action + '.json')['result'] for action in ('region', 'window', 'desktop', 'clipboard')]
assert len(matrix) == 12 and all(c['ok'] for c in matrix + tray)
views = read('views.json')
tray_views = read('tray-views.json')['results']
assert views['views'] == 8 and len(tray_views) == 4
guards = [read('views-0-' + view + '-owned-capture.json')['operation'] for view in ('settings', 'history', 'activity', 'about')]
modal = read('native-folder-modal-result.json')
assert all(o['state'] == 'failed' and o['error']['code'] == 'capture_target' and not o['items'] for o in guards)
assert modal['errorCode'] == 'capture_target' and modal['itemCount'] == 0
continuity = read('continuity-run1/receipt.json')
assert continuity['verdict'] == 'pass' and continuity['activeOperationObservedAfterHelperExit']
samples = continuity['observations']
active = [s for s in samples if s['helperGoneBeforeInspect'] and s['state'] not in ('completed', 'failed', 'cancelled', 'partial', 'interrupted')]
before = [s for s in samples if not s['helperGoneBeforeInspect']]
selected = n.OUT / 'selected-evidence'
selected.mkdir(exist_ok=False)
exported = {}


def copy(name, destination=None):
    target = selected / (destination or name)
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(n.OUT / name, target)
    exported[str(target.relative_to(selected))] = n.lab.hash_file(target)


for name in ['native.py', 'continuity.py', 'finalize.py', 'README.md', 'events.jsonl',
             'installed-executables.json', 'installed-executable-probe.sh', 'native-fonts-probe.sh',
             'native-folder-fonts.json', 'native-folder-modal-result.json', 'draft-conflict.json',
             'draft-cancel-result.json', 'clipboard-after-helper-exit-result.json',
             'clipboard-after-helper-exit.json', 'matrix.json', 'views.json', 'tray-views.json',
             'continuity-run1/parameters.json', 'continuity-run1/observer.py',
             'continuity-run1/observer-result.json', 'continuity-run1/receipt.json',
             'continuity/preflight-failure-assessment.json', 'continuity-phase.log',
             'install-preflight-failed.json', 'install-phase.log']:
    copy(name)
for action in ('region', 'window', 'desktop', 'clipboard'):
    copy('tray-' + action + '.json')
for view in ('settings', 'history', 'activity', 'about'):
    copy('views-0-' + view + '-owned-capture.json')
for name in ['tray-menu-inspect.png', 'resident-tray-only.png', 'functional-final-tray-only.png',
             'draft-final-reopened.png', 'draft-conflict-reopened.png', 'draft-cancel-reopened-clean.png',
             'native-folder-picker-settled.png', 'clipboard-after-helper-exit.png',
             'continuity-before-trigger.png', 'continuity-after-close.png']:
    copy(name)
for view in ('settings', 'history', 'activity', 'about'):
    copy('tray-view-' + view + '-open.png')
    copy('tray-view-' + view + '-closed.png')
for case in matrix + tray:
    directory = n.RUN / case['evidenceDirectory']
    target = selected / 'tests' / case['testId']
    target.mkdir(parents=True)
    for name in ('results.json', 'results.xml', 'parameters.json', 'paste.json', 'ready.json',
                 'versions.json', 'package.json', 'diagnostics.json', 'manifest.json'):
        p = directory / name
        if p.is_file():
            shutil.copyfile(p, target / name)
            exported[str((target / name).relative_to(selected))] = n.lab.hash_file(target / name)
    if (directory / 'final-head0.ppm').exists():
        Image.open(directory / 'final-head0.ppm').save(target / 'final.png')
        exported[str((target / 'final.png').relative_to(selected))] = n.lab.hash_file(target / 'final.png')
n.lab.write_json(selected / 'export-manifest.json', {'schemaVersion': 1, 'files': exported,
    'scope': 'Explicit selected native evidence. Original test manifests describe full retained local run directories; source payloads/private keys/configuration are not exported.'})
receipt = {
    'schemaVersion': 1, 'timestampUtc': n.now(), 'status': 'native-functional-subset-passed',
    'sourceCommit': source['commit'], 'sourceTree': source['tree'], 'archiveSha256': source['archiveSha256'],
    'build': data['build'], 'package': host['package'], 'executables': installed,
    'resident': resident, 'ui': data['diagnostics']['ui'], 'trayAvailable': True,
    'renderedTrayMenuWitness': 'selected-evidence/tray-menu-inspect.png',
    'vnc': {'host': vnc['host'], 'port': 5938, 'enabled': True},
    'nativeCases': {'matrixPassed': 12, 'trayActionsPassed': 4,
                    'testIds': [c['testId'] for c in matrix + tray],
                    'operationIds': [c['operationId'] for c in matrix + tray]},
    'views': {'trayOpenedViews': ['settings', 'history', 'activity', 'about'],
              'additionalCliCycles': 2, 'normalHelperExitAfterEachLastClose': True,
              'populatedHistoryAndActivityVisuallyInspected': True,
              'allViewsClosed': True, 'residentIdentityUnchanged': True},
    'draft': {'syntheticNonSecretEditsRetainedAcrossNormalExit': True,
              'externalChangeConflictRetainedAcrossNormalExit': True,
              'cancelDiscardedDraftWithoutSaving': True,
              'persistedProfileCountAfterCancel': 0,
              'witnesses': ['draft-final-reopened.png', 'draft-conflict-reopened.png', 'draft-cancel-reopened-clean.png']},
    'nativeIntegration': {'gtkFolderDialogOpenedAndCancelled': True,
                          'fontsAndLibraries': read('native-folder-fonts.json'),
                          'limit': 'Native GTK integration and configured Cinnamon font retained; live font-setting changes were not tested.'},
    'ownedWindowCapture': {'fourViewOperationIds': [o['id'] for o in guards],
                          'nativeModalOperationId': modal['operationId'],
                          'allRejectedWith': 'capture_target', 'artifactsCreated': 0},
    'clipboardResultPastedAfterHelperExit': True,
    'localContinuity': {'operationId': continuity['operation']['id'], 'bytes': 536870912,
                       'verdict': continuity['verdict'], 'scope': 'Real local clipboard-file acquisition/archive/copy only; no S3 transfer.',
                       'guestMonotonicLastHelperAliveSample': before[-1],
                       'guestMonotonicFirstActiveAfterHelperGoneSample': active[0],
                       'guestMonotonicLastActiveAfterHelperGoneSample': active[-1],
                       'guestMonotonicCompletionSample': samples[-1],
                       'observedActiveAfterExitIntervalSeconds': active[-1]['monotonicSeconds'] - active[0]['monotonicSeconds'],
                       'timingLimit': 'Sample times bound observation; they are not an exact process-exit timestamp.',
                       'artifactSha256Output': continuity['artifactSha256Output'],
                       'artifactSizes': continuity['artifactSizes'], 'normalExit': continuity['normalExit']},
    'finalOperations': {'count': len(operations), 'states': dict(collections.Counter(o['state'] for o in operations)),
                        'expectedFailureIds': [o['id'] for o in guards] + [modal['operationId']]},
    'preservedNonProductFailures': [
        'Initial install precondition found the prior baseline resident had exited. No install mutation occurred. A read-only synthetic journal check verified all 13 historical operations terminal before preserving its directory; old code was not restarted.',
        'A full-settings evidence write was rejected by automatic approval review before running. Subsequent draft evidence records only revisions, synthetic edited fields, one preference and UI/build state.',
        'The first manual draft gesture was not held long enough after layout changed. Retained screenshots showed the Region field instead of Bucket; subsequent held-click witness verifies the actual synthetic Bucket and Region draft used for acceptance.',
        'A nested shell here-document delimiter collision stopped one font probe before guest execution; the corrected probe is retained.',
        'The first continuity preflight raced fixture ready.json, which is emitted one second after map. No payload or app operation was created. After adding an explicit ready-file condition, exactly one real 512 MiB operation was executed and passed.'
    ],
    'limits': [
        'Single-head Cinnamon/X11 QEMU KVM evidence; no physical-machine performance or complete compositor certification.',
        'No native helper-crash/mismatch attempt, typed-credential close test, exact remote deletion, real S3 upload or interrupted remote recovery was performed here. Relevant headless contracts remain separate.',
        'No memory measurement ran during this functional wave. The post-use memory scenario includes this large local copy and must not be described as empty cold idle.',
        'Golden disk unchanged; existing overlay reused; VNC output remained visible. No cloud credentials, user payloads or remote objects were used.',
        'Early mapped-window witnesses may capture Cinnamon fade animation; settled draft and native-dialog witnesses establish their final rendering.'
    ],
    'fullAE17Complete': False, 'memoryAcceptanceEstablished': False,
    'selectedEvidenceManifest': 'selected-evidence/export-manifest.json',
    'selectedEvidenceManifestSha256': n.lab.hash_file(selected / 'export-manifest.json')
}
n.save('receipt.json', receipt)
suite = ET.Element('testsuite', name='U11 Cinnamon native acceptance subset', tests=str(16), failures='0')
for case in matrix + tray:
    ET.SubElement(suite, 'testcase', name=case['scenario'], classname='cinnamon-x11', time=str(case['elapsedSeconds']))
ET.ElementTree(suite).write(n.OUT / 'matrix-and-tray.xml', encoding='utf-8', xml_declaration=True)
print(json.dumps({'status': receipt['status'], 'resident': resident, 'nativeCases': 16,
                  'continuityOperationId': receipt['localContinuity']['operationId'],
                  'activeAfterExitSeconds': receipt['localContinuity']['observedActiveAfterExitIntervalSeconds'],
                  'receipt': str(n.OUT / 'receipt.json')}))
