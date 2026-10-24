"""Explicit history, upload-burst and slow-network phases for #379.

All actions address the isolated resident, synthetic files and authenticated
loopback fixture. GUI response numbers are external drawable upper bounds,
separate from API acknowledgement and eventual operation completion.
"""
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import re
import statistics
import subprocess
import sys
import time
from urllib.parse import urlsplit

from bench_ui_process import EvidenceError, identity, sha256
from observe_idle import owned_windows, stop_owned_process_group


def require(value, code):
    if not value:
        raise EvidenceError(code)


def policy(path):
    if not path:
        return None
    from physical_campaign import private_path
    path = private_path(path)
    value = json.loads(path.read_text())
    require(set(value) == {'schemaVersion', 'approvedBy', 'approvalReference', 'maximums'}
            and value['schemaVersion'] == 1, 'invalid_threshold_policy_schema')
    require(all(isinstance(value[key], str) and 0 < len(value[key]) <= 512
                for key in ('approvedBy', 'approvalReference')), 'threshold_approval_attribution_required')
    allowed = {'historyOpenUpperBoundMs', 'historySearchP95UpperBoundMs', 'historyScrollP95UpperBoundMs',
               'historyPreviewP95UpperBoundMs', 'slowInputUpperBoundMs', 'cancelTerminalMs', 'settleGrowthBytes'}
    require(isinstance(value['maximums'], dict) and set(value['maximums']) <= allowed
            and all(type(n) in (int, float) and math.isfinite(n) and n >= 0
                    for n in value['maximums'].values()), 'invalid_threshold_policy_maximums')
    return {**value, 'sha256': sha256(path)}


def assess(run, name, measurement):
    run.result['metrics'][name] = measurement
    approved = policy(run.args.threshold_policy)
    if approved:
        run.result['thresholdPolicy'] = approved
    maximum = approved['maximums'].get(name) if approved else None
    run.result['gates'][name] = ({'status': 'unassessed', 'reason': 'no_explicit_approved_threshold'} if maximum is None else
        {'status': 'pass' if measurement is not None and measurement <= maximum else 'fail',
         'maximum': maximum, 'approvalReference': approved['approvalReference']})


def percentile(values):
    values = sorted(value for value in values if value is not None)
    return values[math.ceil(.95 * len(values)) - 1] if values else None


