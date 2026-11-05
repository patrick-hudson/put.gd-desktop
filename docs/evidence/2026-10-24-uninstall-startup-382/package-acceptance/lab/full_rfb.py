"""Require a complete nonincremental RFB framebuffer for acceptance pictures.
Some servers split an initial response across updates; do not fill missing areas
with black and treat the result as a complete screenshot.
"""
import sys,struct,time
sys.path.insert(0,'tools/lab')
from rfb import RFB as BaseRFB
class RFB(BaseRFB):
 def frame(self):
  pixels=bytearray(self.width*self.height*4);covered=bytearray(self.width*self.height);deadline=time.monotonic()+15
  self.sock.sendall(struct.pack('>BBHHHH',3,0,0,0,self.width,self.height))
  while time.monotonic()<deadline:
   kind=self._read(1)[0]
   if kind==0:
    self._read(1);rects=struct.unpack('>H',self._read(2))[0]
    for _ in range(rects):
     x,y,w,h,enc=struct.unpack('>HHHHi',self._read(12))
     if enc==-223:
      self.width,self.height=w,h;pixels=bytearray(w*h*4);covered=bytearray(w*h);continue
     if enc!=0:raise RuntimeError('unexpected RFB encoding '+str(enc))
     assert x+w<=self.width and y+h<=self.height
     data=self._read(w*h*4)
     for row in range(h):
      start=(y+row)*self.width+x;pixels[start*4:(start+w)*4]=data[row*w*4:(row+1)*w*4];covered[start:start+w]=b'\1'*w
    if all(covered):return self.width,self.height,bytes(pixels)
    self.sock.sendall(struct.pack('>BBHHHH',3,0,0,0,self.width,self.height))
   elif kind==1:
    self._read(3);n=struct.unpack('>H',self._read(2))[0];self._read(n*6)
   elif kind==2:pass
   elif kind==3:self._read(3);self._read(struct.unpack('>I',self._read(4))[0])
   else:raise RuntimeError('unexpected RFB message '+str(kind))
  raise RuntimeError('incomplete RFB response: '+str(sum(covered))+'/'+str(len(covered))+' pixels; no screenshot saved')
