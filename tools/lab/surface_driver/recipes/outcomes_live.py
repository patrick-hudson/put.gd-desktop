"""Tier 1 Phase B: real region input, cloud bytes, copied link and UI cleanup.

Only builder guests admitted by #726 can resolve this lane's provider fixture.
No preuploaded result, fake endpoint, direct provider delete or account sign-in.
"""
import io
import json
import sys

from ..api import recipe
from ..browser import default_browser, open_and_verify, live_download_digest, live_link_unavailable
from ..catalog import image_modules
from ..fixtures.live_outcomes import provider, verify_s3
from . import history, outcomes, selectors


def _owned(ctx, service, detail, oid):
    """Do not let a failed byte oracle's finalizer delete a foreign receipt."""
    receipt = detail['receipt']
    destination = receipt['destination']
    ctx.expect(detail['operationId'] == oid and destination['profileId'] == service.profile_id
               and destination['credentials']['reference'] == service.credential_reference,
               'cleanup receipt belongs to this exact operation and bridged destination')
    if ctx.destination == 's3':
        ctx.expect(destination['providerId'] == 's3' and destination['settings']['objectPrefix'] == service.prefix
                   and receipt['objectReference']['key'].startswith(service.prefix),
                   'cleanup cannot leave the new S3 campaign prefix')
    else:
        ctx.expect(destination['providerId'] == 'proton-drive'
                   and destination['settings']['folderUid'] == service.folder_uid,
                   'cleanup cannot leave the newly created Proton folder')


def _cleanup(ctx, detail):
    """Reuse #687's exact-row UI deletion; Proton uses its supported revoke UI."""
    outcomes._history(ctx, detail)
    if ctx.destination == 'proton':
        ctx.click('revokeHistoryLink', window='history')
        previous = ctx.profile.get('windows', {})
        # Ui::confirm is a QMessageBox (native role "alert"), whereas the
        # S3 deletion preparation is a QDialog. Resolve the exact owned title
        # without imposing the deletion dialog's incompatible role.
        ctx.profile['windows'] = {**previous, 'confirmAction': {'title': '^Revoke share link$'}}
        try:
            history._wait_window(ctx, 'confirmAction')
            ctx.frame('revoke-exact-owned-link', window='confirmAction')
            history._confirm_button(ctx, 'confirmAction', 'Revoke link')
        finally:
            ctx.profile['windows'] = previous
        final = ctx.wait(lambda: (d if (d := ctx.call('history.detail', {'id': detail['id']})).get('shareStatus')
                                 in ('revoked', 'failed', 'unknown', 'changed') else None), 'UI revocation settled', timeout=180)
        ctx.expect(final['shareStatus'] == 'revoked' and not final['linkAvailable'], 'History confirms link revoked')
        ctx.note('revocation_count=1')
    else:
        ctx.click('deleteHistoryRemote', window='history')
        history._wait_window(ctx, 'confirmAction')
        ctx.call('ui.history', {'action': 'copyDeletionDetails'})
        prepared = json.loads(ctx.clipboard_text())
        ctx.expect([t['historyId'] for t in prepared['targets']] == [detail['id']]
                   and bool(prepared['acknowledgement']), 'deletion confirms exact owned history ID and effects')
        ctx.frame('delete-exact-owned-object', window='confirmAction')
        history._confirm_button(ctx, 'confirmAction', 'Delete 1 file')
        final = ctx.wait(lambda: (d if (d := ctx.call('history.detail', {'id': detail['id']})).get('remoteStatus')
                                 in ('removed', 'already_missing', 'failed', 'unknown', 'changed', 'version_retained',
                                     'hidden_version_retained', 'unsupported_safe_removal') else None),
                         'UI deletion settled', timeout=180)
        ctx.expect(final['remoteStatus'] in ('removed', 'already_missing') and not final['linkAvailable'],
                   'History confirms object removal')
        ctx.note('deletion_count=1')
    ctx.frame('history-after-remote-cleanup', window='history')


def _published(ctx, oid, pending):
    op = ctx.wait(lambda: (v if (v := ctx.call('operation.inspect', {'id': oid}))['state']
                         in ('completed', 'partial', 'failed', 'cancelled') else None), 'live publication settled', timeout=180)
    if op['state'] != 'completed' and not op.get('items') and op.get('error', {}).get('code') == 'disk_space_insufficient':
        pending['remote'] = False
        ctx.expect(False, 'capture failed before publication: disk_space_insufficient (zero items)')
    records = ctx.wait(lambda: [r for r in ctx.call('history.list', {'limit': 100})['records']
                               if r.get('operationId') == oid], 'live publication has durable history')
    ctx.expect(len(records) == 1, 'one live capture has exactly one durable row')
    detail = ctx.call('history.detail', {'id': records[0]['id']})
    ctx.note('operationId='+oid)
    ctx.note('historyId='+detail['id'])
    # Register cleanup even for partial publication, before checking its outcome.
    # Never clear a row whose remote publication remains unresolved.
    return op, detail


