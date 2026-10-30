"""Light recipe-contract tests. These fakes are never desktop evidence."""
import copy
import json
from pathlib import Path
import sys
import tempfile
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.lab.surface_driver.api import Context, REGISTRY, RecipeBlocked, RecipeFailed
from tools.lab.surface_driver.recipes import editor


class FakeContext(Context):
    def __init__(self):
        self.events = []
        self.profile = {}
        self.phase = 'editing'
        self.state = {'phase':'editing','width':640,'height':320,'shapes':[], 'canUndo':False,'canRedo':False,
                      'view':{'tool':'redact','fitted':True,'visibleRect':[0,0,640,320], 'zoomPercent':100,
                              'canvasSize':[800,500],'propertiesWidth':240, 'selectedShapeId':None,
                              'geometryDetails':{'open':False},'keyboardHelp':{'open':False},
                              'discardConfirmation':{'open':False}, 'destination':{'text':'Local only'}}}
        self.undone = []
        self.draft = None
        self.bad_geometry = False
        self.bad_kind = False

    def fixture(self, name):
        self.events.append(('fixture',name))
        assert name == 'image'
        return {'path':'/private/fixture.png','width':640,'height':320}

    def call(self, method, params=None):
        self.events.append(('call',method,params))
        if method == 'editor.open': return {'operationId':'op-1'}
        if method == 'editor.inspect': return copy.deepcopy(self.state)
        if method == 'operation.inspect': return {'state':self.phase, 'items':[]}
        if method == 'editor.cancel': self.phase='cancelled'; return {}
        if method == 'editor.raise': return {'raised':True}
        raise AssertionError(method)

    def cli(self, method, params, **kwargs):
        self.events.append(('cli',method,params))
        assert method=='editor.inspect'
        return {'returncode':0,'stdout':json.dumps({'ok':True,'result':self.state}),'stderr':''}

    def view(self, method, params=None):
        self.events.append(('view',method,params))
        assert method == 'editor.view'
        if params['action']=='fit': self.state['view']['fitted']=True
        else: raise AssertionError(params)
        return copy.deepcopy(self.state['view'])

    def wait(self, predicate, what, **kwargs):
        value=predicate()
        self.expect(value, what)
        return value

    def frame(self, label, window=None, reference=None):
        self.events.append(('frame',label,window,reference))

    def focus(self, window, **kwargs):
        self.events.append(('focus',window))
        return {'focused':True}

    def use_catalog(self, window, reference):
        self.events.append(('use_catalog',window,reference))

    def geometry(self, window):
        self.events.append(('geometry',window))
        if window == 'editor.canvas': return {'x':100,'y':100,'width':800,'height':500}
        assert window == 'editor'
        return {'x':0,'y':0,'width':1180,'height':820}

    def accessible(self,query):
        return [{'name':'Image being edited','rect':[100,100,800,500]}]

    def click(self, control, window, **kwargs):
        self.events.append(('click',control,window))
        if control=='text:Select and move': self.state['view']['tool']='select'
        elif control.startswith('text:') and control[5:].lower() in ('redact','arrow','rectangle','highlight','text','select','blur','crop'):
            self.state['view']['tool']=control[5:].lower()
        elif control == 'placeMark': self._add({'kind':'redact','rect':[220,110,200,100]})
        elif control in ('removeMark','text:Remove mark'): self.state['shapes']=[]
        elif control.startswith('text:Undo'): self.keys('Ctrl+Z')
        elif control.startswith('text:Redo'): self.keys('Ctrl+Shift+Z')
        elif control == 'editorCancel': self.state['view']['discardConfirmation']['open']=True
        elif control == 'text:Keep editing': self.state['view']['discardConfirmation']['open']=False
        elif control == 'confirmDiscard': self.phase='cancelled'
        else: raise AssertionError(control)

    def _add(self, shape):
        shape={**shape,'id':1}
        self.state['shapes']=[shape]
        self.state['view']['selectedShapeId']=1
        self.state['canUndo']=True

    def drag(self, start, end, **kwargs):
        self.events.append(('drag',start,end,kwargs))
        self._add({'kind':'blur' if self.bad_kind else self.state['view']['tool'],
                   'rect':[0,0,20,20] if self.bad_geometry else [340,20,260,120]})

    def click_at(self, x,y, **kwargs):
        self.events.append(('click_at',x,y))
        if self.state['view']['tool']=='select': return
        self.state['view']['editingText']=True
        self.draft=''

    def type_text(self, value):
        self.events.append(('type_text',value))
        self.draft=value

    def keys(self, *sequence):
        self.events.append(('keys',sequence))
        toolkeys={'X':'redact','R':'rectangle','A':'arrow','T':'text','H':'highlight','B':'blur','V':'select'}
        for key in sequence:
            if key in toolkeys: self.state['view']['tool']=toolkeys[key]
            elif key == 'Insert': self._add({'kind':'redact','rect':[220,110,200,100]})
            elif key in ('Delete','Backspace'): self.state['shapes']=[]
            elif key == 'Ctrl+Z':
                self.undone=copy.deepcopy(self.state['shapes']); self.state['shapes']=[]; self.state['canRedo']=True
            elif key in ('Ctrl+Shift+Z','Ctrl+Y'): self.state['shapes']=copy.deepcopy(self.undone)
            elif key == 'Return':
                if self.state['view'].get('editingText'):
                    self._add({'kind':'text','text':self.draft,'rect':[20,200,80,24]})
                    self.state['view']['editingText']=False
                else: self.state['view']['editingText']=True
            elif key == 'Esc':
                self.state['view']['editingText']=False
                self.state['view']['discardConfirmation']['open']=False
            elif key == 'Ctrl+A': pass
            else: raise AssertionError(key)

    def note(self, text): self.events.append(('note',text))


