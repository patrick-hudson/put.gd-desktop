"""Light recipe-contract tests. These fakes are never desktop evidence."""
import copy
import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.lab.surface_driver.api import Context, REGISTRY, RecipeBlocked, RecipeFailed
from tools.lab.surface_driver.recipes import editor


class FakeContext(Context):
    def __init__(self):
        self.events = []
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
        raise AssertionError(method)

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

    def geometry(self, window):
        self.events.append(('geometry',window))
        assert window == 'editor.canvas'
        return {'x':100,'y':100,'width':800,'height':500}

    def click(self, control, window, **kwargs):
        self.events.append(('click',control,window))
        if control.startswith('text:') and control[5:].lower() in ('redact','arrow','rectangle','highlight','text','select','blur','crop'):
            self.state['view']['tool']=control[5:].lower()
        elif control == 'placeMark': self._add({'kind':'redact','rect':[220,110,200,100]})
        elif control == 'removeMark': self.state['shapes']=[]
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
        def missing(role): raise NotImplementedError
        ctx.geometry=missing
        with self.assertRaises(RecipeBlocked) as raised: editor.redact(ctx)
        self.assertEqual(663,raised.exception.issue)
        self.assertEqual('cancelled',ctx.phase)

    def test_real_key_history_and_buttons(self):
        ctx=FakeContext(); editor.mark_history(ctx)
        for keys in [('Ctrl+Z',),('Ctrl+Shift+Z',),('Ctrl+Y',),('Insert',),('Delete',),('Backspace',)]:
            self.assertIn(('keys',keys),ctx.events)
        self.assertIn(('click','removeMark','editor'),ctx.events)

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
        names=['held_modifiers','double_click','window_chrome',
               'save_copy','preparation_failure']
        for name in names:
            with self.subTest(name=name):
                ctx=FakeContext()
                with self.assertRaises(RecipeBlocked) as raised: getattr(editor,name)(ctx)
                self.assertEqual(663,raised.exception.issue)
                self.assertGreater(len(raised.exception.reason),30)
                self.assertEqual(['before','after'],[e[1] for e in ctx.events if e[0]=='frame'])
                self.assertEqual('cancelled',ctx.phase)

    def test_open_failure_not_mistaken_for_success(self):
        ctx=FakeContext()
        original=ctx.call
        ctx.call=lambda method,params=None: {'ok':False,'error':{'code':'source_missing'}} if method=='editor.open' else original(method,params)
        with self.assertRaisesRegex(AssertionError,'source_missing'): editor.redact(ctx)


if __name__=='__main__': unittest.main()
