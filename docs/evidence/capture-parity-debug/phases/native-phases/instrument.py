from pathlib import Path
import ast,json,hashlib
root=Path('/home/patrick/.codex/worktrees/resident-native-boundary/put.gd');out=Path('/home/patrick/work/put.gd/work/capture-parity/native-phases')
prior=ast.parse(Path('/home/patrick/work/put.gd/work/capture-parity/trace1/instrument.py').read_text())
header=next(n.args[0].value for n in ast.walk(prior) if isinstance(n,ast.Call) and isinstance(n.func,ast.Attribute) and n.func.attr=='write_text' and n.args and isinstance(n.args[0],ast.Constant) and isinstance(n.args[0].value,str) and n.args[0].value.startswith('#pragma once'))
old_changes=next(ast.literal_eval(n.value) for n in prior.body if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='changes' for t in n.targets))
changes={'src/app/OperationService.cpp':old_changes['src/app/OperationService.cpp'],'src/capture/main.cpp':[
 (' QApplication app(argc,argv);',' parityTrace("app-before");QApplication app(argc,argv);parityTrace("app-after");'),
 (' const auto build=QJsonDocument',' parityTrace("adapters-after");const auto build=QJsonDocument'),
 ('const auto target=native.resolve(frozen,resolver,error);','parityTrace("resolve-before");const auto target=native.resolve(frozen,resolver,error);parityTrace("resolve-after");')],
 'src/platform/x11/NativeRegionOverlay.cpp':[
 ('void NativeRegionOverlay::start(){','void NativeRegionOverlay::start(){parityTrace("native-start");'),
 (' cursorWindow_->setCursor(Qt::CrossCursor);',' parityTrace("cursor-before");cursorWindow_->setCursor(Qt::CrossCursor);parityTrace("cursor-after");'),
 (' cursorWindow_->winId();',' parityTrace("winid-before");cursorWindow_->winId();parityTrace("winid-after");'),
 (' cursorWindow_->show();',' parityTrace("show-before");cursorWindow_->show();parityTrace("show-after");'),
 (' initializationQueued_=true;',' parityTrace("exposed-queued");initializationQueued_=true;'),
 ('void NativeRegionOverlay::initializeSurfaces(){','void NativeRegionOverlay::initializeSurfaces(){parityTrace("native-init");'),
 (' for(const auto& surface:surfaces_)xcb_map_window(connection_,surface.window);',' parityTrace("native-map-before");for(const auto& surface:surfaces_)xcb_map_window(connection_,surface.window);'),
 (' notifier_=new QSocketNotifier',' parityTrace("native-map-flushed");notifier_=new QSocketNotifier')]
}
manifest={}
for name,replacements in changes.items():
 p=root/name;s=p.read_text();backup=out/'originals'/name;backup.parent.mkdir(parents=True,exist_ok=True);assert not backup.exists();backup.write_text(s);manifest[name]=hashlib.sha256(p.read_bytes()).hexdigest()
 for old,new in replacements:
  assert s.count(old)==1,(name,old,s.count(old));s=s.replace(old,new)
 p.write_text('#include "core/CaptureParityTrace.h" // CAPTURE_PARITY_TRACE\n'+s)
(out/'originals.json').write_text(json.dumps(manifest,indent=2)+'\n');(root/'src/core/CaptureParityTrace.h').write_text(header)
