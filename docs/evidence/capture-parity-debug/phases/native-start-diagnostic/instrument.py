from pathlib import Path
import json,hashlib
root=Path('/home/patrick/.codex/worktrees/resident-native-boundary/put.gd');out=Path('/home/patrick/work/put.gd/work/capture-parity/native-start-diagnostic');p=root/'src/platform/x11/NativeRegionOverlay.cpp';s=p.read_text();(out/'original.cpp').write_text(s);(out/'original.sha256').write_text(hashlib.sha256(p.read_bytes()).hexdigest()+'\n')
import ast
source=Path('/home/patrick/work/put.gd/work/capture-parity/trace1/instrument.py').read_text()
header=next(n.args[0].value for n in ast.walk(ast.parse(source)) if isinstance(n,ast.Call) and isinstance(n.func,ast.Attribute) and n.func.attr=='write_text' and n.args and isinstance(n.args[0],ast.Constant) and isinstance(n.args[0].value,str) and n.args[0].value.startswith('#pragma once'))
(root/'src/core/CaptureParityTrace.h').write_text(header)
s='#include "core/CaptureParityTrace.h" // CAPTURE_PARITY_TRACE\n'+s
s=s.replace('const bool visualOk=','parityTrace(geometry&&geometry->depth==32?"depth32":"wrong-depth");parityTrace(attributes&&!attributes->override_redirect?"managed":"wrong-managed");parityTrace(attributes&&attributes->map_state==XCB_MAP_STATE_VIEWABLE?"viewable":"not-viewable");\n const bool visualOk=')
s=s.replace('if(!pointerGrabbed_){','parityTrace(pointerGrabbed_?"pointer-grab-ok":"pointer-grab-failed");if(!pointerGrabbed_){')
s=s.replace('if(!keyboardGrabbed_){','parityTrace(keyboardGrabbed_?"keyboard-grab-ok":"keyboard-grab-failed");if(!keyboardGrabbed_){')
p.write_text(s)
