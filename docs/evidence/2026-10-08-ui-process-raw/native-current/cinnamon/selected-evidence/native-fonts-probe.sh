python3 - <<'PROBE'
import json,pathlib,subprocess
pid=54419
paths=set()
for line in pathlib.Path('/proc',str(pid),'maps').read_text().splitlines():
 fields=line.split()
 if len(fields)>=6:
  path=fields[-1]
  if any(x in path for x in ('libgtk-3','libgdk-3','libqgtk3','libQt6Gui','libQt6Widgets','libfontconfig')):paths.add(path)
r=subprocess.run(['gsettings','get','org.cinnamon.desktop.interface','font-name'],capture_output=True,text=True,check=True)
print(json.dumps({'helperPid':pid,'nativeIntegrationLibraries':sorted(paths),'cinnamonFontSetting':r.stdout.strip()}))
PROBE
