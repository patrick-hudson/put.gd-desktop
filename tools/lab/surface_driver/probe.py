"""Small target-side desktop probes, invoked under the private session environment."""
import fnmatch
import json
import os
import re
import subprocess
import sys


def accessible(query):
    import pyatspi
    matches = []
    desktop = pyatspi.Registry.getDesktop(0)
    stack = [(desktop, 0)]
    visited = 0
    while stack and visited < 10000:
        node, depth = stack.pop(); visited += 1
        try:
            role = node.getRoleName()
            name = node.name or ''
            attributes = dict(a.split(':', 1) for a in node.getAttributes() if ':' in a)
            if (not query.get('name') or fnmatch.fnmatchcase(name, query['name'])) and (not query.get('role') or role == query['role']):
                try:
                    rect = list(node.queryComponent().getExtents(pyatspi.DESKTOP_COORDS))
                except NotImplementedError:
                    rect = None
                matches.append({'name':name,'role':role,'rect':rect,'attributes':attributes,
                                'pid':node.get_process_id()})
            if depth < 30:
                stack.extend((child, depth + 1) for child in node if child is not None)
        except (RuntimeError, AttributeError, NotImplementedError):
            continue
    return matches


def geometry(query):
    title = query['title']
    if os.environ.get('XDG_SESSION_TYPE') == 'x11':
        # xdotool returns client geometry, matching QWidget catalog coordinates.
        search = subprocess.run(['xdotool','search','--onlyvisible','--name',title],text=True,capture_output=True)
        found = []
        for wid in search.stdout.split():
            pid = subprocess.run(['xdotool','getwindowpid',wid],text=True,capture_output=True)
            if pid.returncode:
                continue
            try:
                env = open('/proc/'+pid.stdout.strip()+'/environ','rb').read().split(b'\0')
            except OSError:
                continue
            if ('XDG_RUNTIME_DIR='+os.environ['XDG_RUNTIME_DIR']).encode() not in env:
                continue
            raw = subprocess.check_output(['xdotool','getwindowgeometry','--shell',wid],text=True)
            values = dict(line.split('=',1) for line in raw.splitlines() if '=' in line)
            found.append({'x':int(values['X']),'y':int(values['Y']),'width':int(values['WIDTH']),
                          'height':int(values['HEIGHT']),'scale':1,'window_id':wid,'coordinate_space':'framebuffer'})
        if len(found) == 1:
            return found[0]
        raise RuntimeError('live X11 window match is missing or ambiguous')
    matches = accessible({'name':'*'+title+'*'})
    frames = [m for m in matches if m['role'] in ('frame','dialog','window') and m['rect'] and m['rect'][2]>0]
    owned = []
    for row in frames:
        try:
            env = open('/proc/'+str(row['pid'])+'/environ','rb').read().split(b'\0')
            if ('XDG_RUNTIME_DIR='+os.environ['XDG_RUNTIME_DIR']).encode() in env:
                owned.append(row)
        except OSError:
            pass
    if len(owned) != 1:
        raise RuntimeError('live Wayland window geometry unavailable or ambiguous through AT-SPI')
    x,y,w,h = owned[0]['rect']
    return {'x':x,'y':y,'width':w,'height':h,'scale':query.get('scale',1),'coordinate_space':'logical'}


def main():
    request = json.loads(sys.argv[1])
    try:
        result = accessible(request['query']) if request['probe'] == 'accessible' else geometry(request['query'])
        print(json.dumps({'ok':True,'result':result}))
    except Exception as error:
        print(json.dumps({'ok':False,'error':type(error).__name__+': '+str(error)}))


if __name__ == '__main__':
    main()
