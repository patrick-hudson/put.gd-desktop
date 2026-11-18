"""Recording review recipes. Fixtures are fresh, synthetic and private per recipe.

``recording`` supplies id, output_dir, working_path, frozen_path. The two clips
must be same-pattern moving/frozen captures, not generated stand-ins for capture.
``review-controls`` is the current build's 30-review.controls.json inventory.
``destination`` supplies id and label and configures local saving or an isolated
remote destination before recording.prepare snapshots configuration. No secrets
or publication URLs are written into notes. Error fixtures are opt-in needs.
"""
from __future__ import annotations

import json
import shlex
from pathlib import PurePosixPath

from tools.lab.surface_driver.api import recipe

REFERENCE = '30-review'


def _state(ctx, rid):
    return ctx.call('recording.review', {'id': rid, 'action': 'inspect'})


def _inspect(ctx, rid):
    return ctx.call('recording.inspect', {'id': rid})


def _frame(ctx, label, reference=REFERENCE, window='review'):
    return ctx.frame(label, window=window, reference=reference)


def _open(ctx, fixture_name='recording', *, frame_review=True):
    fixture = ctx.fixture(fixture_name)
    required = {'id', 'output_dir', 'working_path', 'frozen_path'}
    if not isinstance(fixture, dict) or not required.issubset(fixture):
        ctx.blocked('Core recording fixture must supply fresh id, output_dir, working_path and captured frozen_path (#672)', issue=672)
    rid = fixture['id']
    ctx.view('recording.review', {'id': rid, 'action': 'inspect'})
    info = _inspect(ctx, rid)
    ctx.expect(info['state'] == 'reviewing', 'fixture is in private review')
    ctx.expect(info['durationUs'] >= 4_000_000, 'moving recording lasts at least four seconds')
    state = _state(ctx, rid)
    ctx.expect(state['visible'] and not state['playing'], 'review is visible and paused')
    ctx.expect(not info['edit']['masks'] and info['edit']['trimStart'] == 0
               and info['edit'].get('trimEnd') in (None, info['durationUs']),
               'fresh fixture has no masks or trim')
    _capture_motion(ctx, fixture)
    if frame_review:
        _frame(ctx, 'before')
    else:
        ctx.frame('before')
        ctx.review('API full-desktop frames require visual review')
    return rid


def _changed(ctx, rid, revision):
    return ctx.wait(lambda: (s if (s := _inspect(ctx, rid))['edit']['revision'] > revision else None),
                    'resident accepts the real-input edit')


def _point(ctx, state, rect, fx=.5, fy=.5):
    g = ctx.geometry('review')
    if g.get('head', 0) != 0:
        ctx.blocked('Context.click_at/drag cannot select a non-primary head (#672)', issue=672)
    sx, sy = g['width'] / state['size'][0], g['height'] / state['size'][1]
    x, y, w, h = rect
    scale = g.get('scale', 1)
    return (round((g['x'] - g.get('screen_x', 0) + (x + w * fx) * sx) * scale),
            round((g['y'] - g.get('screen_y', 0) + (y + h * fy) * sy) * scale))


def _video(ctx, rid, fx=.5, fy=.5):
    s = _state(ctx, rid)
    return _point(ctx, s, s['video']['rect'], fx, fy)


def _drive(ctx, rid, action, **fields):
    """One recording.review control: the call an agent makes for the button, key or gesture."""
    return ctx.call('recording.review', {'id': rid, 'action': action, **fields})


def _select_time(ctx, rid, start, end):
    """The filmstrip drag, as fractions of the recording (recording.review selectRange)."""
    duration = _inspect(ctx, rid)['durationUs']
    state = _drive(ctx, rid, 'selectRange', start=int(duration * start), end=int(duration * end))
    ctx.expect(state['selection'] and state['selection']['start'] == int(duration * start), 'selectRange selects the span')
    return state


def _arm(ctx, rid, kind='solid'):
    state = _drive(ctx, rid, 'armRedact' if kind == 'solid' else 'armBlur')
    ctx.expect(state['armed'] == kind and state['placement']['kind'] == kind, kind + ' is armed with its placement banner')
    return state


def _add(ctx, rid, kind='solid', centered=False):
    before = _inspect(ctx, rid)['edit']
    _arm(ctx, rid, kind)
    if centered:
        ctx.keys('Return')
    else:
        ctx.drag(_video(ctx, rid, .2, .25), _video(ctx, rid, .4, .5))
    info = _changed(ctx, rid, before['revision'])
    ctx.expect(len(info['edit']['masks']) == len(before['masks']) + 1, 'one mask was placed')
    return info['edit']['masks'][-1]


def _strip(ctx, rid):
    """Window state and a function from source time to the strip's screen point."""
    s = _state(ctx, rid)
    strip, visible = s['timeline']['strip'], s['timeline']['visible']
    span = max(1, visible['end'] - visible['start'])
    def at(time, fy=.5):
        return _point(ctx, s, [strip[0] + (time - visible['start']) / span * strip[2], strip[1], 0, strip[3]], 0, fy)
    return s, at


def _seek(ctx, rid, fraction):
    duration = _inspect(ctx, rid)['durationUs']
    return _drive(ctx, rid, 'seek', time=int(duration * fraction))


