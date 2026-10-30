"""Small real-input ports of edit_acceptance.py; no cloud destinations."""
from tools.lab.surface_driver.api import recipe, RecipeFailed
import json
from weakref import WeakKeyDictionary

_editors = WeakKeyDictionary()


def cancelled(ctx, identity):
    def request():
        envelope=json.loads(ctx.cli('editor.cancel',json.dumps({'id':identity}))['stdout'])
        if envelope.get('ok'):
            return True
        code=envelope.get('error',{}).get('code')
        if code=='editor_starting':
            return False
        if code in ('editor_unavailable','editor_closed','editor_finalizing'):
            return True
        raise RecipeFailed('resident editor.cancel: '+str(code))
    ctx.wait(request,'editor cancel accepted',10)
    def closed():
        envelope = json.loads(ctx.cli('editor.inspect', json.dumps({'id':identity}))['stdout'])
        if envelope.get('ok'):
            return False
        code = envelope.get('error', {}).get('code')
        if code in ('editor_closed', 'editor_finalizing', 'editor_unavailable'):
            # Helper shutdown can precede resident cleanup; inspect the
            # resident's editor admission state too, rather than sleeping.
            return ctx.call('capabilities')['editor']['open'] is False
        raise RecipeFailed('resident editor.inspect after cancel: '+str(code))
    ctx.wait(closed, 'editor closed after cancel', 10)
    _editors.pop(ctx,None)


def cleanup(ctx):
    identity=_editors.get(ctx)
    if identity:
        cancelled(ctx,identity)
    elif ctx.call('capabilities')['editor']['open']:
        raise RecipeFailed('cannot identify the open editor; further recipes must not run')


def opened(ctx):
    result=ctx.call('editor.open',{'source':'file','path':ctx.fixture('image')['path']})
    identity=result['operationId']
    _editors[ctx]=identity
    def ready():
        reply=ctx.cli('editor.inspect','{"id":"'+identity+'"}')
        envelope=json.loads(reply['stdout'])
        return envelope['result'] if envelope.get('ok') else None
    ctx.wait(ready,'editor opened',30)
    ctx.call('editor.raise',{'id':identity})
    ctx.view('editor.view',{'id':identity,'action':'inspect'})
    return identity


@recipe('editor.tool-text',timeout=60)
def text_tool(ctx):
    identity=opened(ctx)
    ctx.keys('T')
    ctx.wait(lambda:ctx.call('editor.view',{'id':identity}).get('tool')=='text','real keyboard selected text')
    ctx.frame('text-tool',window='editor')


@recipe('editor.tool-redact',timeout=60)
def redact_tool(ctx):
    identity=opened(ctx)
    ctx.keys('T')
    ctx.use_catalog('editor','20-editor-redact')
    ctx.click('text:Redact',window='editor')
    ctx.wait(lambda:ctx.call('editor.view',{'id':identity}).get('tool')=='redact','real catalog click selected redact')
    ctx.frame('redact-tool',window='editor',reference='20-editor-redact')


@recipe('editor.undo','editor.redo',timeout=60)
def undo_redo(ctx):
    identity=opened(ctx)
    ctx.call('editor.apply',{'id':identity,'commands':[{'op':'add','shape':{'kind':'redact','rect':[10,10,60,30]}}]})
    ctx.keys('Ctrl+Z')
    ctx.wait(lambda:not ctx.call('editor.inspect',{'id':identity})['shapes'],'real keyboard undo')
    ctx.keys('Ctrl+Shift+Z')
    ctx.wait(lambda:len(ctx.call('editor.inspect',{'id':identity})['shapes'])==1,'real keyboard redo')
    ctx.frame('redone',window='editor')


# The runner always invokes cleanup, even when opening or an action times out.
for action in (text_tool,redact_tool,undo_redo):
    action.cleanup=cleanup
