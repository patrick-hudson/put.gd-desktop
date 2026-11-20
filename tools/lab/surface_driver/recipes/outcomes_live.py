"""Tier 1 Phase B: real region input, cloud bytes, copied link and UI cleanup.

Only builder guests admitted by #726 can resolve this lane's provider fixture.
No preuploaded result, fake endpoint, direct provider delete or account sign-in.
"""
import io
import json
import sys
from urllib.parse import urlsplit

from ..api import recipe
from ..browser import default_browser, open_and_verify, live_download_digest, live_link_unavailable
from ..catalog import image_modules
from ..fixtures.live_outcomes import (R2_CUSTOM_DOMAIN, capture_name, provider, verify_presigned, verify_r2,
                                     verify_s3, reconcile_refused_s3)
from . import history, outcomes, selectors


def _owned(ctx, service, detail, oid):
    """Do not let a failed byte oracle's finalizer delete a foreign receipt."""
    receipt = detail['receipt']
    destination = receipt['destination']
    ctx.expect(detail['operationId'] == oid and destination['profileId'] == service.profile_id
               and destination['credentials']['reference'] == service.credential_reference,
               'cleanup receipt belongs to this exact operation and bridged destination')
    if ctx.destination == 's3':
        # Only a key inside this campaign's prefix may replace the marker's key; a rejected receipt leaves the
        # planned key in place. Then require it to be the one recorded before publishing (a unique prefix and
        # name leave nothing to rename).
        kind = 'cloudflare-r2' if service.get('r2') else 's3'
        if (destination['providerId'] == kind and destination['settings']['objectPrefix'] == service.prefix
                and receipt['objectReference']['key'].startswith(service.prefix)):
            service.pending.record_key(receipt['objectReference']['key'])
        ctx.expect(destination['providerId'] == kind and destination['settings']['objectPrefix'] == service.prefix
                   and receipt['objectReference']['key'].startswith(service.prefix),
                   'cleanup cannot leave the new S3 campaign prefix')
        ctx.expect(receipt['objectReference']['key'] == service.object_key,
                   'stored object key equals the exact key recorded before publication')
    else:
        ctx.expect(destination['providerId'] == 'proton-drive'
                   and destination['settings']['folderUid'] == service.folder_uid,
                   'cleanup cannot leave the newly created Proton folder')


DELETION_SETTLED = ('removed', 'already_missing', 'failed', 'unknown', 'changed', 'version_retained',
                    'hidden_version_retained', 'unsupported_safe_removal')


def _retry_deletion(ctx, current):
    """An unresolved delete leaves Retry deletion, not the first-delete button."""
    ctx.call('delete.retry', {'id': current['deleteOperationId']})
    ctx.note('deletion_retried=1')
    # The retry is answered before it runs, so the status it started from can still be read back.
    settled = [status for status in DELETION_SETTLED if status != current['remoteStatus']]
    return ctx.wait(lambda: (d if (d := ctx.call('history.detail', {'id': current['id']})).get('remoteStatus')
                             in settled else None), 'retried deletion settled', timeout=90)


def _delete_from_row_menu(ctx, detail):
    """Delete from destination… in the exact row's own menu, as history_delete does.

    On labwc every capture of History (the driver places it by its own pixels) puts its details
    pane back at the top, so the pane's button, below the window's end, cannot be scrolled to.
    """
    history._search(ctx, history._distinct_text(ctx, detail))
    ctx.wait(lambda: history._state(ctx)['rows'] == 1, 'the live row is the one row shown')
    node = history._searched_row_node(ctx)
    history._click_history_node(ctx, {'role': 'list item', 'name': node['name']})
    ctx.wait(lambda: history._state(ctx)['selectedIds'] == [detail['id']], 'the live row is selected with real input')
    history._focus_items(ctx)
    ctx.keys('Shift+F10')
    history._wait_menu(ctx, 'Delete from destination…')
    ctx.click('text:Delete from destination…', window='historyRowMenu')