def _select_mask(ctx, rid, mask):
    ctx.expect(_drive(ctx, rid, 'selectEdit', kind='cover', editId=mask['id'])['selectedEdit']['id'] == mask['id'], 'Edits selects the cover')


def _choose_format(ctx, rid, fmt, codec=''):
    # Prefer the stable name added by fix/surface-objectnames. For older builds
    # use the combo's accessible name, which differs from its selected item.
    controls = ctx.fixture('review-controls')['controls']
    named = next((c for c in controls if c.get('name') == 'format'), None)
    selector = 'format' if named else 'text:Output format'
    ctx.click(selector, window='review')
    caps = ctx.call('recording.capabilities')
    choices = [(f, c) for f in caps['formats'] for c in (caps['codecs'].get(f) or [''])]
    if (fmt, codec) not in choices:
        ctx.keys('Esc')
        ctx.blocked(f'{fmt}/{codec} is not offered by recording.capabilities')
    ctx.keys('Home', *(['Down'] * choices.index((fmt, codec))), 'Return')
    ctx.wait(lambda: _state(ctx, rid)['format'] == {'format': fmt, 'codec': codec}, 'format choice accepted')


# Run on the target. Decode actual stored bytes; no reliance on lastExport's
# duration/codec claim. All ffmpeg commands use -nostdin (SSH stdin is not media).
_MEDIA_CHECK = r'''
import json, math, subprocess, sys
from pathlib import Path
p = json.loads(sys.argv[1])
def run(args):
    return subprocess.run(args, check=True, capture_output=True, timeout=90).stdout
def probe(path):
    return json.loads(run(['ffprobe', '-v', 'error', '-show_streams', '-show_format', '-of', 'json', path]))
def image_frames(path):
    from PIL import Image
    im = Image.open(path)
    fs, duration = [], 0
    for n in range(im.n_frames):
        im.seek(n)
        im.load()
        duration += im.info.get('duration', 0) / 1000
        if n % max(1, im.n_frames//24) == 0:
            fs.append(im.convert('L').resize((96,64)).tobytes())
    assert len(fs) >= 2, 'animation has fewer than two frames'
    return im,fs,duration
def frames(path):
    if Path(path).suffix.lower() in ('.gif','.webp'):
        return image_frames(path)[1]
    raw = run(['ffmpeg', '-nostdin', '-v', 'error', '-i', path, '-an', '-vf',
               'fps=2,scale=96:64,format=gray', '-t', '12', '-f', 'rawvideo', '-'])
    assert len(raw) >= 2*96*64, 'clip has fewer than two decoded frames'
    return [raw[n:n+96*64] for n in range(0,len(raw)-96*64+1,96*64)]
def motion(path):
    fs = frames(path)
    return sum(sum(abs(a-b) for a,b in zip(x,y))/len(x) for x,y in zip(fs,fs[1:]))/(len(fs)-1)
active, frozen = motion(p['working']), motion(p['frozen'])
assert frozen < .5 and active > max(.8, frozen*3 + .2), ('motion control',active,frozen)
if p.get('captureOnly'):
    print(json.dumps({'movingMotion':active,'frozenMotion':frozen}))
    sys.exit(0)
path = p['path']
assert Path(path).is_file() and Path(path).stat().st_size > 0, 'export missing or empty'
animation = p['container'] in ('gif','webp')
if animation:
    image,fs,duration = image_frames(path)
    assert image.format.lower() == p['container'], image.format
    video = {'codec_name': image.format.lower(), 'width': image.width, 'height': image.height}
    meta = {'format': {'format_name': image.format.lower()}}
else:
    meta = probe(path)
    video = next(s for s in meta['streams'] if s['codec_type']=='video')
    duration = float(meta['format'].get('duration') or video.get('duration') or 0)
assert math.isfinite(duration) and abs(duration-p['duration']) <= .25, (duration,p['duration'])
assert video['codec_name'] == p['codec'], video['codec_name']
assert p['container'] in meta['format']['format_name'].split(','), meta['format']['format_name']
export_motion = motion(path)
assert export_motion > max(.5, frozen*3 + .1), ('frozen export',export_motion,frozen)
for mask in p['masks']:
    if mask['kind'] != 'solid': continue
    start = max(mask['start'], p['trimStart'])
    end = min(mask['end'], p['trimEnd'])
    if end-start < 100000: continue
    x,y,w,h = mask['rect']
    cw,ch = p['canvas']
    sx,sy = video['width']/cw,video['height']/ch
    x,y,w,h = round((x+3)*sx),round((y+3)*sy),max(2,round((w-6)*sx)),max(2,round((h-6)*sy))
    for fraction in (.25,.5,.75):
        t = (start+(end-start)*fraction-p['trimStart'])/1000000
        if animation:
            from PIL import Image
            im = Image.open(path)
            elapsed = 0
            for n in range(im.n_frames):
                im.seek(n)
                elapsed += im.info.get('duration',0)/1000
                if elapsed > t: break
            pix = im.convert('RGB').crop((x,y,x+w,y+h)).tobytes()
        else:
            pix = run(['ffmpeg','-nostdin','-v','error','-i',path,'-ss',str(t),'-frames:v','1',
                   '-vf',f'crop={w}:{h}:{x}:{y},format=rgb24','-f','rawvideo','-'])
        assert len(pix)>=w*h*3, 'masked frame did not decode'
        assert sum(pix)/len(pix) < 8 and max(pix) < 40, 'solid mask leaked source pixels'
        # A naturally black source region is not evidence of a working mask.
        source_time = (start+(end-start)*fraction)/1000000
        ox,oy,ow,oh = mask['rect']
        source = run(['ffmpeg','-nostdin','-v','error','-i',p['working'],'-ss',str(source_time),
                      '-frames:v','1','-vf',f'crop={ow-6}:{oh-6}:{ox+3}:{oy+3},format=rgb24',
                      '-f','rawvideo','-'])
        assert source and sum(source)/len(source) > 20, 'source mask region must contain visible pattern'
        assert sum(source)/len(source) - sum(pix)/len(pix) > 15, 'mask did not change source pixels'
print(json.dumps({'duration':duration,'codec':video['codec_name'],'container':meta['format']['format_name'],
                  'movingMotion':active,'frozenMotion':frozen,'exportMotion':export_motion,'masked':len(p['masks'])}))
'''


