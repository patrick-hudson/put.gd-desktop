import base64,json,random,time
from tools.lab.surface_driver.redact import Redactor
# Deterministic synthetic bytes; no real image, credentials, URLs or account.
r=Redactor();data=base64.b64encode(random.Random(943).randbytes(65536)).decode()
start=time.monotonic();r.remember({'content':[{'type':'image','mimeType':'image/png','data':data}]})
print(json.dumps({'registerSeconds':time.monotonic()-start,'registeredCount':len(r._secrets),'registeredBytes':sum(map(len,r._secrets)),'maxRegisteredBytes':max(map(len,r._secrets),default=0)}),flush=True)
value={'frames':[{'label':'before','path':'frames/review/synthetic.png'} for _ in range(10)]}
start=time.monotonic();r.safe(value);print(json.dumps({'safeSeconds':time.monotonic()-start,'fields':20}),flush=True)