def _cleanup(ctx, detail):
    """Reuse #687's exact-row UI deletion; Proton uses its supported revoke UI."""
    outcomes._history(ctx, detail)
    ctx.focus('history', raise_window=ctx.profile.get('session') == 'x11')
    box = ctx.geometry('history')
    # The remote actions live below the preview in the inspector. Scroll its
    # right side with guarded real input; older Qt does not export its ID. Below
    # the preview: resting on it shows its tooltip over the point (Hyprland).
    ctx.scroll(round(box['x']+.9*box['width']), round(box['y']+.85*box['height']), -16, head=box.get('head', 0))
    button = ({'name': 'Revoke link…', 'role': 'push button'} if ctx.destination == 'proton' else history.DELETE_BUTTON)
    # On labwc the read below captures History to place it, which puts the pane back at the top:
    # whatever it saw is gone by the click, so cleanup there always uses the row's own menu.
    labwc = ctx.profile.get('compositor', {}).get('kind') == 'toplevel-pixels'
    in_view = not labwc and ctx.accessible(dict(button, owned=True, visible=True, showing=True, pid=box.get('pid'),
                                                within=[box[k] for k in ('x', 'y', 'width', 'height')]))
    in_view = bool(in_view) and len(in_view) == 1 and bool(in_view[0].get('rect'))
    ctx.frame('history-before-remote-cleanup', window='history')
    if ctx.destination == 'proton':
        # Qt 6.4 does not publish objectName, and the pinned S3 catalog has
        # no Proton button. Bind the real owned button by its exact caption.
        ctx.click('text:Revoke link…', window='history')
        previous = ctx.profile.get('windows', {})
        # Ui::confirm is a QMessageBox (native role "alert"), whereas the
        # S3 deletion preparation is a QDialog. Require this owned alert's
        # exact native title and alert role instead of the generic dialog role.
        ctx.profile['windows'] = {**previous, 'confirmAction': {'title': '^Revoke share link$',
            'accessible': {'role': 'alert'}}}
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
        current = ctx.call('history.detail', {'id': detail['id']})
        if current.get('deleteOperationId') and current.get('remoteStatus') in ('failed', 'unknown'):
            final = _retry_deletion(ctx, current)
        else:
            if in_view:
                # Qt 6.4 does not expose this button's objectName. The exact caption
                # still binds the unique visible owned inspector action.
                ctx.click('text:Delete from destination…', window='history')
            else:
                _delete_from_row_menu(ctx, detail)
            history._wait_window(ctx, 'confirmAction')
            ctx.call('ui.history', {'action': 'copyDeletionDetails'})
            prepared = json.loads(ctx.clipboard_text())
            ctx.expect([t['historyId'] for t in prepared['targets']] == [detail['id']]
                       and bool(prepared['acknowledgement']), 'deletion confirms exact owned history ID and effects')
            ctx.frame('delete-exact-owned-object', window='confirmAction')
            history._confirm_button(ctx, 'confirmAction', 'Delete 1 file')
            final = ctx.wait(lambda: (d if (d := ctx.call('history.detail', {'id': detail['id']})).get('remoteStatus')
                                     in DELETION_SETTLED else None), 'UI deletion settled', timeout=180)
            if not in_view:
                # The search that picked the row would hide the next pass's row from History.
                history._search(ctx, '')
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
    if 'X-Amz-' in urlsplit(detail.get('receipt', {}).get('directUrl') or '').query:
        # History shows a signed link in full (key ID and signature): no frame for the rest of the recipe.
        ctx._frames_withheld = 'a signed link may be on screen'
    ctx.note('operationId='+oid)
    ctx.note('historyId='+detail['id'])
    # Register cleanup even for partial publication, before checking its outcome.
    # Never clear a row whose remote publication remains unresolved.
    return op, detail


