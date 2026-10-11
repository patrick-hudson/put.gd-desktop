from pathlib import Path
import json,hashlib
root=Path('/home/patrick/.codex/worktrees/resident-native-boundary/put.gd');out=Path('/home/patrick/work/put.gd/work/capture-parity/native-start-diagnostic');p=root/'src/platform/x11/NativeRegionOverlay.cpp';s=p.read_text();(out/'original.cpp').write_text(s);(out/'original.sha256').write_text(hashlib.sha256(p.read_bytes()).hexdigest()+'\n')
header=(Path('/home/patrick/work/put.gd/work/capture-parity/trace1/instrument.py').read_text().split(".write_text(,1)[1].split()",1)[0]);header=header.replace('\\n','\n');(root/'src/core/CaptureParityTrace.h').write_text(header)
s='#include "core/CaptureParityTrace.h" // CAPTURE_PARITY_TRACE\n'+s
s=s.replace('const bool visualOk=','parityTrace(geometry&&geometry->depth==32?"depth32":"wrong-depth");parityTrace(attributes&&!attributes->override_redirect?"managed":"wrong-managed");parityTrace(attributes&&attributes->map_state==XCB_MAP_STATE_VIEWABLE?"viewable":"not-viewable");\n const bool visualOk=')
s=s.replace('if(!pointerGrabbed_){','parityTrace(pointerGrabbed_?"pointer-grab-ok":"pointer-grab-failed");if(!pointerGrabbed_){')
s=s.replace('if(!keyboardGrabbed_){','parityTrace(keyboardGrabbed_?"keyboard-grab-ok":"keyboard-grab-failed");if(!keyboardGrabbed_){')
p.write_text(s)
