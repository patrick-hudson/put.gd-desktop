"""Regression cases adopted from the independent PR 726 validator. Synthetic only."""
import base64, copy, io, json, pickle, sys, tempfile, unittest
from unittest.mock import patch
from pathlib import Path
from contextlib import redirect_stdout, redirect_stderr
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(Path(__file__).parent))
from test_surface_redact import MemoryTarget, LINK
from tools.lab.surface_driver.api import Recipe, RecipeFailed, ReceiptRow
from tools.lab.surface_driver.context import DriverContext
from tools.lab.surface_driver.redact import Redactor, Sensitive, scan_run, save_frame, URL
from tools.lab.surface_driver.receipts import Evidence, write_receipts
from tools.lab.surface_driver.run import run_recipes
try:
    from PIL import Image, ImageDraw
except ImportError:
    Image = ImageDraw = None

PUTGD = 'https://put.gd/Zq9xK2mLpW4vR7tY.png'   # path-token style share link (S3 object key)
TOKEN = 'Zq9xK2mLpW4vR7tY'


class Adv(unittest.TestCase):
    def setUp(self):
        self.ocr_patch = patch('tools.lab.surface_driver.redact.ocr_text', return_value='')
        self.ocr_patch.start(); self.addCleanup(self.ocr_patch.stop)
        self.which_patch = patch('tools.lab.surface_driver.redact.shutil.which', return_value='unit-ocr')
        self.which_patch.start(); self.addCleanup(self.which_patch.stop)
        self.t = tempfile.TemporaryDirectory(); p = Path(self.t.name)
        (p/'catalog').mkdir(); self.out = p/'run'
        self.env = dict(desktop='mint-cinnamon-x11', host='boat', placement='boat', guest='g', package='source',
                        destination='local', build='b')
        tgt = MemoryTarget()
        tgt.cli = lambda *a, **k: {'returncode': 0, 'stdout': json.dumps({'ok': True, 'result': {'directUrl': PUTGD}}), 'stderr': ''}
        self.ctx = DriverContext(tgt, self.env, {'session': 'x11', 'catalog_tolerance': .025, 'at_spi': {'available': True},
            'evidence': {'browser_address_bars': [{'rect': [0, 0, 120, 15]}]}}, p/'catalog', self.out)

    def tearDown(self): self.t.cleanup()

    def files_contain(self, needle):
        return [str(f.relative_to(self.out)) for f in self.out.rglob('*') if f.is_file() and needle.encode() in f.read_bytes()]

    def run_one(self, fn):
        if Image is None: self.skipTest('requires python3-pil')
        sink = io.StringIO()
        with redirect_stdout(sink), redirect_stderr(sink):
            rows = run_recipes(self.ctx, [Recipe(('editor.undo',), fn)], self.env, self.out, 'c', self.out/'coverage.json')
        return rows, sink.getvalue(), json.loads((self.out/'receipt.json').read_text())

    # 1: path token of a history.detail link copied into a failure message
    def test_prevents_path_token_in_recipe_failure(self):
        def r(ctx):
            link = ctx.call('history.detail')['directUrl']
            raise RecipeFailed('object '+link.rsplit('/', 1)[1]+' missing')
        rows, console, receipt = self.run_one(r)
        self.assertFalse(self.files_contain(TOKEN))
        self.assertNotIn(TOKEN, console)
        self.assertFalse(self.files_contain(TOKEN))

    # 2: scheme-less link (e.g. displayed text "put.gd/<token>") in a note
    def test_prevents_schemeless_link_in_note(self):
        def r(ctx):
            link = ctx.call('history.detail')['directUrl']
            ctx.note('copied '+link.split('://')[1])
        rows, console, receipt = self.run_one(r)
        self.assertFalse(self.files_contain(TOKEN))
        self.assertNotIn(TOKEN, console)
        self.assertFalse(self.files_contain(TOKEN))

    # 3: base64 of the link
    def test_prevents_base64_link_in_note(self):
        b = base64.b64encode(PUTGD.encode()).decode()
        def r(ctx):
            ctx.call('history.detail'); ctx.note('blob '+b)
        rows, console, receipt = self.run_one(r)
        self.assertFalse(self.files_contain(TOKEN))
        self.assertNotIn(TOKEN, console)
        self.assertFalse(self.files_contain(b))

    # 3b: same with an explicitly Sensitive() value (audit literal) -- does scan catch base64?
    def test_scan_base64_of_sensitive_literal(self):
        b = base64.b64encode(PUTGD.encode()).decode()
        def r(ctx):
            ctx.sensitive(PUTGD); ctx.note('blob '+b)
        rows, console, receipt = self.run_one(r)
        self.assertFalse(self.files_contain(TOKEN))
        self.assertNotIn(TOKEN, console)

    # 4: forged url span carrying a registered secret as "host"
    def test_prevents_forged_url_span_host(self):
        def r(ctx):
            ctx.call('history.detail')
            ctx.note('[url '+json.dumps({'hash': '0'*64, 'host': TOKEN, 'length': 1, 'present': True}, sort_keys=True)+']')
        rows, console, receipt = self.run_one(r)
        self.assertFalse(self.files_contain(TOKEN))
        self.assertNotIn(TOKEN, console)

    # 5: Sensitive survives pickle/deepcopy/vars with raw value
    def test_sensitive_pickle_and_vars(self):
        s = Sensitive(PUTGD)
        with self.assertRaises(TypeError): pickle.dumps(s)
        with self.assertRaises(TypeError): vars(s)
        self.assertNotIn(TOKEN, repr(copy.deepcopy(s)))
        self.assertNotEqual(Sensitive('a'), Sensitive('b'))
        self.assertEqual(Sensitive('a'), Sensitive('a'))
        self.assertEqual(hash(Sensitive('a')), hash('a'))
        for action in (lambda: len(s), lambda: 'put.gd' in s, lambda: list(s)):
            with self.assertRaises(TypeError): action()

    # 6: type_text(ctx.sensitive(link)) types '[redacted]'
    def test_type_text_of_sensitive_types_placeholder(self):
        typed = []
        self.ctx.key = lambda chord: typed.append(chord)
        self.ctx.target.key = lambda codes: typed.append(codes)
        self.ctx._guard = lambda **kw: None
        self.ctx.type_text(self.ctx.sensitive('abcdefghijkl'))
        self.assertEqual(typed, [[c] for c in 'abcdefghijkl'])

    # 7: PNG with visible link outside any mask, no OCR -> does scan pass?
    @unittest.skipUnless(Image, 'requires python3-pil')
    def test_prevents_png_visible_text_no_ocr(self):
        img = Image.new('RGB', (900, 100), 'white')
        ImageDraw.Draw(img).text((10, 40), PUTGD, fill='black')
        ev = Evidence(self.out)
        self.ocr_patch.stop()
        with patch('tools.lab.surface_driver.redact.shutil.which', return_value=None):
            ev.frame(img, self.out/'frames'/'x.png', [])
            res = ev.scan()
        self.assertTrue(Image.open(self.out/'frames'/'x.png').getbbox() is None)
        self.assertFalse(res['passed'])

    # 8: secret in a STRUCTURAL field value (replace_secrets=False)
    def test_prevents_structural_field(self):
        r = Redactor(); r.remember({'token': TOKEN})
        out = r.safe({'stage': 'x '+TOKEN, 'label': TOKEN, 'kind': TOKEN})
        self.assertNotIn(TOKEN, json.dumps(out))

    # 9: secret as a dict KEY
    def test_prevents_dict_key(self):
        r = Redactor(); r.remember({'token': TOKEN})
        self.assertNotIn(TOKEN, json.dumps(r.safe({TOKEN: 1})))

    # 10: secret registered AFTER being noted (late registration) - is the note rescrubbed at write?
    def test_late_registration(self):
        def r(ctx):
            ctx.note('pre '+TOKEN+'XYZ')   # token-like string, then registered
            ctx.sensitive(TOKEN+'XYZ')
        rows, console, receipt = self.run_one(r)
        self.assertFalse(self.files_contain(TOKEN))
        self.assertNotIn(TOKEN, console)

    # 11: Proton-style link: path id + fragment password
    def test_proton_link_path_id(self):
        link = 'https://drive.proton.me/urls/ABCD1234EF#Pw0rdSecret'
        r = Redactor(); r.remember({'link': link})
        self.assertEqual(r.text('share id ABCD1234EF pw Pw0rdSecret'), 'share id [redacted] pw [redacted]')

    # 12: chained exception: RecipeFailed with __cause__ carrying the secret
    def test_chained_exception(self):
        def r(ctx):
            ctx.call('history.detail')
            try:
                raise ValueError(PUTGD)
            except ValueError as e:
                raise RecipeFailed('compare failed') from e
        rows, console, receipt = self.run_one(r)
        self.assertFalse(self.files_contain(TOKEN))
        self.assertNotIn(TOKEN, console)

    # 13: os.write to fd 1/2 bypasses redirect_stdout
    def test_fd_write_bypass(self):
        import os, subprocess
        def r(ctx):
            ctx.sensitive(PUTGD)
            os.write(1, PUTGD.encode())
            os.write(2, PUTGD.encode())
            subprocess.run([sys.executable, '-c', 'import os; os.write(1, '+repr(PUTGD.encode())+')'], check=True)
        rows, console, receipt = self.run_one(r)
        self.assertFalse(self.files_contain(TOKEN))
        self.assertNotIn(TOKEN, console)

    # 14: JSON scan with escaped unicode
    def test_scan_json_unicode_escape(self):
        self.out.mkdir(parents=True, exist_ok=True)
        (self.out/'a.json').write_text(json.dumps({'n': {'deep': [PUTGD]}}))
        (self.out/'b.json').write_text('{"n": "https\\u003a//put.gd/' + TOKEN + '"}')
        (self.out/'c.log').write_text('line put.gd/'+TOKEN+'\n')
        (self.out/'d.log').write_text('line '+base64.b64encode(PUTGD.encode()).decode()+'\n')
        (self.out/'e.log').write_text('hxxps://put.gd/'+TOKEN+'\n')
        res = scan_run(self.out, frames={}, ocr='')
        self.assertFalse(res['passed'])
        self.assertEqual({f['file_index'] for f in res['findings']}, set(range(5)))


    def test_encoded_forms_all_alignments_percent_hex_and_urlsafe(self):
        from urllib.parse import quote
        r = Redactor(); r.remember({'directUrl': PUTGD})
        forms = [quote(PUTGD, safe=''), ''.join('%%%02x' % b for b in PUTGD.encode()), PUTGD.encode().hex(), PUTGD.encode().hex().upper()]
        for n in range(3):
            for encode in (base64.b64encode, base64.urlsafe_b64encode):
                forms.append(encode(b'x'*n+PUTGD.encode()+b'yy').decode())
        self.out.mkdir()
        for form in forms:
            with self.subTest(form_kind=type(form).__name__):
                self.assertNotEqual(r.text(form), form)
                (self.out/'leak.log').write_text(form)
                scan = scan_run(self.out, literals=r.audit_literals)
                self.assertFalse(scan['passed'])
                self.assertIn('registered-secret', {v['kind'] for v in scan['findings']})

    def test_resident_methods_register_all_url_components(self):
        for method in ('history.detail', 'share.inspect', 'operation.inspect'):
            self.ctx.call(method)
            registry = self.ctx.evidence.redactor.audit_literals
            self.assertIn(TOKEN, registry)
            self.assertIn('put.gd/'+TOKEN+'.png', registry)
            self.assertIn(TOKEN+'.png', registry)
        link = 'https://drive.proton.me/urls/ABCD1234EF?q=QueryPassword42#Pw0rdSecret'
        r=Redactor();r.remember({'directUrl': link})
        for part in ('ABCD1234EF', 'QueryPassword42', 'Pw0rdSecret', '/urls/ABCD1234EF', link.split('://')[1]):
            self.assertIn(part, r.audit_literals)
            self.assertNotIn(part, r.text('part '+part))

    def test_enums_docs_and_release_links_do_not_poison_sensitivity(self):
        r = Redactor()
        r.remember({'clipboard': 'text', 'loginSession': 'tty', 'token': 'local'}, opaque=True)
        self.assertFalse(r.audit_literals)
        for url in ('https://put.gd', 'https://github.com/patrick-hudson/put.gd-desktop/releases/tag/v26.1029.1',
                    'https://github.com/patrick-hudson/put.gd-desktop/blob/main/docs/package-repositories.md#switching-channels'):
            self.assertFalse(r.has_sensitive_url(url))
            r.remember(url)
        self.assertFalse(r.audit_literals)
        self.assertEqual(r.text('tolerance=0.025 rv726 text-tool'), 'tolerance=0.025 rv726 text-tool')
        self.ctx.target.cli = lambda *a, **kw: {'returncode':0, 'stdout':json.dumps({'ok':True,'result':{'docsUrl':url}}),'stderr':''}
        self.ctx.call('ui.snapshot')
        self.assertFalse(self.ctx._sensitive_seen)

    @unittest.skipUnless(Image, 'requires python3-pil')
    def test_unknown_link_pixels_masked_before_first_disk_write(self):
        img = Image.new('RGB', (600,200), 'white')
        path = self.out/'frames'/'unknown.png'
        with patch('tools.lab.surface_driver.redact.ocr_text', return_value=PUTGD):
            meta=self.ctx.evidence.frame(img, path, [])
        self.assertIsNone(Image.open(path).getbbox())
        self.assertEqual(meta['guard'], 'ocr-sensitive-text-fully-masked')

    def test_remote_or_registered_secret_requires_ocr(self):
        from tools.lab.surface_driver.receipts import Evidence
        for remote, secret in ((True, False), (False, True)):
            ev=Evidence(self.out, require_ocr=remote)
            if secret: ev.redactor.remember(Sensitive(PUTGD))
            with patch('tools.lab.surface_driver.redact.shutil.which', return_value=None):
                result=ev.scan()
            self.assertFalse(result['passed'])
            self.assertIn('ocr-required-unavailable', {v['kind'] for v in result['findings']})

    @unittest.skipUnless(Image, 'requires python3-pil')
    def test_quarantine_and_external_coverage_gate(self):
        external=self.out.parent/'external'/'coverage.json'
        def plant(ctx):
            ctx.sensitive(PUTGD)
            (ctx.out/'leak.log').write_text(TOKEN)
        import tools.lab.surface_driver.redact as redact
        original=redact.tempfile.mkdtemp
        directories=[]
        def quarantine(**kw):
            directory=Path(original(**kw));directories.append(directory)
            self.addCleanup(__import__('shutil').rmtree,directory)
            return str(directory)
        with patch.object(redact.tempfile,'mkdtemp',side_effect=quarantine):
            rows=run_recipes(self.ctx,[Recipe(('editor.undo',),plant)],self.env,self.out,'safe',external)
        self.assertEqual(rows[0].functional, 'failed')
        self.assertFalse((self.out/'leak.log').exists())
        self.assertFalse(external.exists())
        receipt=json.loads((self.out/'receipt.json').read_text())
        self.assertTrue(receipt['secret_scan']['quarantine'])
        self.assertTrue(directories)
        for directory in directories:
            self.assertFalse(directory.is_relative_to(self.out.parent))
            self.assertEqual(directory.stat().st_mode & 0o777, 0o700)
            self.assertTrue(any(TOKEN.encode() in f.read_bytes() for f in directory.iterdir()))

    def test_coverage_written_inside_run_and_scanned_before_copy(self):
        import tools.lab.surface_driver.run as runner
        external=self.out.parent/'external'/'coverage.json'
        original=runner.write_receipts
        def poisoned(out, env, rows, campaign, coverage, **kw):
            self.assertTrue(Path(coverage).is_relative_to(self.out))
            original(out, env, rows, campaign, coverage, **kw)
            if Path(coverage).name == 'coverage.json':
                (self.out/'coverage.json').write_text(json.dumps({'leak': PUTGD}))
        with patch.object(runner, 'write_receipts', side_effect=poisoned):
            run_recipes(self.ctx,[Recipe(('editor.undo',),lambda ctx:None)],self.env,self.out,'safe',external)
        self.assertFalse(external.exists())
        self.assertFalse(json.loads((self.out/'receipt.json').read_text())['secret_scan']['passed'])

    def test_quarantine_removes_leaks_across_filesystems(self):
        import errno
        import tools.lab.surface_driver.redact as redact
        self.out.mkdir()
        planted=self.out/'planted.log'
        planted.write_text(TOKEN)
        quarantine=self.out.parent/'private-quarantine'
        quarantine.mkdir(mode=0o700)
        with patch.object(redact.tempfile,'mkdtemp',return_value=str(quarantine)), \
                patch.object(redact.os,'rename',side_effect=OSError(errno.EXDEV,'cross-device link')):
            result=scan_run(self.out,literals=(TOKEN,),quarantine=True)
        self.assertFalse(result['passed'])
        self.assertFalse(planted.exists())
        self.assertEqual(len(result['quarantine']),1)
        moved=next(quarantine.iterdir())
        self.assertEqual(moved.read_text(),TOKEN)
        self.assertEqual(quarantine.stat().st_mode & 0o777,0o700)
        self.assertEqual(moved.stat().st_mode & 0o777,0o600)

    def test_target_scripts_queries_and_cli_params_never_reach_argv(self):
        from tools.lab.surface_driver.target import Target
        target=Target.__new__(Target);target.physical=None;target.heartbeat=lambda **kw:None;target.prefix=lambda:''
        scripts=[]
        target.session=lambda script,timeout: scripts.append(script) or json.dumps({'returncode':0,'stdout':'','stderr':''})
        target.execute('echo '+PUTGD)
        code=scripts[-1].split("<<'SD_PY'\n",1)[1].rsplit('SD_PY',1)[0]
        import ast
        tree=ast.parse(code)
        call=next(n for n in ast.walk(tree) if isinstance(n,ast.Call) and isinstance(n.func,ast.Attribute) and n.func.attr=='run')
        self.assertEqual(ast.literal_eval(call.args[0]), ['sh','-s'])
        self.assertIn('input', {k.arg for k in call.keywords})
        target.socket='/tmp/s/session.sock'
        target.cli('share.inspect',json.dumps({'link':PUTGD}))
        # The nested CLI subprocess receives only method/socket/options in argv.
        outer=ast.parse(scripts[-1].split("<<'SD_PY'\n",1)[1].rsplit('SD_PY',1)[0])
        script=next(k.value.value for n in ast.walk(outer) if isinstance(n,ast.Call) for k in n.keywords if k.arg=='input')
        inner=ast.parse(script.split("<<'SD_CLI'\n",1)[1].rsplit('SD_CLI',1)[0])
        argv=next(ast.literal_eval(n.args[0]) for n in ast.walk(inner) if isinstance(n,ast.Call) and isinstance(n.func,ast.Attribute) and n.func.attr=='run')
        self.assertNotIn(TOKEN, repr(argv));self.assertIn('--params-stdin',argv)
        self.ctx.shell=lambda script: {'returncode':0,'stdout':json.dumps({'ok':True,'result':{}})}
        self.ctx._probe('geometry',{'title':PUTGD})

    def test_standalone_literals_file_detects_derived_token(self):
        import subprocess, os
        self.out.mkdir()
        (self.out/'leak.log').write_text(TOKEN)
        literals=self.out.parent/'private-literals.json'
        literals.write_text(json.dumps([TOKEN]));literals.chmod(0o600)
        result=subprocess.run([sys.executable,'-m','tools.lab.surface_driver.redact',str(self.out),'--literals-file',str(literals)],capture_output=True,text=True)
        self.assertEqual(result.returncode,1)
        self.assertNotIn(TOKEN,result.stdout+result.stderr)
        self.assertFalse((self.out/'leak.log').exists())


    @unittest.skipUnless(Image, 'requires python3-pil')
    def test_mask_geometry_preserves_the_input_head(self):
        self.ctx._input_head=0
        self.ctx._sensitive_controls=[('tokenField','history')]
        def geometry(role):
            self.ctx._input_head=1
            return {'head':1}
        self.ctx.geometry=geometry
        self.ctx._frame_regions(Image.new('RGB',(120,80),'white'),0)
        self.assertEqual(self.ctx._input_head,0)

    def test_resident_private_log_and_state_root_removed_after_quit(self):
        from tools.lab.surface_driver.target import Target
        target=Target.__new__(Target)
        target._rfb=None;target.monitor_pid=None;target.started=True;target._owns_root=True;target._created_root=True
        target.root='/tmp/sd-owned';target.socket=target.root+'/runtime/putgd/session.sock'
        target.cli=lambda *a,**kw:{'returncode':0}
        scripts=[]
        target.execute=lambda script,**kw:scripts.append(script) or {'returncode':0}
        self.assertFalse(target.close())
        self.assertEqual(target.private_cleanup,'resident-log-and-state-root-removed')
        self.assertIn('shutil.rmtree',scripts[-1])
        self.assertFalse(target._owns_root)

    @unittest.skipUnless(Image, 'requires python3-pil')
    def test_exported_coverage_copy_is_scanned_and_quarantined(self):
        import tools.lab.surface_driver.run as runner
        external=self.out.parent/'export'/'coverage.json'
        original=runner.write_json
        def corrupt(path, value, **kw):
            original(path, value, **kw)
            if Path(path)==external:
                Path(path).write_text(json.dumps({'leak':PUTGD}))
        with patch.object(runner, 'write_json', side_effect=corrupt):
            rows=run_recipes(self.ctx,[Recipe(('editor.undo',),lambda ctx:None)],self.env,self.out,'safe',external)
        self.assertEqual(rows[0].functional,'failed')
        self.assertFalse(external.exists())
        self.assertFalse(self.env['coverage_export']['copy_scan_passed'])
        self.assertTrue(json.loads((self.out/'receipt.json').read_text())['secret_scan']['quarantine'])


    @unittest.skipUnless(Image, 'requires python3-pil')
    def test_black_frames_cannot_receive_visual_approval(self):
        black=Image.new('RGB',(120,80),'black')
        self.ctx.target.capture_image=lambda head=0:black
        black.save(self.ctx.catalog.directory/'black.png')
        self.ctx.geometry=lambda role:{'x':0,'y':0,'width':120,'height':80,'scale':1}
        def recipe(ctx):ctx.frame('black',window='editor',reference='black')
        rows=run_recipes(self.ctx,[Recipe(('editor.undo',),recipe)],self.env,self.out,'safe',self.out/'coverage.json')
        self.assertNotEqual(rows[0].visual,'reviewed-ok')
        self.assertIn('no visible pixels', rows[0].review_reason)


    def test_mixed_case_percent_and_hex_tokens_and_encoded_forged_spans(self):
        r=Redactor();r.remember({'directUrl':PUTGD})
        percent=''.join(('%%%02X' % b) if n%2 else ('%%%02x' % b) for n,b in enumerate(TOKEN.encode()))
        hexadecimal=''.join(c.upper() if n%2 else c.lower() for n,c in enumerate(TOKEN.encode().hex()))
        self.out.mkdir()
        for encoded in (percent, hexadecimal, base64.b64encode(TOKEN.encode()).decode()):
            (self.out/'leak.log').write_text(encoded)
            result=scan_run(self.out,literals=r.audit_literals)
            self.assertIn('registered-secret',{f['kind'] for f in result['findings']})
            self.assertNotEqual(r.text(encoded),encoded)
            span='[url '+json.dumps({'host':encoded,'hash':'0'*64,'length':1,'present':True})+']'
            self.assertNotIn(encoded,r.text(span))


    @unittest.skipUnless(Image, 'requires python3-pil')
    def test_coverage_destination_filename_checked_before_creating_it(self):
        external=self.out.parent/TOKEN/'coverage.json'
        rows=run_recipes(self.ctx,[Recipe(('editor.undo',),lambda ctx:ctx.sensitive(PUTGD))],self.env,self.out,'safe',external)
        self.assertEqual(rows[0].functional,'failed')
        self.assertFalse(external.parent.exists())
        receipt=(self.out/'receipt.json').read_text()
        self.assertNotIn(TOKEN,receipt)
        self.assertIn('coverage-export-filename-registered-secret',receipt)


    def test_repository_paths_are_not_mistaken_for_schemeless_share_links(self):
        from tools.lab.surface_driver.redact import detected
        r=Redactor()
        path='/home/user/work/put.gd/src/tools/lab/surface_map/coverage/campaign.json'
        self.assertFalse(r.has_sensitive_url(path))
        self.assertEqual(r.text(path),path)
        self.assertFalse(r.audit_literals)
        self.assertFalse(detected(path))
        self.assertTrue(detected('put.gd/'+TOKEN))
        self.assertTrue(detected('//put.gd/'+TOKEN))


    def test_non_url_resident_values_do_not_blank_local_frames(self):
        r=Redactor()
        r.remember({'kind':'clipboard-image','format':'video/mp4','mode':'very-long-enum'},opaque=True)
        self.assertFalse(r.audit_literals)
        r.remember(Sensitive('synthetic-opaque-private-value'))
        self.assertFalse(r.has_sensitive_url('synthetic-opaque-private-value'))
        r.remember(Sensitive('https://custom.example.invalid/SyntheticObjectId726'))
        self.assertTrue(r.has_sensitive_url('custom.example.invalid/SyntheticObjectId726'))


    def test_decoded_path_segments_fragment_fields_and_url_userinfo(self):
        link='https://UserName726Secret:Password726Secret@example.invalid/a%2FPathToken726Z9?q=QueryToken726Z9#password=FragmentToken726Z9&k=ExtraToken726Z9'
        r=Redactor();r.remember({'directUrl':link})
        for token in ('UserName726Secret','Password726Secret','PathToken726Z9','QueryToken726Z9','FragmentToken726Z9','ExtraToken726Z9'):
            self.assertIn(token,r.audit_literals)
            self.assertNotIn(token,r.text('piece '+token))


    def test_unregistered_schemeless_s3_endpoint_is_a_link_shape(self):
        self.out.mkdir()
        (self.out/'leak.log').write_text('bucket.s3.us-east-2.amazonaws.com/'+TOKEN+'.png')
        result=scan_run(self.out)
        self.assertFalse(result['passed'])
        self.assertIn('url',{f['kind'] for f in result['findings']})


    def test_standalone_audit_never_restores_a_quarantined_receipt_secret(self):
        import subprocess
        self.out.mkdir()
        row={'interaction':'editor.undo','functional':'passed','note':TOKEN}
        (self.out/'receipt.json').write_text(json.dumps({'interactions':[row]}))
        external=self.out.parent/'coverage.json'
        external.write_text(json.dumps({'entries':[row]}))
        literals=self.out.parent/'literals.json'
        literals.write_text(json.dumps([PUTGD]));literals.chmod(0o600)
        result=subprocess.run([sys.executable,'-m','tools.lab.surface_driver.redact',str(self.out),'--literals-file',str(literals),'--coverage',str(external)],capture_output=True,text=True)
        self.assertEqual(result.returncode,1)
        self.assertNotIn(TOKEN,result.stdout+result.stderr)
        self.assertFalse(self.files_contain(TOKEN))
        self.assertNotIn(TOKEN,external.read_text())
        self.assertEqual(json.loads(external.read_text())['entries'][0]['functional'],'failed')
        self.assertFalse(json.loads((self.out/'receipt.json').read_text())['secret_scan']['passed'])

    @unittest.skipUnless(Image, 'requires python3-pil')
    def test_late_leak_invalidates_previous_coverage_without_restoring_secrets(self):
        from tools.lab.surface_driver.run import finish_evidence
        external=self.out.parent/'export'/'coverage.json'
        rows=run_recipes(self.ctx,[Recipe(('editor.undo',),lambda ctx:None)],self.env,self.out,'safe',external)
        self.assertEqual(rows[0].functional,'passed')
        value=json.loads(external.read_text())
        value['entries'].append(dict(value['entries'][0],interaction='editor.redo'))
        external.write_text(json.dumps(value))
        self.ctx.sensitive(PUTGD)
        (self.out/'late.log').write_text(TOKEN)
        result=finish_evidence(self.ctx.evidence,self.out,self.env,rows,'safe',external)
        self.assertFalse(result['passed'])
        self.assertEqual(result['invalidated_coverage_rows'],1)
        self.assertEqual([r['functional'] for r in json.loads(external.read_text())['entries']],['failed','passed'])
        self.assertNotIn(TOKEN,external.read_text())
        self.assertFalse((self.out/'late.log').exists())


    def test_link_shaped_resident_dictionary_keys_feed_the_registry(self):
        self.ctx.target.cli=lambda *a,**kw:{'returncode':0,'stdout':json.dumps({'ok':True,'result':{PUTGD:'local'}}),'stderr':''}
        self.ctx.call('history.detail')
        self.assertIn(TOKEN,self.ctx.evidence.redactor.audit_literals)
        self.assertNotIn(TOKEN,self.ctx.evidence.redactor.text('piece '+TOKEN))


if __name__ == '__main__':
    unittest.main()