def _saved_digest(scope, path):
    # Constrain the read before hashing: a bad local receipt must never cause
    # a credential/user file outside this fresh profile to be fingerprinted.
    return scope.json('''import hashlib,json,os,stat
from pathlib import Path
p=Path(PATH);root=Path(ROOT)
assert p.resolve(strict=True).is_relative_to(root.resolve(strict=True)) and not p.is_symlink()
with os.fdopen(os.open(p,os.O_RDONLY|os.O_NOFOLLOW|os.O_NONBLOCK),'rb') as stream:
 info=os.fstat(stream.fileno());assert stat.S_ISREG(info.st_mode) and info.st_uid==os.getuid()
 assert 0<info.st_size<=32*1024*1024
 data=stream.read(32*1024*1024+1)
assert data.startswith(b'\\x89PNG\\r\\n\\x1a\\n') and len(data)<=32*1024*1024
print(json.dumps(hashlib.sha256(data).hexdigest()))
'''.replace('PATH', repr(path)).replace('ROOT', repr(scope.ctx.target.root)))


def _preview_only_eligible(downloaded):
    # A completed file with the wrong hash is a failure, never an unproven hash.
    observations = downloaded.get('downloadObservations', [])
    return (not downloaded.get('matches') and 'sha256' not in downloaded
            and downloaded.get('preview', {}).get('matches') is True
            and bool(observations) and all(o.get('available') is True for o in observations))


def _copy_history_link(ctx, scope, detail):
    """Require the History action to replace a known text-only clipboard value."""
    from ..fixtures.clipboard import owner
    sentinel = scope.root + '/copy-link-sentinel'
    scope.write(sentinel, b'putgd synthetic copy-link sentinel')
    owner(scope, sentinel, 'text/plain', name='copy-link-sentinel')
    outcomes._history(ctx, detail)
    ctx.click('copyHistoryLink', window='history')
    ctx.wait(lambda: ctx.clipboard_text() == detail['receipt']['directUrl'],
             'UI copied exact uploaded share link')


