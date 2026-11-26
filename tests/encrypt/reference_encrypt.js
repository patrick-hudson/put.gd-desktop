// Test only: format v1 encrypting in WebCrypto with a fixed salt and nonce prefix, the browser
// half of "both directions" (#2079). browser_vectors.py checks its bytes equal the C++
// containers in tests/fixtures/encrypt/vectors.json. The product never encrypts in a browser.
"use strict";
const EncryptRef=(()=>{
 const S=crypto.subtle,te=new TextEncoder(),CHUNK=1048576;
 const cat=(...a)=>{const n=a.reduce((s,x)=>s+x.length,0),c=new Uint8Array(n);let o=0;for(const x of a){c.set(x,o);o+=x.length}return c};
 const u32=v=>{const b=new Uint8Array(4);new DataView(b.buffer).setUint32(0,v);return b};
 const pos=(i,last)=>cat(u32(i),new Uint8Array([last?1:0]));
 const hkdf=async(ikm,salt,info)=>new Uint8Array(await S.deriveBits({name:"HKDF",hash:"SHA-256",salt,info:te.encode(info)},await S.importKey("raw",ikm,"HKDF",false,["deriveBits"]),256));
 async function encrypt(file,meta,key,salt,prefix,passphrase,iterations){
  const pass=!!passphrase,it=pass?iterations:0;
  const head=cat(te.encode("PGDSEAL1"),new Uint8Array([1,pass?1:0,0,0]),u32(CHUNK),u32(it),salt,prefix,new Uint8Array(5));
  const ck=await S.importKey("raw",await hkdf(key,salt,"put.gd sealed v1 check"),{name:"HMAC",hash:"SHA-256"},false,["sign"]);
  const header=cat(head,new Uint8Array(await S.sign("HMAC",ck,head)));
  let ikm=key;
  if(pass){const pk=await S.importKey("raw",te.encode(passphrase.normalize("NFC")),"PBKDF2",false,["deriveBits"]);
   ikm=cat(key,new Uint8Array(await S.deriveBits({name:"PBKDF2",hash:"SHA-256",salt,iterations:it},pk,256)))}
  const fk=await S.importKey("raw",await hkdf(ikm,salt,"put.gd sealed v1 payload"),"AES-GCM",false,["encrypt"]);
  const json=te.encode(JSON.stringify({name:meta.name,size:file.length,type:meta.type}));
  const plain=cat(u32(json.length),json,file),out=[header];
  for(let at=0,i=0;at<plain.length;at+=CHUNK,i++){const last=at+CHUNK>=plain.length,p=pos(i,last);
   out.push(new Uint8Array(await S.encrypt({name:"AES-GCM",iv:cat(prefix,p),additionalData:cat(header,p),tagLength:128},fk,plain.subarray(at,Math.min(at+CHUNK,plain.length)))))}
  return cat(...out);
 }
 return {encrypt};
})();