def _capture_motion(ctx, fixture):
    params = {'working': fixture['working_path'], 'frozen': fixture['frozen_path'], 'captureOnly': True}
    result = ctx.shell('python3 - ' + shlex.quote(json.dumps(params)) + " <<'REVIEW_MEDIA_PY'\n"
                       + _MEDIA_CHECK + '\nREVIEW_MEDIA_PY', timeout=120)
    ctx.expect(result['returncode'] == 0, 'recorded pattern moves and captured frozen control stays still')
    ctx.note('Capture motion metrics: ' + result['stdout'].strip())


def _verify(ctx, rid, path, info=None):
    info = info or _inspect(ctx, rid)
    fixture = ctx.fixture('recording')
    edit, config = info['edit'], info['config']
    end = edit.get('trimEnd') or info['durationUs']
    codec = {'h264': 'h264', 'h265': 'hevc', 'vp9': 'vp9', '': config['format']}[config.get('codec', '')]
    params = {'path': path, 'working': fixture['working_path'], 'frozen': fixture['frozen_path'],
              'duration': (end - edit['trimStart']) / 1_000_000, 'codec': codec,
              'container': {'mp4': 'mp4', 'webm': 'webm', 'gif': 'gif', 'webp': 'webp'}[config['format']],
              'masks': edit['masks'], 'trimStart': edit['trimStart'], 'trimEnd': end, 'canvas': info['canvas']}
    result = ctx.shell('python3 - ' + shlex.quote(json.dumps(params)) + " <<'REVIEW_MEDIA_PY'\n"
                       + _MEDIA_CHECK + '\nREVIEW_MEDIA_PY', timeout=180)
    ctx.expect(result['returncode'] == 0, 'decoded output passes duration, codec, motion and mask pixel checks')
    ctx.note('Decoded export metrics: ' + result['stdout'].strip())


def _saved(ctx, rid, path):
    info = ctx.wait(lambda: (s if (s := _inspect(ctx, rid))['state'] == 'reviewing'
                            and (s.get('lastExport') or {}).get('path') == path
                            and (s.get('lastExport') or {}).get('status') in ('saved', 'failed') else None),
                    'export finishes at requested path', timeout=180)
    ctx.expect(info['lastExport'].get('path') == path, 'copy export matches requested path')
    ctx.expect(info['lastExport'].get('revision') == info['edit']['revision'], 'copy export matches current edit revision')
    ctx.expect(info['lastExport']['status'] == 'saved', 'copy export succeeded')
    _verify(ctx, rid, info['lastExport']['path'], info)
    return info


def _export(ctx, rid, name='review-check'):
    info = _inspect(ctx, rid)
    path = str(PurePosixPath(ctx.fixture('recording')['output_dir']) / (name + '.' + info['config']['format']))
    ctx.call('recording.saveCopy', {'id': rid, 'path': path, 'overwrite': False})
    return _saved(ctx, rid, path)


@recipe('review.play', 'review.speed', needs=('recording',))
def playback(ctx):
    rid = _open(ctx)
    ctx.click('speed', window='review')
    ctx.keys('Home', 'Down', 'Down', 'Return')
    ctx.wait(lambda: _state(ctx, rid)['speed'] == 2, '2x playback selected')
    at = _state(ctx, rid)['playheadUs']
    ctx.click('play', window='review')
    ctx.wait(lambda: (s := _state(ctx, rid))['playing'] and s['playheadUs'] > at, 'playhead advances')
    ctx.keys('Space')
    ctx.wait(lambda: not _state(ctx, rid)['playing'], 'Space pauses playback')
    ctx.expect(_inspect(ctx, rid)['state'] == 'reviewing', 'playback remains private')
    _frame(ctx, 'after')