def _region_upload(ctx):
    ctx.fixture('app-profile')
    scope = ctx._fixture_manager.scope
    service = ctx._live_service = provider(scope)
    outcomes._clipboard_reader(scope)
    scene, oracle = outcomes._pattern(ctx)
    if ctx.destination == 's3':
        resolved = next(p for p in ctx.call('settings.get')['document']['profiles'] if p['id'] == service.profile_id)
        ctx.expect(resolved['settings'].get('publicUrlBase') == 'https://put.gd'
                   and resolved['settings'].get('aclMode') == 'public-read'
                   and resolved['settings'].get('objectPrefix') == service.prefix,
                   'resolved S3 profile uses public-read, CloudFront and this exact acceptance prefix')
        ctx.note('s3_resolved_cloudfront_base_count=1')
    detail, cleaned, owned = None, False, False
    try:
        with selectors._portal_scope(ctx):
            x, y, w, h = scene.rect
            rect = (x+20, y+20, x+w-20, y+h-20)
            service.pending['remote'] = True
            oid = selectors._open_region(ctx, accessible=False)
            ctx.drag(rect[:2], rect[2:])
            op, detail = _published(ctx, oid, service.pending)
            if detail.get('uploadStatus') == 'uploaded':
                _owned(ctx, service, detail, oid)
                owned = True
            elif ctx.destination == 's3':
                reconcile_refused_s3(ctx, service, op, detail)
                code = detail.get('error', {}).get('code', '')
                if code == 's3_permission_denied':
                    ctx.note('upload_failure_code=s3_permission_denied')
        ctx.expect(op['state'] == 'completed' and detail['uploadStatus'] == 'uploaded'
                   and detail['local']['status'] == 'saved', 'real capture saved and uploaded')
        outcomes._auto_copy(ctx, scope, detail)
        ctx.note('upload_count=1')
        digest = _saved_digest(scope, detail['local']['path'])
        data = outcomes._payload(ctx, detail['local']['path'], 'png', digest)
        Image, _, _ = image_modules()
        ctx.expect(outcomes.pixels_equal(Image.open(io.BytesIO(data)), oracle.crop(rect)),
                   'live region pixels equal independently captured pattern crop (tolerance=0)')
        if ctx.destination == 's3':
            verify_s3(ctx, service, detail, data)
            ctx.note('remote_hash_match_count=1')
        with default_browser(ctx, 'firefox'):
            previous = ctx.profile.get('windows', {})
            ctx.profile['windows'] = {**previous, 'outcome-browser': {'title': '.*Firefox.*'}}
            try:
                _copy_history_link(ctx, scope, detail)
                ctx.click('windowClose', window='history')
                def paste():
                    ctx.focus('outcome-browser', raise_window=ctx.profile.get('session') == 'x11')
                    ctx.keys('Ctrl+L', 'Ctrl+V', 'Return')
                browser_error, preview_only = None, False
                try:
                    opened = open_and_verify(ctx, paste, {'kind': ctx.destination, 'name': detail['name'], 'preview': True})
                    ctx.expect(opened['matched'] and opened['frames'], 'guarded Firefox displays the copied link')
                    try:
                        downloaded = live_download_digest(ctx, ctx.destination, detail['local']['path'], filename=detail['name'])
                    except RuntimeError as error:
                        # The helper emits only our static, allowlisted errors.
                        ctx.note(str(error))
                        raise
                    if ctx.destination == 'proton':
                        ctx.note('firefox_download_list='+json.dumps(downloaded['downloadObservations'], sort_keys=True))
                        ctx.note('recipient_preview_pixels='+json.dumps(downloaded['preview'], sort_keys=True))
                        if _preview_only_eligible(downloaded):
                            ctx.expect(bool(downloaded['downloadObservations']) and
                                       all(o.get('available') for o in downloaded['downloadObservations']),
                                       'P-preview requires Firefox own Download-list state')
                            ctx.note('preview_match_count=1; recipient_proof=P-preview; original_byte_hash=unproven')
                            preview_only = True
                    if not preview_only:
                        ctx.expect(downloaded['matches'] and downloaded['sha256'] == digest and downloaded['bytes'] == len(data),
                                   'recipient original bytes SHA-256 equals saved file in guest')
                        if ctx.destination == 'proton':
                            ctx.note('browser_hash_match_count=1')
                        if ctx.destination == 's3':
                            ctx.note('cloudfront_positive_status='+str(downloaded['status']))
                            public = downloaded['guestGet']
                            ctx.expect(public['status'] == 200 and public['matches'] and public['sha256'] == digest
                                       and public['bytes'] == len(data), 'guest CloudFront GET 200 SHA-256 equals saved file')
                            ctx.note('guest_public_hash_match_count=1; guest_cloudfront_positive_status=200')
                        if ctx.destination == 'proton':
                            ctx.note('remote_hash_match_count=1')
                except Exception as error:
                    ctx.note('recipient_positive_failure='+ctx.evidence.redactor.exception(error))
                    browser_error = error
                # Cleanup is inside the browser guard: its URL stays only in
                # the private Firefox memory for the fresh negative check.
                _cleanup(ctx, detail)
                if ctx.destination == 's3':
                    verify_s3(ctx, service, detail, absent=True)
                cleaned = True
                service.pending['remote'] = False
                ctx.click('windowClose', window='history')
                if ctx.destination == 's3':
                    # Origin GET 404 above is decisive. CloudFront can retain
                    # the previously public image until its cache expires.
                    try:
                        negative = live_link_unavailable(ctx, 's3')
                        ctx.note('cloudfront_after_delete_status='+str(negative['status']))
                        if negative['status'] in (403,404,410):
                            ctx.note('unavailable_link_count=1')
                    except RuntimeError as error:
                        ctx.note('cloudfront_after_delete_informational='+ctx.evidence.redactor.exception(error))
                else:
                    negative = live_link_unavailable(ctx, 'proton')
                    ctx.expect(negative['unavailable'], 'fresh link navigation no longer resolves after UI cleanup')
                    ctx.note('unavailable_link_count=1')
                if browser_error is not None:
                    raise browser_error
                if preview_only:
                    ctx.blocked('P-preview: decrypted pixels match and Firefox Download list recorded; original-byte hash unproven; coordinator acceptance pending', issue=978)
            finally:
                ctx.profile['windows'] = previous
    finally:
        if detail and owned and not cleaned and detail.get('uploadStatus') == 'uploaded':
            failed = sys.exc_info()[0] is not None
            try:
                _cleanup(ctx, detail)
                if ctx.destination == 's3':
                    verify_s3(ctx, service, detail, absent=True)
                service.pending['remote'] = False
            except Exception as error:
                ctx.review('live UI cleanup failed: '+ctx.evidence.redactor.exception(error))
                if not failed:
                    raise


