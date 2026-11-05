from pathlib import Path
from PIL import Image,ImageDraw,ImageFont
import sys,math
host=sys.argv[1];mode=sys.argv[2];root=Path('work/i382n/evidence')/host/mode
labels=['enabled-settings','enabled-login','login-settings','inactive-cleaned-login','rearmed-settings','active-cleaned','active-cleaned-login','disabled-settings','disabled-clean-login']
font=ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf',16);w=640;h=360;items=[]
for label in labels:
 p=root/(label+'-visual.json')
 if not p.exists():continue
 import json
 d=json.loads(p.read_text());items.append((label,root/d['file']))
out=Image.new('RGB',(w*2,(h+28)*math.ceil(len(items)/2)),(230,232,235));draw=ImageDraw.Draw(out)
for i,(label,p) in enumerate(items):
 im=Image.open(p).convert('RGB');im.thumbnail((w,h));x=(i%2)*w;y=(i//2)*(h+28);draw.text((x+8,y+5),host+' '+mode+' · '+label,fill=(20,20,24),font=font);out.paste(im,(x,y+28))
out.save(root/'visual-review-contact.png');print(root/'visual-review-contact.png')
