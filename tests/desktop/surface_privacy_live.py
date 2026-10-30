"""Synthetic Cinnamon regression campaign. Run on the disposable QEMU host only.

No credentials, cloud requests, raw frame exports or private quarantine exports.
The caller owns overlay/wall lifecycle and reviews the saved core-demo pixels.
"""
import argparse
import base64
import dataclasses
import hashlib
import io
import json
from pathlib import Path
import subprocess
import time

from PIL import Image, ImageDraw, ImageFont
from tools.lab.surface_driver.api import Recipe
from tools.lab.surface_driver.context import DriverContext
from tools.lab.surface_driver.profiles import load_profile
from tools.lab.surface_driver.receipts import write_json, write_receipts
from tools.lab.surface_driver.run import run_recipes, discover, guard_target
from tools.lab.surface_driver.target import Target, checked_socket

LINK = 'https://put.gd/Zq9xK2mLpW4vR7tY.png'
TOKEN = 'Zq9xK2mLpW4vR7tY'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--boat-id', required=True)
    args = parser.parse_args()
    args.out.mkdir(mode=0o700, parents=True, exist_ok=False)
    source = subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip()
    target = Target(run=args.run)
    guard_target(argparse.Namespace(placement='boat'), target)
    checked_socket(target.socket); checked_socket(target.root+'/notify.sock')
    profile = load_profile('mint-cinnamon-x11')
    profile.setdefault('windows', {})['privacy-fixture'] = {'title':'^Synthetic privacy fixture$'}
    catalog = args.out/'catalog'; catalog.mkdir(mode=0o700)
    env = dict(desktop='mint-cinnamon-x11', host='boat', placement='boat', guest='cinnamon-x11',
               package='deb', destination='local', build=source, driver_source=source,
               placement_details={'boat_sandbox_id':args.boat_id, 'run_id':target.info['id'],
                                  'input_backend':'qmp','frame_backend':'vnc-in-memory'}, synthetic_only=True)
    env['target'] = target.identity()
    summary = {'driver_source':source, 'run_id':target.info['id'], 'placement':'boat'}
    pid = None
    try:
        target.start()
        ctx = DriverContext(target, env, profile, catalog, args.out/'undeclared')
        observed = ctx.call('ui.snapshot').get('build', {})
        env['observed_build'] = observed
        assert observed.get('gitCommit') == source, 'installed app must match final driver source'
        assert not ctx._sensitive_seen, 'docs/release URLs must not poison the context'
        fixture = '''import gi,signal
gi.require_version('Gtk','3.0')
from gi.repository import Gtk,GLib
window=Gtk.Window(title='Synthetic privacy fixture');window.set_default_size(1150,300);window.move(40,220)
label=Gtk.Label();label.set_markup('<span font_desc="DejaVu Sans Mono 24">'+LINK+'</span>');window.add(label)
window.connect('destroy',Gtk.main_quit);signal.signal(signal.SIGTERM,lambda *_:GLib.idle_add(Gtk.main_quit))
window.show_all();Gtk.main()
'''
        script = "nohup python3 - <<'PRIVACY_FIXTURE' >/dev/null 2>&1 &\nLINK="+repr(LINK)+'\n'+fixture+'PRIVACY_FIXTURE\necho $!\n'
        result = ctx.shell(script)
        pid = int(result['stdout'].strip())
        ctx.wait(lambda: ctx.windows({'title':'^Synthetic privacy fixture$'}), 'privacy fixture visible', 20)
        ctx.focus('privacy-fixture')
        ctx.expect(len(ctx.accessible({'name':LINK,'owned':True,'visible':True,'showing':True})) == 1, 'complete synthetic link is drawn')
        # Fresh context has not seen the URL in stdout/CLI or declared a secret.
        ctx = DriverContext(target, dict(env), profile, catalog, args.out/'undeclared')
        assert not ctx._sensitive_seen and not ctx.evidence.redactor.audit_literals
        frame = ctx.frame('unknown-link')
        meta = ctx.evidence.frames[frame.path]
        assert meta['guard'] == 'ocr-sensitive-text-fully-masked', meta['guard']
        with Image.open(ctx.out/frame.path) as saved:
            assert saved.getbbox() is None
        summary['undeclared_pixels'] = {'guard':meta['guard'], 'first_write_safe':True, 'registered_before_capture':False}
        write_json(ctx.out/'receipt.json', {'environment':env,'frame':dataclasses.asdict(frame)}, redactor=ctx.evidence.redactor)
        assert ctx.evidence.scan()['passed']
        target.execute('kill '+str(pid)); pid = None
        time.sleep(.5)

        coverage = args.out/'export'/'coverage.json'
        def clean(c):
            c.note('no secret material')
        positive = DriverContext(target, dict(env), profile, catalog, args.out/'positive')
        rows = run_recipes(positive, [Recipe(('editor.undo',), clean)], positive.environment, positive.out, 'privacy726', coverage)
        assert rows[0].functional == 'passed'
        assert positive.environment['coverage_export'] == {'scanned_before_copy':True,'copy_scan_passed':True}
        summary['coverage'] = positive.environment['coverage_export']

        for kind in ('path-token', 'base64', 'scheme-less', 'ocr-visible'):
            context = DriverContext(target, dict(env), profile, catalog, args.out/kind)
            def plant(c, kind=kind):
                c.sensitive(LINK)
                if kind == 'ocr-visible':
                    image = Image.new('RGB',(1200,160),'white')
                    ImageDraw.Draw(image).text((20,50), LINK, font=ImageFont.truetype('DejaVuSansMono.ttf',32), fill='black')
                    stream=io.BytesIO();image.save(stream,format='PNG');data=stream.getvalue()
                    path=c.out/'planted.png';path.write_bytes(data)  # Deliberate synthetic bypass.
                    c.evidence.frames['planted.png']={'sha256':hashlib.sha256(data).hexdigest(),'size':list(image.size),'regions':[]}
                else:
                    text={'path-token':TOKEN,'base64':base64.b64encode(LINK.encode()).decode(),'scheme-less':LINK.split('://')[1]}[kind]
                    (c.out/'planted.log').write_text(text)
            rows = run_recipes(context, [Recipe(('editor.undo',), plant)], context.environment, context.out, 'privacy726', context.out/'coverage.json')
            receipt=json.loads((context.out/'receipt.json').read_text())
            scan=receipt['secret_scan']
            assert rows[0].functional == 'failed' and not scan['passed'] and scan['quarantine']
            assert not (context.out/'planted.log').exists() and not (context.out/'planted.png').exists()
            if kind == 'ocr-visible':
                assert any(f['kind'].startswith('png-text-') for f in scan['findings'])
            summary.setdefault('negative_cases',[]).append({'kind':kind,'functional':'failed','scan':scan,'offender_removed':True})

        # Build a baseline using the same public fixture, then run the actual recipes.
        from tools.lab.surface_driver.recipes.core_demo import opened, cancelled
        preparation=DriverContext(target,dict(env),profile,catalog,args.out/'baseline')
        identity=opened(preparation)
        preparation.click('text:Redact',window='editor')
        preparation.wait(lambda:preparation.call('editor.view',{'id':identity}).get('tool')=='redact','baseline redact tool')
        frame=preparation.frame('baseline-redact',window='editor')
        (catalog/'20-editor-redact.png').write_bytes((preparation.out/frame.path).read_bytes())
        cancelled(preparation,identity)
        core=DriverContext(target,dict(env),profile,catalog,args.out/'core-demo')
        rows=run_recipes(core,discover('core_demo'),core.environment,core.out,'privacy726-core',core.out/'coverage.json')
        assert all(r.functional=='passed' for r in rows)
        masks=core.evidence.frames
        for path in masks:
            with Image.open(core.out/path) as image:
                assert image.getbbox() is not None, 'core-demo frame is entirely black'
        compared=[r for r in rows if r.visual=='reviewed-ok']
        assert compared and all(not r.review for r in compared)
        summary['core_demo']={'rows':[r.to_json() for r in rows], 'frames':len(masks),'non_black':True,
                              'comparison_reviewed_ok':len(compared),'human_review':'pending'}
    except BaseException as error:
        summary['failure_type']=type(error).__name__
        target.screenshot(args.out/'failure.png')  # Full mask; never persist unexpected raw pixels.
        raise RuntimeError('privacy campaign failed: '+type(error).__name__) from None
    finally:
        if pid:
            target.execute('kill '+str(pid)+' 2>/dev/null || true')
        errors=target.close()
        summary['cleanup']={'errors':errors,'private_root':getattr(target,'private_cleanup',None)}
        write_json(args.out/'summary.json',summary)
    assert not errors


if __name__ == '__main__':
    main()