def _capture_once(ctx, service, scene, owned):
    """One real region capture of the pattern to the live destination; its History row and rectangle.

    OWNED receives the row as soon as it is proven to be this operation's, before anything else is
    asserted, so a later failure still deletes the object it published.
    """
    x, y, w, h = scene.rect
    rect = (x+20, y+20, x+w-20, y+h-20)
    with selectors._portal_scope(ctx):
        service.pending['remote'] = True
        oid = selectors._open_region(ctx, accessible=False)
        ctx.drag(rect[:2], rect[2:])
        op, detail = _published(ctx, oid, service.pending)
    if detail.get('uploadStatus') == 'uploaded':
        _owned(ctx, service, detail, oid)
        owned.append(detail)
    ctx.expect(op['state'] == 'completed' and detail['uploadStatus'] == 'uploaded'
               and detail['local']['status'] == 'saved', 'real capture saved and uploaded')
    return detail, rect


def _settle_extra(ctx, service, detail, gone):
    """Delete an extra live object through History and prove it gone (GONE); never hide a failed delete."""
    _cleanup(ctx, detail)
    gone(detail)
    service.pending['remote'] = False
    ctx.click('windowClose', window='history')


def _link_pass(ctx, settings, check, gone, *, oracle=None, signed=False):
    """Switch the live destination's link settings, publish one capture, CHECK it, delete it, GONE proves it.

    Each pass has its own object key, recorded in the cleanup marker before anything is published.
    With ORACLE (the frozen pattern frame) the saved pixels must also equal its crop. A SIGNED link
    carries the access key ID and a working signature, and History shows it in full: no frame is saved
    from the start of the pass to the end of the driver process, the runner's after-frame included.
    """
    scope = ctx._fixture_manager.scope
    service = ctx._live_service
    scene = ctx.fixture('selector-desktop')
    current = next(p for p in ctx.call('settings.get')['document']['profiles'] if p['id'] == service.profile_id)
    template, key = capture_name(service.prefix)
    service['object_key'] = key
    service.pending.details['objectKey'] = key
    scope.patch({'profiles': [dict(current, settings=dict(current['settings'], **settings))],
                 'naming': {'captureTemplate': template}})
    scene.command('freeze')
    scene.command('raise')
    if signed:
        ctx._frames_withheld = 'a signed link may be on screen'
    owned = []
    try:
        detail, rect = _capture_once(ctx, service, scene, owned)
        data = outcomes._payload(ctx, detail['local']['path'], 'png', _saved_digest(scope, detail['local']['path']))
        if oracle is not None:
            Image, _, _ = image_modules()
            ctx.expect(outcomes.pixels_equal(Image.open(io.BytesIO(data)), oracle.crop(rect)),
                       'live region pixels equal independently captured pattern crop (tolerance=0)')
        check(detail, data)
    finally:
        if owned:
            failed = sys.exc_info()[0] is not None
            try:
                # The frame guard stays set to the end of the recipe, after-frame included: closing
                # History is a click, and the window can still be on screen when the runner's frame comes.
                _settle_extra(ctx, service, owned[0], gone)
            except Exception as error:
                ctx.review('live UI cleanup failed: '+ctx.evidence.redactor.exception(error))
                if not failed:
                    raise
    return detail