@recipe('review.timeline', 'review.cut', 'review.undo-redo', 'review.edit-step', needs=('recording',))
def cut_flow(ctx):
    rid = _open(ctx)
    duration = _inspect(ctx, rid)['durationUs']
    # The real gesture: a drag on the filmstrip selects time, C cuts it.
    s, at = _strip(ctx, rid)
    before = _inspect(ctx, rid)['edit']
    ctx.drag(at(duration * .3), at(duration * .45))
    s = ctx.wait(lambda: (v if (v := _state(ctx, rid))['selection'] else None), 'a filmstrip drag selects time')
    ctx.expect(s['selection']['end'] > s['selection']['start'] and s['panel']['mode'] == 'selection' and s['tools']['cut'],
               'the selection enables Cut and the panel offers it')
    _frame(ctx, 'selected', None)
    ctx.keys('C')
    info = _changed(ctx, rid, before['revision'])
    cut = info['edit']['cuts'][-1]
    s = _state(ctx, rid)
    ctx.expect(len(info['edit']['cuts']) == 1, 'C cut the selection')
    ctx.expect(s['outputDurationUs'] < duration and s['selection'] is None, 'the cut shortens the output and clears the selection')
    # Select the cut as a click does, then Restore it; Undo and Redo step the history.
    state = _drive(ctx, rid, 'selectCut', cutId=cut['id'])
    ctx.expect(state['selectedEdit'] == {'kind': 'cut', 'id': cut['id']} and state['panel']['mode'] == 'cut', 'a selected cut shows Restore and Play across')
    _drive(ctx, rid, 'adjustCut', cutId=cut['id'], start=cut['start'], end=min(duration - 1, cut['end'] + 200_000))
    info = _changed(ctx, rid, info['edit']['revision'])
    ctx.expect(info['edit']['cuts'][-1]['end'] > cut['end'], 'adjustCut moves the cut edge')
    _drive(ctx, rid, 'restoreCut', cutId=cut['id'])
    info = _changed(ctx, rid, info['edit']['revision'])
    ctx.expect(not info['edit']['cuts'], 'Restore removes the cut')
    ctx.expect(_state(ctx, rid)['undo']['canUndo'], 'the history can step back')
    _drive(ctx, rid, 'undo')
    info = _changed(ctx, rid, info['edit']['revision'])
    ctx.expect(len(info['edit']['cuts']) == 1, 'Undo brings the cut back')
    _drive(ctx, rid, 'redo')
    info = _changed(ctx, rid, info['edit']['revision'])
    ctx.expect(not info['edit']['cuts'], 'Redo restores it again')
    # Edits in source order: Alt+Right / Alt+Left.
    _drive(ctx, rid, 'undo')
    info = _changed(ctx, rid, info['edit']['revision'])
    _drive(ctx, rid, 'clearSelection')
    ctx.expect(_drive(ctx, rid, 'nextEdit')['selectedEdit']['kind'] == 'cut', 'nextEdit selects the first edit')
    ctx.expect(_state(ctx, rid)['edits'], 'inspect lists the Edits rows')
    _frame(ctx, 'after', None)


@recipe('review.trim-in', 'review.trim-out', 'review.trim-reset', 'review.key-timeline', 'review.frame-step', needs=('recording',))
def trim(ctx):
    rid = _open(ctx)
    duration = _inspect(ctx, rid)['durationUs']
    before = _inspect(ctx, rid)['edit']
    _drive(ctx, rid, 'setTrim', start=int(duration * .1), end=int(duration * .9))
    info = _changed(ctx, rid, before['revision'])
    ctx.expect(info['edit']['trimStart'] > 0 and info['edit']['trimEnd'] < duration, 'setTrim sets both brackets')
    _seek(ctx, rid, .25)
    ctx.keys('I')
    ctx.wait(lambda: _inspect(ctx, rid)['edit']['trimStart'] > info['edit']['trimStart'], 'I trims the start at the playhead')
    _seek(ctx, rid, .75)
    ctx.keys('O')
    ctx.wait(lambda: _inspect(ctx, rid)['edit']['trimEnd'] < info['edit']['trimEnd'], 'O trims the end at the playhead')
    _export(ctx, rid)
    _seek(ctx, rid, .5)
    ctx.keys('Home')
    start = _state(ctx, rid)
    ctx.expect(abs(start['playhead']['sourceUs'] - start['trim']['start']) < 100_000, 'Home jumps to the first kept frame')
    ctx.keys('Right')
    stepped = _state(ctx, rid)['playhead']['sourceUs']
    ctx.expect(abs(stepped - start['playhead']['sourceUs'] - 100_000) < 40_000, 'Right steps a tenth of a second')
    ctx.keys('Shift+Left')
    ctx.expect(abs(_state(ctx, rid)['playhead']['sourceUs'] - stepped + 1_000_000) < 40_000 or _state(ctx, rid)['playhead']['sourceUs'] == 0,
               'Shift+Left steps one second')
    at = _state(ctx, rid)['playhead']['sourceUs']
    after = _drive(ctx, rid, 'frameStep', delta=1)['playhead']['sourceUs']
    ctx.expect(0 < after - at < 100_000, 'frameStep moves one frame')
    ctx.expect(_drive(ctx, rid, 'frameStep', delta=-1)['playhead']['sourceUs'] == at, 'frameStep -1 returns to the frame')
    ctx.keys('End')
    end = _state(ctx, rid)
    ctx.expect(abs(end['playhead']['sourceUs'] - end['trim']['end']) < 100_000, 'End jumps to the last kept frame')
    _drive(ctx, rid, 'selectEdit', kind='trim')
    _drive(ctx, rid, 'resetTrim')
    ctx.wait(lambda: (s := _inspect(ctx, rid))['edit']['trimStart'] == 0 and s['edit']['trimEnd'] is None, 'Reset restores the whole clip')
    _frame(ctx, 'after')


