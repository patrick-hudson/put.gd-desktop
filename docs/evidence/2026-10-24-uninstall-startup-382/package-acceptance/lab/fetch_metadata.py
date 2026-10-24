"""Fetch selected authenticated CI ZIP members without downloading debug/source archives.
The gh token and redirected signed URL exist only in memory and are never logged.
"""
import sys,subprocess,urllib.request,urllib.error,urllib.parse,json,io,zipfile,hashlib
from pathlib import Path
REPO='patrick-hudson/put.gd-desktop'
class NoRedirect(urllib.request.HTTPRedirectHandler):
 def redirect_request(self,*a,**kw):return None
class Ranges(io.RawIOBase):
 def __init__(self,url):
  self.url=url;self.pos=0;self.block=4*1024*1024;self.cache_start=-1;self.cache=b'';self.transferred=0
  r=urllib.request.urlopen(urllib.request.Request(url,method='HEAD'),timeout=30);self.size=int(r.headers['Content-Length']);r.close();assert 0<self.size<3*1024**3
 def readable(self):return True
 def seekable(self):return True
 def tell(self):return self.pos
 def seek(self,n,whence=0):
  self.pos=n if whence==0 else self.pos+n if whence==1 else self.size+n;return self.pos
 def read(self,n=-1):
  if n<0:n=self.size-self.pos
  n=min(n,self.size-self.pos)
  if n<=0:return b''
  if self.cache_start<=self.pos and self.pos+n<=self.cache_start+len(self.cache):
   data=self.cache[self.pos-self.cache_start:self.pos-self.cache_start+n];self.pos+=n;return data
  start=self.pos;end=min(self.size-1,start+max(n,self.block)-1)
  req=urllib.request.Request(self.url,headers={'Range':f'bytes={start}-{end}'})
  with urllib.request.urlopen(req,timeout=90) as r:
   assert r.status==206,'range not supported';self.cache=r.read();self.cache_start=start;self.transferred+=len(self.cache)
  data=self.cache[:n];self.pos+=len(data);return data
def main():
 run=int(sys.argv[1]);name=sys.argv[2];dest=Path(sys.argv[3]);assert not dest.exists();dest.mkdir(parents=True,mode=0o700)
 metadata=json.loads(subprocess.check_output(['gh','api',f'repos/{REPO}/actions/runs/{run}/artifacts'],text=True));artifact=next(x for x in metadata['artifacts'] if x['name']==name);assert not artifact['expired']
 token=subprocess.check_output(['gh','auth','token'],text=True).strip()
 req=urllib.request.Request(f'https://api.github.com/repos/{REPO}/actions/artifacts/{artifact["id"]}/zip',headers={'Authorization':'Bearer '+token,'Accept':'application/vnd.github+json','X-GitHub-Api-Version':'2022-11-28'})
 opener=urllib.request.build_opener(NoRedirect)
 try:opener.open(req,timeout=30);raise RuntimeError('expected artifact redirect')
 except urllib.error.HTTPError as e:
  assert e.code in (301,302,303,307,308);url=e.headers['Location']
 del token,req,opener
 parsed=urllib.parse.urlsplit(url);assert parsed.scheme=='https' and parsed.hostname.endswith(('.blob.core.windows.net','.githubusercontent.com','.amazonaws.com'))
 remote=Ranges(url);z=zipfile.ZipFile(remote);files={}
 for entry in z.infolist():
  p=Path(entry.filename);assert not p.is_absolute() and '..' not in p.parts
  if len(p.parts)!=1 or entry.is_dir():continue
  if p.suffix!='.json':continue
  target=dest/p.name
  with z.open(entry) as src,target.open('wb') as out:
   h=hashlib.sha256()
   while b:=src.read(1024*1024):out.write(b);h.update(b)
  files[p.name]={'sha256':h.hexdigest(),'bytes':entry.file_size,'zipCrc32':entry.CRC}
 receipt={'run':run,'artifactId':artifact['id'],'name':name,'zipBytes':remote.size,'transferredBytes':remote.transferred,'members':files};(dest/'download-receipt.json').write_text(json.dumps(receipt,indent=2)+'\n');print(json.dumps(receipt),flush=True)
if __name__=='__main__':
 try:main()
 except Exception as exc:
  # Never print request URLs, headers, tokens, or signed redirect values.
  print(json.dumps({'errorType':type(exc).__name__,'httpStatus':getattr(exc,'code',None)}));sys.exit(1)