class ExportContext(FakeContext):
    def __init__(self, corrupt=False):
        super().__init__()
        self.corrupt=corrupt
        self.oracle_script=None

    def click(self,control,window,**kwargs):
        if control=='editorUpload':
            self.events.append(('click',control,window)); self.phase='completed'
        else: super().click(control,window,**kwargs)

    def call(self,method,params=None):
        if method=='operation.inspect' and self.phase=='completed':
            return {'state':'completed','items':[{'local':{'status':'saved','path':"/private/output's copy.png"},
                                                'artifact':{'mime':'image/png'}}]}
        return super().call(method,params)

    def shell(self,script,**kwargs):
        self.oracle_script=script
        # Execute the exact recipe pixel oracle with a controlled PPM. This
        # tests the oracle's rejection behaviour, never an app export.
        oracle=script.split("<<'ORACLE'\n",1)[1].rsplit('\nORACLE',1)[0]
        pixels=bytearray(640*320*3)
        colours=[(220,30,40),(20,180,70),(35,70,220),(235,190,20)]
        for y in range(320):
            for x in range(640):
                rgb=colours[(y>=160)*2+(x>=320)]
                if 340<=x<600 and 20<=y<140: rgb=(0,0,0)
                pixels[(y*640+x)*3:(y*640+x+1)*3]=bytes(rgb)
        if self.corrupt: pixels[(50*640+400)*3]=20
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'output.ppm'
            path.write_bytes(b'P6\n640 320\n255\n'+pixels)
            result=subprocess.run([sys.executable,'-c',oracle,str(path)],capture_output=True,text=True)
        return {'returncode':result.returncode,'stdout':result.stdout,'stderr':result.stderr}