class UI:
    def __init__(self, run):
        from campaign_ui import Accessible, Drawable
        self.run = run
        started = time.monotonic()
        reply = run.call('ui.show', {'view': 'history'})
        acknowledged = time.monotonic()
        require(reply.get('shown') == 'history', 'history_show_ack_mismatch')
        deadline = time.monotonic() + 20
        while True:
            observed = run.sample('ui-mapping')
            helpers = [row for row in observed['processes'] if row['role'] == 'ui']
            if len(helpers) == 1:
                windows = owned_windows(helpers[0]['pid'])
                if len(windows) == 1:
                    self.window, self.owner = windows[0], helpers[0]
                    break
            require(time.monotonic() < deadline, 'history_single_owned_window_not_ready')
            time.sleep(.05)
        subprocess.run(['wmctrl', '-ia', self.window], check=True, timeout=5,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        info = subprocess.check_output(['xwininfo', '-id', self.window], text=True, timeout=5)
        self.width = int(re.search(r'Width:\s+(\d+)', info)[1])
        self.height = int(re.search(r'Height:\s+(\d+)', info)[1])
        self.draw = Drawable(self.window, self.owner)
        self.list_rectangle = [24, 180, min(self.width - 48, 480), min(self.height - 260, 280)]
        self.full_rectangle = [0, 0, self.width, self.height]
        self.draw.pixels(self.list_rectangle)
        open_ms = (time.monotonic() - started) * 1000
        run.event('history-open-external-observation',
                  cliAcknowledgementMs=(acknowledged - started) * 1000,
                  ownedDrawableObservationUpperBoundMs=open_ms,
                  limitation='Observation begins after ui.show acknowledgement; upper bound, not first compositor-present time')
        assess(run, 'historyOpenUpperBoundMs', open_ms)
        deadline = time.monotonic() + 15
        while True:
            try:
                self.accessible = Accessible(self.owner)
                break
            except EvidenceError as error:
                if str(error) != 'owned_qt_accessibility_unavailable' or time.monotonic() >= deadline:
                    self.save('history-accessibility-unavailable')
                    self.draw.close()
                    raise
                time.sleep(.1)
        run.result['guiInstrumentation'] = {'accessibilityForcedForThisScenarioOnly': run.args.enable_accessibility,
            'QT_LINUX_ACCESSIBILITY_ALWAYS_ON': run.environment.get('QT_LINUX_ACCESSIBILITY_ALWAYS_ON'),
            'desktopSettingsChanged': False, 'coldIdleAcceptance': False,
            'observerSha256': sha256(Path(__file__).with_name('campaign_ui.py')),
            'requestedProbeSleepMs': 10, 'pixelSource': 'PID-owned native X11 drawable; no product hook/checkpoint'}

    def save(self, label):
        from campaign_ui import Drawable
        for index, window in enumerate(owned_windows(self.owner['pid'])):
            draw = self.draw if window == self.window else Drawable(window, self.owner)
            try:
                info = subprocess.check_output(['xwininfo', '-id', window], text=True, timeout=5)
                width = int(re.search(r'Width:\s+(\d+)', info)[1])
                height = int(re.search(r'Height:\s+(\d+)', info)[1])
                path = self.run.root / (label + '-' + str(index) + '.png')
                draw.pixels([0, 0, width, height]).save(path)
                self.run.event('owned-ui-visual', label=label, path=path.name, sha256=sha256(path),
                               owner=self.owner, window=window, pixelSource='native owned drawable; companion retains wall screenshots')
            finally:
                if draw is not self.draw:
                    draw.close()

    def action(self, label, action, rectangle=None, require_change=True):
        value = self.draw.response(rectangle or self.list_rectangle, action)
        self.run.event('external-ui-response', label=label, **value)
        if require_change:
            require(value['firstObservedDrawableChangeUpperBoundMs'] is not None, 'no_external_drawable_change_observed')
        self.run.sample(label)
        return value['firstObservedDrawableChangeUpperBoundMs']

    def wait_rows(self, expected):
        deadline = time.monotonic() + 15
        while True:
            names = self.accessible.list_names()
            if len(names) == len(expected) and all(
                    record['name'] in name or Path(record['name']).stem in name
                    for record, name in zip(expected, names)):
                return names
            require(time.monotonic() < deadline, 'accessible_history_rows_mismatch')
            time.sleep(.05)

    def search(self, term):
        measured = self.action('search', lambda: self.accessible.search(term))
        rows = self.run.call('history.list', {'search': term, 'limit': 50})
        self.wait_rows(rows['records'])
        return measured, rows['records']


def history(run):
    require(run.args.fixture_root, 'history_requires_independent_10000_fixture')
    ui = UI(run)
    try:
        rows = run.call('history.list', {'limit': 50})
        ui.wait_rows(rows['records'])
        ui.save('history-open-10000')
        scrolls, seen = [], 0
        # Visit every page and actually scroll its list from Home to its last
        # row. API reads verify expected rows; they do not drive the viewport.
        for offset in range(0, 10000, 50):
            page = run.call('history.list', {'offset': offset, 'limit': 50})['records']
            names = ui.wait_rows(page)
            node = ui.accessible.find('Captures and upload activity')
            ui.accessible.focus(node)
            ui.accessible.key('Home')
            for step in range(30):
                if ui.accessible.last_row_visible():
                    break
                scrolls.append(ui.action('whole-list-scroll', lambda: ui.accessible.key('Page_Down')))
            require(ui.accessible.last_row_visible(), 'whole_page_not_scrolled')
            seen += len(page)
            run.event('history-page-traversed', offset=offset, records=len(page),
                      visibleAccessibleNamesSha256=hashlib.sha256(json.dumps(names).encode()).hexdigest())
            if offset in (0, 4950, 9950):
                ui.save('history-scroll-' + str(offset))
            if offset + 50 < 10000:
                ui.action('history-next-page', lambda: ui.accessible.click('Next'))
        require(seen == 10000, 'whole_list_traversal_incomplete')
        run.result['metrics']['historyWholeListRowsTraversed'] = seen
        assess(run, 'historyScrollP95UpperBoundMs', percentile(scrolls))
        queries = ['landscape-00', 'motion-01', 'landscape-02', 'motion-03', 'landscape-04', 'motion-05']
        searches = []
        for i in range(30):
            measured, _ = ui.search(queries[i % len(queries)])
            searches.append(measured)
        assess(run, 'historySearchP95UpperBoundMs', percentile(searches))
        previews = []
        for term in ('landscape-00', 'motion-01'):
            _, records = ui.search(term)
            selected = records[0]
            # Inspector changes outside the list rectangle; observe it directly.
            previews.append(ui.action('preview-selection', lambda: ui.accessible.select(0),
                                      [max(0, ui.width - 360), 180, 330, min(300, ui.height - 200)]))
            time.sleep(.8)
            detail = run.call('history.detail', {'id': selected['id']})
            preview = Path(detail['previewPath'])
            require(preview.is_file() and preview.resolve().is_relative_to(run.root), 'retained_preview_not_private')
            ui.save('history-preview-' + ('image' if term.startswith('landscape') else 'video'))
            run.event('selected-preview', historyId=selected['id'], category=selected['category'],
                      previewSha256=sha256(preview), semanticVerification='owned accessible row + retained synthetic preview + saved pixels')
            if term.startswith('landscape'):
                ui.action('copy-image', lambda: ui.accessible.click('Copy image'), ui.full_rectangle)
                paste = run.clipboard_command('paste-image')
                from PIL import Image
                original = Path(detail['local']['path'])
                require(original.resolve().is_relative_to(run.root), 'copy_original_not_private')
                image = Image.open(original).convert('RGBA')
                require(paste['pixelSha256'] == hashlib.sha256(image.tobytes()).hexdigest() and
                        (paste['width'], paste['height']) == image.size, 'copy_image_independent_pixels_mismatch')
                run.event('copy-image-independent-verification', historyId=selected['id'], **paste)
                ui.save('history-copy-image')
        assess(run, 'historyPreviewP95UpperBoundMs', percentile(previews))
        # Deletion is scoped to one exact seed receipt on the independently
        # authenticated fixture. No policy/versioning controls are sent.
        run.upload_configuration()
        _, records = ui.search('landscape-00')
        selected = records[0]
        ui.accessible.select(0)
        prepared = run.call('delete.prepare', {'ids': [selected['id']]})
        require(len(prepared['targets']) == 1 and prepared['targets'][0]['historyId'] == selected['id'],
                'delete_prepared_target_mismatch')
        receipt = prepared['targets'][0]['receipt']
        require(receipt['destination']['settings']['endpoint'].rstrip('/') == run.args.endpoint.rstrip('/'),
                'delete_receipt_not_owned_fixture')
        run.event('delete-reviewed', historyId=selected['id'], receipt=receipt,
                  effect=prepared['targets'][0]['effect'])
        ui.action('delete-open-confirmation', lambda: ui.accessible.click('Delete from destination…'),
                  ui.full_rectangle)
        ui.save('history-delete-confirmation')
        ui.accessible.find('Confirm exact remote deletion')
        ui.action('delete-confirm', lambda: ui.accessible.click('Yes'), ui.full_rectangle)
        deadline = time.monotonic() + 30
        while True:
            detail = run.call('history.detail', {'id': selected['id']})
            if detail['remoteStatus'] in ('removed', 'already_missing'):
                break
            require(time.monotonic() < deadline, 'destination_deletion_not_confirmed')
            time.sleep(.1)
        key = receipt['objectReference'].get('key')
        objects = run.fixture()['objects']
        require(key is not None and not objects.get(key), 'fixture_object_still_present_after_delete')
        run.event('destination-delete-independent-verification', historyId=selected['id'],
                  remoteStatus=detail['remoteStatus'], fixtureObjectAbsent=True)
        ui.save('history-destination-deleted')
        run.clipboard_command('clear')
    finally:
        ui.draw.close()


def tray(run, label):
    path = Path(run.args.native_driver).resolve(strict=True) if run.args.native_driver else None
    require(path is not None, '495_native_driver_required_for_tray_witness')
    spec = importlib.util.spec_from_file_location('campaign_native_acceptance', path)
    native = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(native)
    target = native.tray_target(run.environment, native.checked_resident(run.resident.pid, run.args.app,
                                                                         run.environment['XDG_RUNTIME_DIR']))
    result = {'label': label, 'owner': target, 'driverSha256': sha256(path),
              'driverRevision': run.args.native_driver_revision}
    for name in ('Status', 'IconName', 'IconPixmap', 'ToolTip'):
        value = native.bus(run.environment, 'get-property', target['service'], target['path'],
                           'org.kde.StatusNotifierItem', name)
        result[name] = value if name in ('Status', 'IconName') else hashlib.sha256(json.dumps(value).encode()).hexdigest()
        if name == 'ToolTip':
            result['workingTitleObserved'] = 'put.gd — Working' in json.dumps(value, ensure_ascii=False)
    run.event('independent-sni-observation', **result)
    return result


def operation_summary(value):
    return {'id': value['id'], 'state': value['state'], 'phase': value.get('phase'),
            'items': [{'id': item['artifact']['id'], 'uploadStatus': item['uploadStatus'],
                       'phase': item.get('phase'), 'progress': item.get('uploadProgress'),
                       'receipt': item.get('receipt')} for item in value['items']]}


def verify_uploaded(run, operation):
    require(operation['state'] == 'completed' and operation['items'], 'upload_operation_incomplete')
    from urllib.request import build_opener, ProxyHandler, Request, HTTPRedirectHandler
    class NoRedirect(HTTPRedirectHandler):
        def redirect_request(self, *args, **kwargs):
            return None
    opener = build_opener(ProxyHandler({}), NoRedirect())
    for item in operation['items']:
        require(item['uploadStatus'] == 'uploaded', 'upload_item_not_uploaded')
        item_id = item['artifact']['id']
        record = run.call('history.detail', {'id': item_id})
        require(record['operationId'] == operation['id'] and record['uploadStatus'] == 'uploaded',
                'upload_history_mismatch')
        receipt = item['receipt']
        link = receipt['directUrl']
        # The fixture's public links must stay on the authenticated fixture or
        # its owned proxy. Never follow a redirect to an external destination.
        parsed = urlsplit(link)
        require(parsed.hostname == '127.0.0.1' and parsed.scheme == 'http' and
                not parsed.username and not parsed.password and
                link.startswith(run.args.endpoint.rstrip('/') + '/fixture/'), 'non_loopback_result_link')
        with opener.open(Request(link), timeout=15) as response:
            require(response.geturl() == link, 'result_link_redirected')
            digest = hashlib.sha256()
            for block in iter(lambda: response.read(65536), b''):
                digest.update(block)
        require(digest.hexdigest() == item['artifact']['sha256'], 'independent_uploaded_bytes_mismatch')
        run.event('upload-independent-verification', operationId=operation['id'], historyId=item_id,
                  directUrl=link, sha256=digest.hexdigest())


def settle(run, baseline):
    run.clipboard_command('clear')
    rows = run.interval('post-upload-settle', run.args.settle_seconds, closed=True)
    final = rows[-1][1]['totals']
    run.result['metrics']['postUploadFinalFamily'] = final
    growth = max(0, final['pssBytes'] - baseline['pssBytes'])
    assess(run, 'settleGrowthBytes', growth)
    from physical_campaign import MIB
    for name, limit in (('rssBytes', 40 * MIB), ('pssBytes', 29 * MIB)):
        run.result['gates']['settled-' + name] = {'status': 'pass' if final[name] <= limit else 'fail',
                                               'limitBytes': limit}
    run.result['limitations'].append('Post-upload sampled growth/plateau is measured; a finite settle does not prove absence of all leaks.')


def burst(run):
    run.upload_configuration()
    baseline = run.sample('pre-burst')['totals']
    initial = tray(run, 'before-burst')
    ui = UI(run)
    run.fixture('/__rules', {'rules': [{'method': 'PUT', 'delay': 2, 'remaining': 100}]})
    operations = []
    queued_peak = uploading_peak = 0
    try:
        for index in range(50):
            deadline = time.monotonic() + 30
            while True:
                try:
                    operation_id = run.call('action.invoke', {'action': 'desktop'})['operationId']
                    break
                except EvidenceError as error:
                    require(str(error) == 'cli_busy_selecting' and time.monotonic() < deadline,
                            'burst_capture_admission_failed')
                    run.event('burst-admission-wait', index=index, code='busy_selecting')
                    time.sleep(.05)
            operations.append(operation_id)
            run.event('burst-accepted', index=index, operationId=operation_id)
            if index % 5 == 4:
                states = [run.call('operation.inspect', {'id': key}) for key in operations]
                queued = sum(value.get('phase') == 'queued' for value in states)
                uploading = sum(any(item.get('uploadStatus') == 'uploading' for item in value['items']) for value in states)
                queued_peak, uploading_peak = max(queued_peak, queued), max(uploading_peak, uploading)
                run.event('burst-queue-observation', queued=queued, uploading=uploading)
                run.sample('burst-active')
                active = tray(run, 'burst-' + str(index))
                if index in (4, 24, 49):
                    ui.save('burst-progress-' + str(index))
        require(len(set(operations)) == 50 and uploading_peak >= 2 and queued_peak >= 1,
                'burst_did_not_demonstrate_concurrent_queued_uploads')
        require(active['IconPixmap'] != initial['IconPixmap'] or active['ToolTip'] != initial['ToolTip'],
                'independent_tray_did_not_change_during_burst')
        for operation_id in operations:
            operation = run.wait_operation(operation_id)
            run.event('burst-terminal', **operation_summary(operation))
            verify_uploaded(run, operation)
        time.sleep(2)
        final = tray(run, 'burst-settled-tray')
        require(not final['workingTitleObserved'], 'tray_did_not_return_to_idle')
        ui.save('burst-completed')
        run.result['metrics']['burst'] = {'acceptedCaptures': 50, 'concurrentUploadPeakObserved': uploading_peak,
                                        'queuedOperationPeakObserved': queued_peak, 'linksIndependentlyVerified': 50,
                                        'finalTrayStatus': final['Status']}
    finally:
        run.fixture('/__rules', {'rules': []})
        ui.draw.close()
        subprocess.run(['wmctrl', '-ic', ui.window], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5)
        time.sleep(2)
    settle(run, baseline)


def slow(run):
    from campaign_fixture import png
    from physical_campaign import read_line
    run.upload_configuration()
    baseline = run.sample('pre-slow')['totals']
    source = run.result['driverProxySource'] = str(run.repo / 'docs/evidence/2026-10-20-round9-storage/scripts/throttle_proxy.py')
    run.result['proxyWorkload'] = {'originalSha256': sha256(source), 'wrapperSha256': sha256(Path(__file__).with_name('campaign_proxy.py')),
        'bytesPerSecondShared': 262144, 'change': 'token cap max(rate*.05,chunkBytes) prevents original16384>13107.2deadlock',
        'scope': 'loopback S3 client-to-server only; separate stalled listener; no system-network changes'}
    proxy = None
    def start(stall=False, port=0):
        command = [sys.executable, str(Path(__file__).with_name('campaign_proxy.py')), '--listen', str(port),
                   '--target', str(urlsplit(run.args.endpoint).port), '--rate', '262144', '--source', source]
        if stall:
            command.append('--stall')
        process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                   stderr=subprocess.DEVNULL, text=True, start_new_session=True)
        # Proxy prints a single integer, unlike fixture JSON handshakes.
        import selectors
        with selectors.DefaultSelector() as selector:
            selector.register(process.stdout, selectors.EVENT_READ)
            require(selector.select(10), 'proxy_readiness_timeout')
            actual_port = int(process.stdout.readline())
        run.event('owned-proxy-ready', pid=process.pid, port=actual_port, stalled=stall, command=command)
        return process, actual_port
    ui = None
    try:
        proxy, port = start()
        doc = run.call('settings.get')['document']
        doc['profiles'][0]['settings'].update(endpoint=f'http://127.0.0.1:{port}')
        # Public GET verification bypasses the upload-only proxy but remains
        # on the authenticated fixture (same object keys and bytes).
        run.call('settings.patch', {'patch': doc, 'expectedRevision': run.call('settings.get')['metadata']['observedRevision']})
        media = run.root / 'media'
        media.mkdir(mode=0o700, exist_ok=True)
        path = media / 'slow-synthetic-noise.png'
        png(path, 379, 1920, 1080, noise=True)
        run.event('slow-payload', path=path.name, bytes=path.stat().st_size, sha256=sha256(path), seed=379379)
        run.clipboard_command('file', path=str(path))
        started = time.monotonic()
        operation_id = run.call('action.invoke', {'action': 'clipboard'})['operationId']
        ui = UI(run)
        response = ui.action('slow-input-response', lambda: ui.accessible.search('synthetic'),
                             [0, 0, min(ui.width, 800), 180])
        assess(run, 'slowInputUpperBoundMs', response)
        ui.save('slow-upload-progress')
        tray(run, 'slow-progress')
        observed_progress = []
        while True:
            operation = run.call('operation.inspect', {'id': operation_id})
            run.event('slow-progress', **operation_summary(operation))
            observed_progress.extend(item.get('uploadProgress') for item in operation['items'] if item.get('uploadProgress'))
            run.sample('slow-upload')
            if operation['state'] in ('completed', 'failed', 'partial', 'cancelled'):
                break
            require(time.monotonic() - started < 180, 'slow_upload_timeout')
            time.sleep(.25)
        require(observed_progress, 'slow_upload_progress_not_observed')
        verify_uploaded(run, operation)
        run.result['metrics']['slowUploadSeconds'] = time.monotonic() - started
        stop_owned_process_group(proxy)
        proxy.stdout.close()
        proxy, _ = start(stall=True, port=port)
        run.clipboard_command('file', path=str(path))
        cancelled_id = run.call('action.invoke', {'action': 'clipboard'})['operationId']
        time.sleep(3)
        run.event('stalled-operation', **operation_summary(run.call('operation.inspect', {'id': cancelled_id})))
        ui.action('stalled-input-response', lambda: ui.accessible.search('slow'), [0, 0, min(ui.width, 800), 180])
        ui.save('stalled-upload')
        tray(run, 'stalled')
        cancelled_at = time.monotonic()
        run.call('operation.cancel', {'id': cancelled_id})
        cancelled = run.wait_operation(cancelled_id, 30)
        assess(run, 'cancelTerminalMs', (time.monotonic() - cancelled_at) * 1000)
        require(cancelled['state'] == 'cancelled', 'stalled_upload_not_cancelled')
        run.event('stalled-cancelled', **operation_summary(cancelled))
        ui.save('stalled-cancelled')
        stop_owned_process_group(proxy)
        proxy.stdout.close()
        proxy, _ = start(port=port)
        run.call('operation.retry', {'id': cancelled_id})
        retried = run.wait_operation(cancelled_id, 180)
        verify_uploaded(run, retried)
        run.event('stalled-retry-succeeded', **operation_summary(retried))
        ui.save('stalled-retry-completed')
        # Close only the verified UI through the WM, before closed-family settle.
        subprocess.run(['wmctrl', '-ic', ui.window], check=True, timeout=5,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(2)
    finally:
        if ui:
            ui.draw.close()
        if proxy:
            stop_owned_process_group(proxy)
            proxy.stdout.close()
    settle(run, baseline)