@recipe('review.add-mask', 'review.frame', 'review.mask-disarm', 'review.placement-cancel',
        'review.masks', 'review.mask-in-out', 'review.mask-delete', 'review.key-frame-nudge', needs=('recording',))
def masks(ctx):
    rid = _open(ctx)
    # Arm, then cancel: the Select tool, the banner's Cancel and Esc all leave no edit.
    _arm(ctx, rid)
    _frame(ctx, 'placing', None)
    ctx.keys('Esc')
    ctx.expect(_state(ctx, rid)['armed'] is None, 'Escape cancels placing')
    _arm(ctx, rid, 'blur')
    ctx.expect(_state(ctx, rid)['warning'], 'arming Blur shows the reversible-blur warning')
    _drive(ctx, rid, 'cancelPlacement')
    ctx.expect(_state(ctx, rid)['placement'] is None, 'cancelPlacement leaves the placement')
    # Drawing the box with the pointer places the cover: one edit.
    mask = _add(ctx, rid)
    state = _state(ctx, rid)
    ctx.expect(state['selectedEdit'] == {'kind': 'cover', 'id': mask['id']} and state['panel']['mode'] == 'cover', 'the new cover is selected')
    _select_mask(ctx, rid, mask)
    ctx.keys('Right', 'Ctrl+Right', 'Shift+Down')
    def nudged():
        info = _inspect(ctx, rid)
        rect = info['edit']['masks'][0]['rect']
        return info if rect[0] == mask['rect'][0] + 11 and rect[3] == mask['rect'][3] + 1 else None
    info = ctx.wait(nudged, 'all three frame key edits reach the resident')
    # The agent's box: placeMask draws while armed, moves the selected cover otherwise.
    _arm(ctx, rid, 'blur')
    canvas = info['canvas']
    _drive(ctx, rid, 'placeMask', rect=[canvas[0] // 2, canvas[1] // 2, canvas[0] // 4, canvas[1] // 4])
    info = _changed(ctx, rid, info['edit']['revision'])
    ctx.expect(len(info['edit']['masks']) == 2 and info['edit']['masks'][-1]['kind'] == 'blur', 'placeMask while armed draws the box')
    # The covers' times: Shows from / Until and Set to playhead.
    _select_mask(ctx, rid, info['edit']['masks'][0])
    first = _drive(ctx, rid, 'seekCoverStart')['playhead']['sourceUs']
    last = _drive(ctx, rid, 'seekCoverEnd')['playhead']['sourceUs']
    ctx.expect(last > first, 'Shows from and Until seek to the first and last covered frame')
    _seek(ctx, rid, .2)
    ctx.keys('Shift+I')
    ctx.wait(lambda: _inspect(ctx, rid)['edit']['masks'][0]['start'] > 0, 'Shift+I sets the cover start')
    _seek(ctx, rid, .8)
    _drive(ctx, rid, 'setCoverEdgeToPlayhead', edge='end')
    ctx.wait(lambda: _inspect(ctx, rid)['edit']['masks'][0]['end'] < _inspect(ctx, rid)['durationUs'], 'Set to playhead sets the cover end')
    _export(ctx, rid)
    # Delete removes the selected cover; Esc clears; the Select tool (V) ends placing.
    count = len(_inspect(ctx, rid)['edit']['masks'])
    ctx.keys('Delete')
    ctx.wait(lambda: len(_inspect(ctx, rid)['edit']['masks']) == count - 1, 'Delete removes the selected cover')
    _add(ctx, rid, centered=True)
    ctx.click_at(*_video(ctx, rid, .05, .05))
    ctx.expect(_state(ctx, rid)['selectedEdit'] is None, 'an empty frame click clears the selection')
    _frame(ctx, 'after')


@recipe('review.mask-options', 'review.picture-zoom', needs=('recording',))
def edits_panel(ctx):
    rid = _open(ctx)
    mask = _add(ctx, rid, 'blur')
    ctx.expect(_state(ctx, rid)['warning'], 'a selected blur shows the warning')
    _drive(ctx, rid, 'coverKind', kind='solid')
    ctx.wait(lambda: _inspect(ctx, rid)['edit']['masks'][0]['kind'] == 'solid', 'the panel\'s Redact converts the blur')
    ctx.expect(_state(ctx, rid)['selectedEdit']['id'] == mask['id'], 'the conversion keeps the cover selected')
    # Picture zoom: Fit and 100 %, pan at 100 %.
    ctx.expect(_drive(ctx, rid, 'viewerZoom', mode='actual')['viewer']['zoom'] == 'actual', 'viewerZoom actual shows 100 %')
    _drive(ctx, rid, 'viewerPan', dx=40, dy=0)
    ctx.expect(_drive(ctx, rid, 'viewerZoom', mode='fit')['viewer']['zoom'] == 'fit', 'viewerZoom fit restores Fit')
    # Fold and unfold: below ~1180x720 the panel becomes the Edits overlay.
    wide = _state(ctx, rid)['size']
    _drive(ctx, rid, 'resize', width=1000, height=680)
    ctx.wait(lambda: _state(ctx, rid)['folded'], 'a small window folds the panel')
    opened = _drive(ctx, rid, 'panel', open=True)
    ctx.expect(opened['panel']['overlayOpen'], 'the Edits toggle opens the overlay')
    _frame(ctx, 'overlay', None)
    ctx.expect(not _drive(ctx, rid, 'panel', open=False)['panel']['overlayOpen'], 'the overlay closes')
    _drive(ctx, rid, 'resize', width=wide[0], height=wide[1])
    ctx.wait(lambda: not _state(ctx, rid)['folded'], 'the panel unfolds in a large window')


@recipe('review.wheel-lanes', 'review.timeline-zoom', needs=('recording',))
def wheel_lanes(ctx):
    rid = _open(ctx)
    for _ in range(5):
        _add(ctx, rid, centered=True)
    s = _state(ctx, rid)
    ctx.expect(s['timeline']['lanes'] == 2 and s['timeline']['hiddenCovers'] >= 1, 'overlapping covers use two lanes and a +n chip')
    ctx.expect(_drive(ctx, rid, 'timelineZoom', level=4)['timeline']['zoom'] == 4, 'timelineZoom zooms the strip')
    visible = _state(ctx, rid)['timeline']['visible']
    ctx.expect(visible['end'] - visible['start'] < _inspect(ctx, rid)['durationUs'], 'a zoomed strip shows part of the recording')
    ctx.expect(_drive(ctx, rid, 'timelineFit')['timeline']['zoom'] == 1, 'timelineFit shows the whole recording')
    ctx.expect(len(_inspect(ctx, rid)['edit']['masks']) == 5, 'zooming preserves the covers')
    _frame(ctx, 'after', None)


@recipe('review.format', needs=('recording', 'review-controls'), timeout=600)
def formats(ctx):
    rid = _open(ctx)
    caps = ctx.call('recording.capabilities')
    for fmt in caps['formats']:
        for codec in caps['codecs'].get(fmt) or ['']:
            _choose_format(ctx, rid, fmt, codec)
            ctx.expect(_inspect(ctx, rid)['config']['format'] == fmt, 'resident accepts format')
            _export(ctx, rid, 'format-' + fmt + '-' + codec)
    _frame(ctx, 'after')


@recipe('review.save-copy', needs=('recording',))
def save_copy(ctx):
    rid = _open(ctx)
    ctx.click('text:Save a copy…', window='review')
    ctx.wait(lambda: _state(ctx, rid)['fileDialog']['open'], 'Save a copy opens file chooser')
    ctx.focus('fileDialog')
    info = _inspect(ctx, rid)
    path = str(PurePosixPath(ctx.fixture('recording')['output_dir']) / ('user-copy.' + info['config']['format']))
    ctx.keys('Ctrl+L')
    ctx.type_text(path)
    ctx.keys('Return')
    info = _saved(ctx, rid, path)
    ctx.expect(info['lastExport']['path'] == path, 'chooser exports to chosen absolute path')
    s = _state(ctx, rid)
    ctx.expect(s['savedCopy']['current'] and s['visible'], 'saved copy keeps private review open')
    _frame(ctx, 'saved', '49-review-saved-copy')
    ctx.click('text:Copy path', window='review')
    ctx.expect(ctx.clipboard_text() == path, 'Copy path copies actual saved path')
    ctx.click('text:Dismiss', window='review')
    ctx.wait(lambda: not any(a['drive'].get('action') == 'copyPath'
                            for row in _state(ctx, rid)['statusFacts'] for a in row['actions']),
             'Dismiss hides saved-copy result')
    _frame(ctx, 'after')



@recipe('review.status-actions', needs=('recording', 'recording-export-failure'))
def status_actions(ctx):
    rid = _open(ctx, 'recording-export-failure')
    fixture = ctx.fixture('recording-export-failure')
    ctx.expect(_inspect(ctx, rid)['lastExport']['status'] == 'failed', 'fixture has a genuine failed export')
    _frame(ctx, 'export-failed', '39-review-status-errors')
    ctx.click('text:Try again', window='review')
    _saved(ctx, rid, fixture['retry_path'])
    ctx.click('text:Copy path', window='review')
    ctx.expect(ctx.clipboard_text() == fixture['retry_path'], 'retry result copies the saved path')
    ctx.click('text:Dismiss', window='review')
    ctx.wait(lambda: not _state(ctx, rid)['savedCopy']['path'], 'Dismiss clears retry result')
    _frame(ctx, 'after')
    ctx.blocked('Try again, Copy path and Dismiss exercised; Show in folder needs guarded opener (#682), size/format recovery needs an over-limit fixture (#672)', issue=672)


@recipe('review.destination-menu', 'review.upload', needs=('recording', 'destination'),
        destinations=('local', 's3', 'proton'), timeout=240)
def upload(ctx):
    if ctx.placement == 'boat' and ctx.destination != 'local':
        ctx.blocked('Boat forbids S3 credentials and Proton sessions; use a configured builder destination')
    rid = _open(ctx)
    destination = ctx.fixture('destination')
    ctx.click('uploadDestinationMenu', window='review')
    # This menu describes the immutable destination snapshot, rather than selecting another one.
    entries = [e for e in ctx.accessible({'owned': True, 'visible': True, 'showing': True}) if e.get('popup')]
    if not entries:
        ctx.blocked('Review destination popup is not exposed by the live accessibility resolver (#672)', issue=672)
    ctx.expect(any(destination['label'] in e.get('name', '') for e in entries), 'destination menu names configured destination')
    ctx.keys('Esc')
    before = _inspect(ctx, rid)
    ctx.click('upload', window='review')
    op = ctx.wait(lambda: (s if (s := ctx.call('operation.inspect', {'id': rid}))['state']
                          in ('completed', 'failed', 'cancelled') else None), 'explicit Upload finishes', timeout=180)
    ctx.expect(op['state'] == 'completed', 'Upload pipeline completes')
    # A destination fixture provides the local artifact path for the terminal
    # operation; live remote verification belongs to the configured fixture.
    paths = [i['local']['path'] for i in op.get('items', []) if i.get('local', {}).get('status') == 'saved']
    ctx.expect(bool(paths), 'upload retains an isolated local copy for decoded verification')
    for path in paths:
        _verify(ctx, rid, path, before)
    ctx.expect(op.get('destination', {}).get('id') == destination['id']
               if ctx.destination != 'local' else True, 'upload uses configured isolated destination')
    # Upload closes review; frame the terminal surface, retaining a catalog reference.
    ctx.view('ui.history')
    _frame(ctx, 'after', '01-history-list', 'history')


@recipe('confirm.review-discard', 'review.discard', needs=('recording',))
def discard(ctx):
    rid = _open(ctx)
    before = _inspect(ctx, rid)['edit']
    ctx.click('discard', window='review')
    ctx.wait(lambda: _state(ctx, rid)['discardConfirmation']['open'], 'discard confirmation opens')
    _frame(ctx, 'confirmation', '41-review-discard-confirm', 'confirmAction')
    ctx.click('keepReviewing', window='confirmAction')
    ctx.wait(lambda: not _state(ctx, rid)['discardConfirmation']['open'], 'Keep reviewing returns to review')
    ctx.expect(_inspect(ctx, rid)['edit'] == before, 'Keep reviewing preserves edits')
    _frame(ctx, 'kept')
    ctx.click('discard', window='review')
    ctx.wait(lambda: _state(ctx, rid)['discardConfirmation']['open'], 'second discard confirmation opens')
    ctx.click('confirmDiscard', window='confirmAction')
    op = ctx.wait(lambda: (s if (s := ctx.call('operation.inspect', {'id': rid}))['state'] == 'cancelled' else None),
                  'discard ends operation as cancelled')
    ctx.expect(not op.get('items'), 'discard published nothing')
    ctx.view('ui.history')
    _frame(ctx, 'after', '01-history-list', 'history')


@recipe('review.window-close', needs=('recording',))
def window_close(ctx):
    rid = _open(ctx)
    ctx.click('windowMaximize', window='review')
    ctx.expect(_state(ctx, rid)['visible'], 'maximized review remains visible')
    ctx.click('windowMaximize', window='review')
    ctx.click('windowMinimize', window='review')
    ctx.call('recording.raise', {'id': rid})
    ctx.wait(lambda: _state(ctx, rid)['visible'], 'review restored after minimize')
    ctx.click('windowClose', window='review')
    ctx.wait(lambda: _state(ctx, rid)['discardConfirmation']['open'], 'window close asks before discard')
    _frame(ctx, 'after', '41-review-discard-confirm', 'confirmAction')
    ctx.focus('confirmAction')
    ctx.keys('Esc')
    ctx.expect(_inspect(ctx, rid)['state'] == 'reviewing', 'Escape keeps recording')


@recipe('review.advice', needs=('recording', 'review-controls'))
def advice(ctx):
    rid = _open(ctx)
    _choose_format(ctx, rid, 'mp4', 'h265')
    ctx.click('formatAdvice', window='review')
    ctx.wait(lambda: _state(ctx, rid)['adviceDialog']['open'], 'Format tips opens')
    ctx.expect(bool(_state(ctx, rid)['adviceDialog']['text']), 'Format tips contains compatibility/audio advice')
    _frame(ctx, 'after', '68-review-error-details', 'confirmAction')
    ctx.focus('confirmAction')
    ctx.keys('Esc')
    ctx.expect(_inspect(ctx, rid)['state'] == 'reviewing', 'advice leaves recording in review')
    ctx.blocked('Format tips exercised; Export failed/Recording error advice variants need fault fixtures (#672)', issue=672)


@recipe('review.notice', needs=('recording-budget-stop',), timeout=240)
def stop_notice(ctx):
    rid = _open(ctx, 'recording-budget-stop')
    info = _inspect(ctx, rid)
    if info.get('stopReason') != 'budget':
        ctx.blocked('Needs a genuine byte-budget stopped motion recording, not a synthetic notice')
    ctx.expect(bool(_state(ctx, rid)['notice']) and bool(info.get('stopDetails')), 'budget notice explains actual stop')
    ctx.click('play', window='review')
    ctx.keys('Space')
    _frame(ctx, 'after')


@recipe('review.preview-error', needs=('recording-preview-error',))
def preview_error(ctx):
    rid = _open(ctx, 'recording-preview-error')
    if not _state(ctx, rid)['preview']['error']:
        ctx.blocked('Needs a reversible decoder failure fixture; do not corrupt a shared working clip')
    _frame(ctx, 'error', '40-review-preview-error')
    ctx.click('retryPreview', window='review')
    ctx.wait(lambda: not _state(ctx, rid)['preview']['error'], 'Retry preview recovers decoder')
    ctx.expect(_inspect(ctx, rid)['state'] == 'reviewing', 'preview recovery preserves private review')
    _frame(ctx, 'after')


def _reply(ctx, result):
    if 'returncode' in result:
        ctx.expect(result['returncode'] == 0, 'production CLI command succeeds')
        result = json.loads(result['stdout'])
    ctx.expect(not result.get('isError'), 'MCP tool succeeds')
    if result.get('structuredContent'):
        result = result['structuredContent']
    elif 'content' in result:
        texts = [c['text'] for c in result['content'] if c.get('type') == 'text']
        ctx.expect(bool(texts), 'MCP JSON tool returns text or structured content')
        result = json.loads(texts[0])
    ctx.expect(result.get('ok', True), 'recording command succeeds')
    return result.get('result', result)


def _api_edits(info):
    """Exercise timed opaque masks as well as trim through the public APIs."""
    w, h = info['canvas']
    return [{'op': 'setTrim', 'start': 1_000_000, 'end': 3_000_000},
            {'op': 'addMask', 'mask': {'kind': 'solid', 'rect': [w//4, h//4, w//4, h//4],
                                      'start': 1_200_000, 'end': 2_800_000}}]


@recipe('cli.recording', needs=('recording',))
def cli_recording(ctx):
    rid = _open(ctx, frame_review=False)
    initial = _inspect(ctx, rid)
    for method, params in [('recording.capabilities', {}), ('recording.inspect', {'id': rid}),
                           ('recording.review', {'id': rid, 'action': 'seek', 'time': 1_000_000}),
                           ('recording.edit', {'id': rid, 'revision': initial['edit']['revision'],
                                               'commands': _api_edits(initial)}),
                           ('recording.preview', {'id': rid, 'time': 1_500_000, 'maxEdge': 320})]:
        reply = _reply(ctx, ctx.cli(method, json.dumps(params)))
        if method == 'recording.preview':
            ctx.expect(bool(reply.get('png')), 'CLI preview returns encoded PNG')
    ctx.expect(_inspect(ctx, rid)['retainedUs'] == 2_000_000, 'CLI edit reaches resident')
    info = _inspect(ctx, rid)
    path = str(PurePosixPath(ctx.fixture('recording')['output_dir']) / ('cli-copy.' + info['config']['format']))
    _reply(ctx, ctx.cli('recording.saveCopy', json.dumps({'id': rid, 'path': path})))
    _saved(ctx, rid, path)
    ctx.frame('after')
    ctx.blocked('CLI review-stage entries exercised; capture prepare/start/pause/resume/stop/select/overlay need a capture fixture (#672)', issue=672)


def _mcp_review(ctx):
    rid = _open(ctx, frame_review=False)
    _reply(ctx, ctx.mcp('get_recording_capabilities'))
    info = _reply(ctx, ctx.mcp('get_recording', {'id': rid}))
    _reply(ctx, ctx.mcp('edit_recording', {'id': rid, 'revision': info['edit']['revision'],
                                         'commands': _api_edits(info)}))
    _reply(ctx, ctx.mcp('drive_recording_review', {'id': rid, 'action': 'seek', 'time_us': 1_500_000}))
    preview = ctx.mcp('preview_recording', {'id': rid, 'time_us': 1_500_000, 'max_edge': 320})
    ctx.expect(any(c.get('type') == 'image' and c.get('data') for c in preview.get('content', [])),
               'MCP preview returns an image content block')
    info = _inspect(ctx, rid)
    path = str(PurePosixPath(ctx.fixture('recording')['output_dir']) / ('mcp-copy.' + info['config']['format']))
    _reply(ctx, ctx.mcp('save_recording_copy', {'id': rid, 'path': path}))
    _saved(ctx, rid, path)
    ctx.expect(_inspect(ctx, rid)['retainedUs'] == 2_000_000, 'MCP trim reaches resident')
    ctx.frame('after')
    return rid, _inspect(ctx, rid)


@recipe('mcp.recording', needs=('recording', 'mcp'))
def mcp_recording(ctx):
    _mcp_review(ctx)
    ctx.blocked('MCP review/edit/preview/save exercised; prepare/control/select/overlay capture lifecycle remains unexercised (#672)', issue=672)


@recipe('mcp.recording-review', needs=('recording', 'mcp', 'destination'), timeout=240)
def mcp_recording_review(ctx):
    rid, before = _mcp_review(ctx)
    ctx.expect(ctx.destination == 'local', 'MCP review upload uses the isolated local destination')
    submitted = _reply(ctx, ctx.mcp('upload_recording', {'id': rid, 'revision': before['edit']['revision']}))
    operation_id = submitted['operationId']
    def terminal():
        op = _reply(ctx, ctx.mcp('get_operation', {'id': operation_id}))
        return op if op['state'] in ('completed', 'failed', 'cancelled') else None
    op = ctx.wait(terminal, 'MCP explicit Upload finishes', timeout=180)
    ctx.expect(op['state'] == 'completed', 'MCP Upload pipeline completes')
    paths = [i['local']['path'] for i in op.get('items', []) if i.get('local', {}).get('status') == 'saved']
    ctx.expect(bool(paths), 'MCP upload retains a local file for decoded verification')
    for path in paths:
        _verify(ctx, rid, path, before)
    ctx.view('ui.history')
    ctx.frame('uploaded')