def _region_upload(ctx):
    ctx.fixture('app-profile')
    scope = ctx._fixture_manager.scope
    service = provider(scope)
    scene, oracle = outcomes._pattern(ctx)
    detail, cleaned, owned = None, False, False
    try:
        with selectors._portal_scope(ctx):
            x, y, w, h = scene.rect
            rect = (x+20, y+20, x+w-20, y+h-20)
            service.pending['remote'] = True
            oid = selectors._open_region(ctx, accessible=False)
            ctx.drag(rect[:2], rect[2:])
            op, detail = _published(ctx, oid, service.pending)
            _owned(ctx, service, detail, oid)
            owned = True
        ctx.expect(op['state'] == 'completed' and detail['uploadStatus'] == 'uploaded'
                   and detail['local']['status'] == 'saved', 'real capture saved and uploaded')
        digest = scope.json('import hashlib,json\nfrom pathlib import Path\nprint(json.dumps(hashlib.sha256(Path('
                            +repr(detail['local']['path'])+').read_bytes()).hexdigest()))')
        data = outcomes._payload(ctx, detail['local']['path'], 'png', digest)
        Image, _, _ = image_modules()
        ctx.expect(outcomes.pixels_equal(Image.open(io.BytesIO(data)), oracle.crop(rect)),
                   'live region pixels equal independently captured pattern crop (tolerance=0)')
        if ctx.destination == 's3':
            verify_s3(ctx, service, detail, data)
        with default_browser(ctx, 'firefox'):
            previous = ctx.profile.get('windows', {})
            ctx.profile['windows'] = {**previous, 'outcome-browser': {'title': '.*Firefox.*'}}
            try:
                outcomes._history(ctx, detail)
                ctx.click('copyHistoryLink', window='history')
                ctx.expect(ctx.clipboard_text() == detail['receipt']['directUrl'], 'UI copied exact uploaded share link')
                ctx.click('windowClose', window='history')
                def paste():
                    ctx.focus('outcome-browser', raise_window=False)
                    ctx.keys('Ctrl+L', 'Ctrl+V', 'Return')
                opened = open_and_verify(ctx, paste, {'kind': ctx.destination, 'name': detail['name'], 'preview': True})
                ctx.expect(opened['matched'] and opened['frames'], 'guarded Firefox displays the copied link')
                if ctx.destination == 'proton':
                    downloaded = live_download_digest(ctx)
                    ctx.expect(downloaded['sha256'] == digest and downloaded['bytes'] == len(data),
                               'Proton recipient download SHA-256 equals saved file')
                ctx.note('remote_hash_match_count=1')
                # Cleanup is inside the browser guard: its URL stays only in
                # the private Firefox memory for the fresh negative check.
                _cleanup(ctx, detail)
                cleaned = True
                service.pending['remote'] = False
                if ctx.destination == 's3':
                    verify_s3(ctx, service, detail, absent=True)
                ctx.expect(live_link_unavailable(ctx, ctx.destination)['unavailable'],
                           'fresh link navigation no longer resolves after UI cleanup')
                ctx.note('unavailable_link_count=1')
            finally:
                ctx.profile['windows'] = previous
    finally:
        if detail and owned and not cleaned and detail.get('uploadStatus') == 'uploaded':
            failed = sys.exc_info()[0] is not None
            try:
                _cleanup(ctx, detail)
                service.pending['remote'] = False
            except Exception as error:
                ctx.review('live UI cleanup failed: '+type(error).__name__)
                if not failed:
                    raise


@recipe('region.to-upload', 'region.drag', 'history.copy-link', 'history.open-link',
        'history.delete-remote', 'confirm.history-delete', destinations=('s3',), desktop_drawn=True, timeout=720)
def s3_region_upload(ctx):
    _region_upload(ctx)


@recipe('region.to-upload', 'region.drag', 'history.copy-link', 'history.open-link',
        'history.proton-sharing', 'confirm.revoke-link', destinations=('proton',), desktop_drawn=True, timeout=720)
def proton_region_upload(ctx):
    _region_upload(ctx)
