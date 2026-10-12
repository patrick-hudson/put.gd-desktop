import json,sys,subprocess
from pathlib import Path
r=Path(__file__).resolve().parent;s=json.loads((r/'steps.json').read_text());old,label,kind,*values=sys.argv[1:];p=r/'inputs'/(label+'.json');subprocess.run(['python3',str(r/'inputs/make-input.py'),s[old],label,kind,*values],check=True);subprocess.run(['python3',str(r/'invoke.py'),label,'input','--visual-input',str(p)],check=True)
