#!/usr/bin/env python3
import argparse,time
from PIL import Image
from common import OUT,RUN,now,save,load_lab
p=argparse.ArgumentParser();p.add_argument('--name',required=True);a=p.parse_args();assert a.name.replace('-','').isalnum();output=OUT/(a.name+'.json');assert not output.exists()
lab=load_lab();script='''set -eu
session_id=$(loginctl show-user "$(id -u)" -p Display --value)
[ "$(loginctl show-session "$session_id" -p User --value)" = "$(id -u)" ]
[ "$(loginctl show-session "$session_id" -p Type --value)" = wayland ]
loginctl unlock-session "$session_id"
'''
lab.ssh(RUN,script);lab.key(RUN,['esc']);time.sleep(.6);w=lab.screenshot(RUN,'u11-'+a.name);Image.open(w['path']).save(OUT/(a.name+'.png'))
save(output,{'timestampUtc':now(),'script':script,'visibleKeys':['esc'],'witness':w,'vncPort':5934,'policyChanged':False})
print(str(OUT/(a.name+'.png')))
