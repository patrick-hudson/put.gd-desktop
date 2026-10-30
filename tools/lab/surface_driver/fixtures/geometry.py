"""Recipe points resolved from live core window and accessible geometry."""
from ..api import RecipeBlocked
from .base import Fixture


def framebuffer(box, point):
    return [round((point[i]-box.get(('screen_x','screen_y')[i],0))*box.get('scale',1)) for i in (0,1)]


def history_window(scope):
    ctx=scope.ctx
    ctx.fixture('app-profile')
    ctx.view('ui.show',{'view':'history'})
    title=ctx.profile.get('windows',{}).get('history',{}).get('title') or ctx.profile.get('window_titles',{}).get('history',r'^put\.gd$')
    # Core 2a geometry deliberately raises for absent windows. Poll the
    # read-only presence API, then wait for the accessible header to be ready.
    ctx.wait(lambda:ctx.windows({'title':title}),'visible history window')
    ctx.focus('history')
    def layout():
        box=ctx.geometry('history')
        nodes=ctx.accessible({'owned':True,'visible':True,'pid':box['pid'],'within':[box[k] for k in ('x','y','width','height')]})
        menus=[n for n in nodes if n.get('name')=='Application menu' and n.get('rect')]
        if len(menus)>1:
            raise RecipeBlocked('history header has ambiguous Application menu anchors',issue=675)
        return (box,nodes,menus[0]) if menus else None
    box,nodes,menu=ctx.wait(layout,'accessible history header')
    # The header's blank area is between actual live controls, never its center
    # (which contains navigation). Subtract every visible node at anchor height.
    _,y,_,h=menu['rect'];y+=h/2
    intervals=[(box['x']+2,box['x']+box['width']-2)]
    for node in nodes:
        rect=node.get('rect')
        if not rect or node.get('role') in ('frame','panel','filler','root pane','application'):
            continue
        x,top,w,h=rect
        if not top<=y<top+h:continue
        intervals=[part for left,right in intervals for part in ((left,min(right,x-2)),(max(left,x+w+2),right)) if part[1]>part[0]]
    if not intervals or max(right-left for left,right in intervals)<16:
        raise RecipeBlocked('history header has no verified blank drag target',issue=675)
    left,right=max(intervals,key=lambda v:v[1]-v[0])
    return Fixture(header_drag_point=framebuffer(box,((left+right)/2,y)),geometry=box)


class LiveFixture(Fixture):
    """Canvas coordinates must be observed after the recipe opens its editor."""
    def __init__(self, ctx, **values):
        super().__init__(values,editor_mark_start=None,editor_mark_end=None)
        self._ctx=ctx

    def __getitem__(self, key):
        if key in ('editor_mark_start','editor_mark_end'):
            box=self._ctx.geometry('editor.canvas')
            fraction=.35 if key=='editor_mark_start' else .65
            return framebuffer(box,(box['x']+box['width']*fraction,box['y']+box['height']*fraction))
        return super().__getitem__(key)

    def get(self,key,default=None):
        return self[key] if key in self else default
