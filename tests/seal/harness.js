// Test only (#2079): opens every vector's sealed page in an iframe, as a recipient would,
// and checks what the pinned viewer shows and lets them download. Results go to /result.
"use strict";
(async()=>{
 const results=[];const note=(id,check,ok,detail)=>results.push({id,check,ok:!!ok,detail:detail===undefined?"":String(detail)});
 const hex=b=>[...new Uint8Array(b)].map(x=>x.toString(16).padStart(2,"0")).join("");
 const unhex=h=>new Uint8Array((h.match(/../g)||[]).map(x=>parseInt(x,16)));
 const sha=async b=>hex(await crypto.subtle.digest("SHA-256",b));
 const b64=b=>{let s="";for(let i=0;i<b.length;i+=32768)s+=String.fromCharCode.apply(null,b.subarray(i,i+32768));return btoa(s)};
 const frag=k=>"v1."+b64(k).replace(/\+/g,"-").replace(/\//g,"_").replace(/=+$/,"");
 const content=c=>{if("text" in c)return new TextEncoder().encode(c.text);if("hex" in c)return unhex(c.hex);
  const n=c.pattern,o=new Uint8Array(n);for(let i=0;i<n;i++)o[i]=(i*31+7)%251;return o};
 const sleep=ms=>new Promise(r=>setTimeout(r,ms));
 const frame=document.getElementById("frame");
 async function load(url){
  // Only the page asked for counts: on a slow machine the previous page can still be in the frame.
  const want=new URL(url,location.href).pathname;
  frame.src="about:blank";
  for(let i=0;i<100;i++){await sleep(20);const d=frame.contentDocument;if(d&&d.URL==="about:blank")break}
  frame.src=url;
  for(let i=0;i<1200;i++){await sleep(100);const d=frame.contentDocument;if(!d||d.URL==="about:blank"||new URL(d.URL).pathname!==want||d.readyState==="loading")continue;
   const st=d.documentElement.dataset.state,ask=d.getElementById("ask");
   if(st||(ask&&!ask.hidden))return d}
  return frame.contentDocument;
 }
 async function waitState(d){for(let i=0;i<1200;i++){if(d.documentElement.dataset.state)return d.documentElement.dataset.state;const a=d.getElementById("ask");if(a&&!a.hidden)return "ask";await sleep(100)}return "timeout"}
 async function submit(d,pass){d.getElementById("pass").value=pass;d.getElementById("ask").requestSubmit();await sleep(200)}
 async function downloaded(d){const a=d.getElementById("save");if(!a||a.hidden)return null;const r=await fetch(a.href);return {sha:await sha(await r.arrayBuffer()),type:r.headers.get("Content-Type")||"",name:a.download}}
 const status=d=>d.getElementById("status").textContent;
 try{
  const doc=await (await fetch("/vectors.json")).json();
  for(const v of doc.vectors){
   const file=content(v.content),want=await sha(file),key=unhex(v.link);
   const c=await SealRef.seal(file,{name:v.name,type:v.type},key,unhex(v.salt),unhex(v.noncePrefix),v.passphrase,v.iterations);
   note(v.id,"reference seal equals C++",await sha(c)===v.containerSha256,await sha(c));
   if(!v.container)await fetch("/container/"+v.id,{method:"POST",body:b64(c)});
   const forms=v.id==="multi"||v.id==="passphrase"?["page","split"]:["page"];
   for(const form of forms){
    let d=await load("/"+form+"/"+v.id+".html#"+frag(key));
    if(v.passphrase){
     note(v.id,form+": asks for the passphrase",await waitState(d)==="ask");
     await submit(d,"wrong passphrase");
     note(v.id,form+": wrong passphrase",await waitState(d)==="ask"&&status(d)==="That passphrase doesn't open this link.",status(d));
     await submit(d,v.passphrase.normalize("NFD"));
    }
    const st=await waitState(d);note(v.id,form+": opens",st==="open",st+" "+status(d));
    if(st!=="open")continue;
    const got=await downloaded(d);note(v.id,form+": download is the file",got&&got.sha===want,got&&got.sha);
    note(v.id,form+": download name",got&&got.name===v.name,got&&got.name);
    note(v.id,form+": name shown",d.getElementById("name").textContent===v.name);
    const view=d.getElementById("view");
    if(v.type==="text/plain")note(v.id,form+": text shown",view.querySelector("pre")&&await sha(new TextEncoder().encode(view.querySelector("pre").textContent))===want);
    if(v.type.startsWith("image/")){const img=view.querySelector("img");for(let i=0;i<50&&img&&!img.complete;i++)await sleep(100);note(v.id,form+": image shown",img&&img.naturalWidth>0,img&&img.naturalWidth)}
    if(v.type==="text/html")note(v.id,form+": HTML only downloads",!view.children.length&&got&&got.type==="application/octet-stream",got&&got.type);
    if(v.type==="application/octet-stream")note(v.id,form+": other files only download",!view.children.length);
   }
  }
  // A changed key, a key left out, a changed byte of the data and a changed viewer.
  const h=doc.vectors.find(v=>v.id==="hello"),k=unhex(h.link);k[3]^=1;
  let d=await load("/page/hello.html#"+frag(k));await waitState(d);note("hello","wrong key",status(d)==="This link is incomplete or damaged.",status(d));
  d=await load("/page/hello.html");await waitState(d);note("hello","missing key",status(d).startsWith("This link is missing its key."),status(d));
  d=await load("/tampered/hello.html#"+frag(unhex(h.link)));await waitState(d);note("hello","changed data",status(d)==="This link is incomplete or damaged.",status(d));
  // Too large for the browser to read back: Save to file writes the file, or a plain message without a save picker.
  const m=doc.vectors.find(v=>v.id==="multi"),mf=frag(unhex(m.link)),mwant=await sha(content(m.content));
  d=await load("/large/multi.html#"+mf);let st=await waitState(d);const sf=d.getElementById("savefile");
  note("multi","too large: offers Save to file",st==="open"&&!sf.hidden&&d.getElementById("save").hidden&&!d.getElementById("view").children.length
   &&status(d)==="This file is too large for this browser to show. Select Save to file to save it.",st+" "+status(d));
  sf.click();for(let i=0;i<100&&status(d)!=="Saved.";i++)await sleep(100);
  const w=frame.contentWindow;
  note("multi","too large: saved file is the file",status(d)==="Saved."&&w.saved&&await sha(w.saved)===mwant,status(d));
  note("multi","too large: saved name",w.savedName===m.name,w.savedName);
  d=await load("/large-nosave/multi.html#"+mf);st=await waitState(d);
  note("multi","too large without a save picker",st==="open"&&d.getElementById("savefile").hidden&&d.getElementById("save").hidden
   &&status(d)==="This file is too large for this browser. Open the link in Firefox, or use putgd-cli open.",st+" "+status(d));
  d=await load("/split-missing/multi.html#"+frag(unhex(doc.vectors.find(v=>v.id==="multi").link)));await waitState(d);note("multi","split without its data",status(d)==="The sealed file isn't at this address any more.",status(d));
 }catch(e){note("harness","ran",false,e&&e.stack||e)}
 await fetch("/result",{method:"POST",body:JSON.stringify({userAgent:navigator.userAgent,results})});
})();
