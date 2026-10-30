"""Disposable loopback S3 subset. No auth/header/URL logs; synthetic objects only.

Not a cloud emulator or a provider acceptance oracle. The production official AWS
SDK still signs its requests and handles the responses. Tests of signatures and
service edge cases remain in tests/fixtures/s3_server.py.
"""
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import threading
import time
from urllib.parse import parse_qs, unquote, urlsplit
import uuid
from xml.sax.saxutils import escape


class Server(ThreadingHTTPServer):
    daemon_threads=True
    def __init__(self, root):
        super().__init__(('127.0.0.1',0),Handler)
        self.root=Path(root)
        self.objects={};self.uploads={};self.events=[]
        self.lock=threading.RLock()

    def control(self):
        return json.loads((self.root/'control.json').read_text())


class Handler(BaseHTTPRequestHandler):
    protocol_version='HTTP/1.1'
    def log_message(self,*args):pass
    def reply(self,status,body=b'',headers=None):
        self.send_response(status)
        for k,v in (headers or {}).items():self.send_header(k,str(v))
        self.send_header('Content-Length',str(len(body)))
        self.send_header('x-amz-request-id','SYNTHETIC')
        self.end_headers()
        if self.command!='HEAD':
            try:self.wfile.write(body)
            except (BrokenPipeError,ConnectionResetError):pass
    def error(self,code,name):
        self.reply(code,('<Error><Code>'+name+'</Code><Message>synthetic</Message></Error>').encode())
    def xml(self,tag,body):
        self.reply(200,('<'+tag+' xmlns="http://s3.amazonaws.com/doc/2006-03-01/">'+body+'</'+tag+'>').encode())
    def do_HEAD(self):self.wire()
    def do_GET(self):self.wire()
    def do_PUT(self):self.wire()
    def do_POST(self):self.wire()
    def do_DELETE(self):self.wire()
    def wire(self):
        parsed=urlsplit(self.path);query=parse_qs(parsed.query,keep_blank_values=True)
        key=unquote(parsed.path).lstrip('/');command=self.command
        body=self.rfile.read(int(self.headers.get('Content-Length','0')))
        if key=='__health':return self.reply(200,b'{}')
        if key=='__events':
            with self.server.lock:return self.reply(200,json.dumps(self.server.events).encode())
        ctl=self.server.control()
        if command in ('PUT','POST'):
            deadline=time.monotonic()+ctl.get('stallSeconds',0)
            while time.monotonic()<deadline:
                time.sleep(.1)
                if not self.server.control().get('stallSeconds'):break
            if ctl.get('failWrites'):return self.error(403,'AccessDenied')
        with self.server.lock:
            self.server.events.append({'method':command,'bytes':len(body),'multipart':'uploadId' in query or 'uploads' in query})
            self.server.events[:]=self.server.events[-1024:]
            if 'uploads' in query and command=='POST':
                identity=uuid.uuid4().hex
                self.server.uploads[identity]={'key':key,'parts':{},'headers':self.metadata()}
                return self.xml('InitiateMultipartUploadResult','<Bucket>'+escape(key.split('/')[0])+'</Bucket><Key>'+escape(key.split('/',1)[-1])+'</Key><UploadId>'+identity+'</UploadId>')
            if 'uploadId' in query:
                identity=query['uploadId'][0];upload=self.server.uploads.get(identity)
                if not upload:return self.error(404,'NoSuchUpload')
                if command=='DELETE':
                    if ctl.get('denyAbort'):return self.error(403,'AccessDenied')
                    del self.server.uploads[identity];return self.reply(204)
                if command=='PUT':
                    if ctl.get('failParts'):return self.error(403,'AccessDenied')
                    upload['parts'][int(query['partNumber'][0])]=body
                    return self.reply(200,headers={'ETag':'"'+hashlib.md5(body).hexdigest()+'"'})
                if command=='GET':
                    return self.xml('ListPartsResult','<IsTruncated>false</IsTruncated>'+''.join('<Part><PartNumber>%d</PartNumber><ETag>"%s"</ETag><Size>%d</Size></Part>'%(n,hashlib.md5(b).hexdigest(),len(b)) for n,b in sorted(upload['parts'].items())))
                if command=='POST':
                    body=b''.join(v for k,v in sorted(upload['parts'].items()))
                    self.server.objects[key]=(body,upload['headers']);del self.server.uploads[identity]
                    return self.xml('CompleteMultipartUploadResult','<Key>'+escape(key.split('/',1)[-1])+'</Key><ETag>"'+hashlib.md5(body).hexdigest()+'"</ETag>')
            if 'versioning' in query:return self.xml('VersioningConfiguration','')
            if key and '/' not in key and command in ('HEAD','GET'):
                return self.reply(200)
            obj=self.server.objects.get(key)
            if command=='PUT':
                if self.headers.get('if-none-match')=='*' and obj:return self.error(412,'PreconditionFailed')
                self.server.objects[key]=(body,self.metadata())
                return self.reply(200,headers={'ETag':'"'+hashlib.md5(body).hexdigest()+'"'})
            if command=='DELETE':
                self.server.objects.pop(key,None);return self.reply(204)
            if not obj:return self.error(404,'NoSuchKey')
            data,headers=obj
            return self.reply(200,data,dict(headers,ETag='"'+hashlib.md5(data).hexdigest()+'"'))
    def metadata(self):
        return {k:v for k,v in self.headers.items() if k.lower().startswith('x-amz-meta-') or k.lower()=='content-type'}


if __name__=='__main__':
    import sys
    server=Server(sys.argv[1])
    (server.root/'port').write_text(str(server.server_address[1]))
    server.serve_forever()
