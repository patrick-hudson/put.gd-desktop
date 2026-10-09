#!/usr/bin/env python3
import argparse
from common import OUT,RUN,now,save,load_lab,assert_resident,ui
p=argparse.ArgumentParser();p.add_argument('--label',required=True);a=p.parse_args();assert a.label.replace('-','').isalnum();out=OUT/(a.label+'.json');assert not out.exists()
lab=load_lab();data=lab.cli(RUN,'settings.get');save(out,{'timestampUtc':now(),'resident':assert_resident(lab),'ui':ui(lab),'acceptedRevision':data['metadata']['acceptedRevision'],'archiveRoot':data['document']['archive']['root'],'notifications':data['document']['preferences']['notifications']})