def _presigned_upload(ctx):
    """#1366: the same S3 destination switched to expiring links publishes a signed link; unsigned is refused."""
    service = ctx._live_service
    _link_pass(ctx, dict(linkMode='presigned', linkValidityDays=1),
               lambda detail, data: verify_presigned(ctx, service, detail, data, host='s3.'+service.region+'.amazonaws.com'),
               lambda detail: verify_s3(ctx, service, detail, absent=True), signed=True)
    ctx.note('presigned_link_count=1')


def _live_flow(ctx, extra=None):
    try:
        _region_upload(ctx)
        if extra:
            extra(ctx)
    except BaseException:
        outcomes.note_app_error_banners(ctx)
        raise
    outcomes.no_app_error_banner(ctx)


def _r2_flow(ctx):
    """#1699: Cloudflare R2 public links on the custom domain and on r2.dev, then an expiring link.

    Each pass uploads a region capture, checks the copied link with an unauthenticated GET (and the
    object with the bucket-scoped key), deletes it from History and proves the origin no longer has it.
    """
    ctx.fixture('app-profile')
    scope = ctx._fixture_manager.scope
    service = ctx._live_service = provider(scope, r2=True)
    outcomes._clipboard_reader(scope)
    _, oracle = outcomes._pattern(ctx)
    for name, base in (('custom domain', R2_CUSTOM_DOMAIN), ('r2.dev', service.public_base)):
        def check(detail, data, base=base):
            outcomes._auto_copy(ctx, scope, detail)
            verify_r2(ctx, service, detail, data, base=base)
        _link_pass(ctx, dict(linkMode='public', publicUrlBase=base), check,
                   lambda detail, base=base: verify_r2(ctx, service, detail, absent=True, base=base),
                   oracle=oracle if name == 'custom domain' else None)
        ctx.note('r2_public_link_count=1; host='+base.split('//', 1)[1])
    def signed(detail, data):
        verify_r2(ctx, service, detail, data)
        # R2 refuses an unsigned S3 API request; the status it uses is not AWS's.
        verify_presigned(ctx, service, detail, data, host=service.account+'.r2.cloudflarestorage.com',
                         refused=(400, 401, 403))
    _link_pass(ctx, dict(linkMode='presigned', linkValidityDays=1), signed,
               lambda detail: verify_r2(ctx, service, detail, absent=True), signed=True)
    ctx.note('r2_presigned_link_count=1')


@recipe('region.to-upload', 'region.drag', 'history.copy-link', 'history.open-link',
        'history.delete-remote', 'confirm.history-delete', destinations=('s3',), desktop_drawn=True, timeout=840)
def s3_region_upload(ctx):
    _live_flow(ctx, _presigned_upload)


@recipe('region.to-upload', 'region.drag', 'history.delete-remote', 'confirm.history-delete',
        destinations=('s3',), desktop_drawn=True, timeout=600)
def s3_r2_region_upload(ctx):
    try:
        _r2_flow(ctx)
    except BaseException:
        outcomes.note_app_error_banners(ctx)
        raise
    outcomes.no_app_error_banner(ctx)


@recipe('region.to-upload', 'region.drag', 'history.copy-link', 'history.open-link',
        'history.proton-sharing', 'confirm.revoke-link', destinations=('proton',), desktop_drawn=True, timeout=720)
def proton_region_upload(ctx):
    _live_flow(ctx)