class EditorRecipes(unittest.TestCase):
    def test_exact_ownership_once(self):
        owned={i for i,lane in json.loads((ROOT/'tools/lab/surface_driver/ownership.json').read_text())['owners'].items() if lane=='editor'}
        recipes=[r for r in REGISTRY if r.function.__module__==editor.__name__]
        actual=[i for r in recipes for i in r.interactions]
        self.assertEqual(owned,set(actual))
        self.assertEqual(len(actual),len(set(actual)))
        for r in recipes:
            self.assertEqual(('image',),r.needs)
            self.assertEqual(('local',),r.destinations)

    def test_fresh_session_frames_inspect_and_cleanup(self):
        ctx=FakeContext(); editor.redact(ctx)
        self.assertIn(('view','editor.view',{'id':'op-1','action':'fit'}),ctx.events)
        self.assertEqual(['before','after'],[e[1] for e in ctx.events if e[0]=='frame'])
        self.assertTrue(all(e[3] for e in ctx.events if e[0]=='frame'))
        self.assertTrue(any(e[0]=='drag' for e in ctx.events))
        self.assertEqual('cancelled',ctx.phase)

    def test_bad_mouse_geometry_fails_and_still_cleans_up(self):
        ctx=FakeContext(); ctx.bad_geometry=True
        with self.assertRaisesRegex(RecipeFailed,'image geometry'): editor.redact(ctx)
        self.assertEqual('cancelled',ctx.phase)

    def test_wrong_tool_kind_fails(self):
        ctx=FakeContext(); ctx.bad_kind=True
        with self.assertRaisesRegex(RecipeFailed,'chosen kind'): editor.redact(ctx)

    def test_unknown_canvas_geometry_blocks_not_passes(self):
        ctx=FakeContext()
        original=ctx.geometry
        def missing(role):
            if role=='editor.canvas': raise RecipeBlocked('owned canvas unavailable',issue=663)
            return original(role)
        ctx.geometry=missing
        with self.assertRaises(RecipeBlocked) as raised: editor.redact(ctx)
        self.assertEqual(663,raised.exception.issue)
        self.assertEqual('cancelled',ctx.phase)

    def test_real_key_history_and_buttons(self):
        ctx=FakeContext(); editor.mark_history(ctx)
        for keys in [('Ctrl+Z',),('Ctrl+Shift+Z',),('Ctrl+Y',),('Insert',),('Delete',),('Backspace',)]:
            self.assertIn(('keys',keys),ctx.events)
        self.assertIn(('click','text:Remove mark','editor'),ctx.events)

    def test_typing_tool_letters_and_escape_preserves_label(self):
        ctx=FakeContext(); editor.text_entry(ctx)
        self.assertEqual('xtract',ctx.state['shapes'][0]['text'])
        self.assertIn(('type_text','cancelled draft'),ctx.events)
        self.assertEqual('editing',ctx.state['phase'])

    def test_discard_question_keeps_then_stores_nothing(self):
        ctx=FakeContext(); editor.discard(ctx)
        self.assertIn(('frame','question','confirm','19-editor-discard-confirm'),ctx.events)
        self.assertIn(('click','text:Keep editing','confirm'),ctx.events)
        self.assertEqual('cancelled',ctx.phase)

    def test_all_pending_recipes_explain_block_after_frames(self):
        names=['preparation_failure']
        for name in names:
            with self.subTest(name=name):
                ctx=FakeContext()
                with self.assertRaises(RecipeBlocked) as raised: getattr(editor,name)(ctx)
                self.assertEqual(663,raised.exception.issue)
                self.assertGreater(len(raised.exception.reason),30)
                self.assertEqual(['before','after','failure-before-cleanup'],[e[1] for e in ctx.events if e[0]=='frame'])
                self.assertEqual('cancelled',ctx.phase)

    def test_export_oracle_accepts_sanitized_pixels_and_quotes_path(self):
        ctx=ExportContext(); editor.upload(ctx)
        self.assertEqual('completed',ctx.phase)
        self.assertIn("putgd-desktop-fixture --convert '/private/output",ctx.oracle_script)
        self.assertNotIn('from tools.lab',ctx.oracle_script)

    def test_export_oracle_rejects_a_single_source_pixel_in_mask(self):
        ctx=ExportContext(corrupt=True)
        with self.assertRaisesRegex(RecipeFailed,'every mask pixel'): editor.upload(ctx)

    def test_stable_controls_have_no_legacy_fallback(self):
        source=Path(editor.__file__).read_text()
        self.assertIn("ctx.click('colorSwatch'+name, window='editor')",source)
        self.assertIn("ctx.click('lineWidth', window='editor')",source)
        self.assertNotIn('_named_or_legacy',source)

    def test_copy_path_creation_rejects_bad_shell_result(self):
        ctx=FakeContext()
        ctx.shell=lambda script: {'returncode':0,'stdout':'/private/real-user-folder','stderr':''}
        with self.assertRaisesRegex(RecipeFailed,'new private test directory'): editor._copy_path(ctx)

    def test_cleanup_error_preserves_original_failed_assertion(self):
        ctx=FakeContext(); ctx.bad_geometry=True
        original=ctx.call
        def call(method,params=None):
            if method=='operation.inspect': raise RuntimeError('transport disappeared')
            return original(method,params)
        ctx.call=call
        with self.assertRaisesRegex(RecipeFailed,'image geometry'): editor.redact(ctx)
        self.assertIn(('note','editor cleanup unavailable: RuntimeError'),ctx.events)

    def test_readiness_retries_editor_starting_envelope(self):
        ctx=FakeContext(); original=ctx.cli
        replies=[{'returncode':1,'stdout':json.dumps({'ok':False,'error':{'code':'editor_starting'}}),'stderr':''}]
        def cli(method,params,**kwargs):
            return replies.pop() if replies else original(method,params,**kwargs)
        ctx.cli=cli
        def wait(predicate,what,**kwargs):
            value=predicate() or predicate()
            ctx.expect(value,what)
            return value
        ctx.wait=wait
        editor.redact(ctx)
        self.assertEqual('cancelled',ctx.phase)

    def test_first_launch_gate_removed_and_foreground_verified(self):
        ctx=FakeContext(); editor.redact(ctx)
        raised=next(i for i,e in enumerate(ctx.events) if e[:2]==('call','editor.raise'))
        focused=ctx.events.index(('focus','editor'))
        drawn=next(i for i,e in enumerate(ctx.events) if e[0]=='drag')
        self.assertLess(raised,focused)
        self.assertLess(focused,drawn)
        self.assertNotIn('acceptance_block',json.loads((ROOT/'tools/lab/surface_driver/profiles/mint-cinnamon-x11.d/editor.json').read_text())['editor'])

    def test_open_failure_not_mistaken_for_success(self):
        ctx=FakeContext()
        original=ctx.call
        ctx.call=lambda method,params=None: {'ok':False,'error':{'code':'source_missing'}} if method=='editor.open' else original(method,params)
        with self.assertRaisesRegex(AssertionError,'source_missing'): editor.redact(ctx)


if __name__=='__main__': unittest.main()
