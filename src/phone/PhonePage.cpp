#include "PhonePage.h"

namespace clipitto::phone {

// Written in three pieces: one string literal may not be longer than about 16 KB on MSVC; adjacent literals are joined.
QByteArray pageHtml()
{
    static const QByteArray page = QByteArray(R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="color-scheme" content="light dark">
<meta name="referrer" content="no-referrer">
<title>ClipItTo</title>
<style>
:root{--bg:#f4f5f7;--fg:#1b1d21;--card:#fff;--line:#d5d8de;--accent:#1f6feb;--accent-fg:#fff;--ok:#1a7f37;--bad:#c62828;--dim:#5b616b}
@media (prefers-color-scheme:dark){:root{--bg:#14161a;--fg:#e8eaed;--card:#1e2126;--line:#353a42;--accent:#4c8dff;--accent-fg:#06101f;--ok:#56d364;--bad:#ff7b72;--dim:#9aa1ab}}
*{box-sizing:border-box}
body{margin:0;padding:16px;background:var(--bg);color:var(--fg);font:16px/1.4 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;-webkit-text-size-adjust:100%}
main{max-width:560px;margin:0 auto}
h1{font-size:1.3rem;margin:4px 0 4px}
p.hint{color:var(--dim);margin:0 0 16px;font-size:.92rem}
.btn{display:block;position:relative;width:100%;margin:0 0 12px;padding:20px 12px;border-radius:14px;border:0;background:var(--accent);color:var(--accent-fg);font:inherit;font-size:1.15rem;font-weight:600;text-align:center;cursor:pointer;-webkit-tap-highlight-color:transparent}
.btn.alt{background:var(--card);color:var(--fg);border:2px solid var(--accent)}
.btn input{position:absolute;left:0;top:0;width:100%;height:100%;opacity:0;cursor:pointer;font-size:0}
.btn:active{opacity:.85}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:12px;margin:16px 0}
textarea{width:100%;min-height:96px;padding:10px;border-radius:10px;border:1px solid var(--line);background:var(--bg);color:var(--fg);font:inherit;resize:vertical}
.send{margin:10px 0 0;padding:14px 12px;font-size:1rem}
ul{list-style:none;margin:0;padding:0}
li{padding:8px 0;border-top:1px solid var(--line);word-break:break-word}
li:first-child{border-top:0}
li.ok{color:var(--ok)}li.bad{color:var(--bad)}
#conn{color:var(--dim);font-size:.85rem;margin:12px 0 0}
</style></head><body><main>
<h1>Send to your computer</h1>
<p class="hint">A picture becomes its text on your computer. Nothing leaves your home network.</p>
<label class="btn">Take a picture<input id="cam" type="file" accept="image/*" capture="environment"></label>
<label class="btn alt">Choose from the gallery<input id="gal" type="file" accept="image/*" multiple></label>
<div class="card">
<textarea id="txt" placeholder="Or type or paste text here" maxlength="50000"></textarea>
<button id="go" class="btn alt send" type="button">Send text</button>
</div>
<ul id="log" class="card" hidden></ul>
<p id="conn"></p>
</main>
)HTML")
        + QByteArray(R"HTML(<script>
(function(){
'use strict';
var MAXSIDE=2400,QUALITY=0.9,queue=Promise.resolve(),log=document.getElementById('log');
function row(name){
  var li=document.createElement('li');log.hidden=false;
  log.insertBefore(li,log.firstChild);
  var o={set:function(msg,cls){li.textContent=name+': '+msg;li.className=cls||'';}};
  o.set('Preparing…');return o;
}
function wait(ms){return new Promise(function(r){setTimeout(r,ms);});}
function fromBitmap(file){
  if(!window.createImageBitmap)return Promise.reject(new Error('no bitmap'));
  return createImageBitmap(file,{imageOrientation:'from-image'}).then(function(b){return{src:b,w:b.width,h:b.height,done:function(){if(b.close)b.close();}};});
}
function fromImg(file){
  return new Promise(function(res,rej){
    var url=URL.createObjectURL(file),img=new Image();
    img.onload=function(){res({src:img,w:img.naturalWidth,h:img.naturalHeight,done:function(){URL.revokeObjectURL(url);}});};
    img.onerror=function(){URL.revokeObjectURL(url);rej(new Error('cannot load'));};
    img.src=url;
  });
}
// A gallery picture on an iPhone may be HEIC: draw it on a canvas and send a JPEG no bigger than MAXSIDE.
// If that fails for any reason, the original file goes and the computer says whether it can read it.
function convert(file){
  return fromBitmap(file).catch(function(){return fromImg(file);}).then(function(p){
    var s=Math.min(1,MAXSIDE/Math.max(p.w,p.h)),c=document.createElement('canvas');
    c.width=Math.max(1,Math.round(p.w*s));c.height=Math.max(1,Math.round(p.h*s));
    var x=c.getContext('2d');x.fillStyle='#fff';x.fillRect(0,0,c.width,c.height);x.drawImage(p.src,0,0,c.width,c.height);
    p.done();
    return new Promise(function(res,rej){c.toBlob(function(b){b?res(b):rej(new Error('no blob'));},'image/jpeg',QUALITY);});
  }).catch(function(){return file;});
}
function post(path,body,type){
  return fetch(path,{method:'POST',body:body,headers:{'Content-Type':type||'application/octet-stream'},cache:'no-store',credentials:'omit',referrerPolicy:'no-referrer'});
}
function bad(msg){var e=new Error(msg);e.ours=true;return e;}
function explain(status){
  return status===404?'this address has stopped working; open the window on your computer again'
    :status===413?'too large'
    :status===429?'the limit for this session is reached'
    :status===503?'the computer is busy'
    :'error '+status;
}
function send(path,body,type,r){
  r.set('Sending…');
  var tries=0;
  function attempt(){
    return post(path,body,type).then(function(resp){
      if(resp.status===503&&tries++<40){r.set('Waiting for the computer…');return wait(1500).then(attempt);}
      if(!resp.ok)throw bad(explain(resp.status));
      return resp.json();
    });
  }
  return attempt().then(function(j){
    r.set('Sent','');
    return follow(j.id,r);
  }).catch(function(e){
    r.set('Failed: '+(e&&e.ours?e.message:'no connection to your computer'),'bad');
  });
}
function follow(id,r){
  var n=0;
  function poll(){
    return fetch('status?id='+id,{cache:'no-store',credentials:'omit'}).then(function(resp){
      if(!resp.ok)throw bad(explain(resp.status));
      return resp.json();
    }).then(function(j){
      if(j.state==='reading'){r.set('Reading the text…');}
      else if(j.state==='done'){r.set('Done: '+j.chars+(j.chars===1?' character':' characters')+(j.note?' ('+j.note+')':''),'ok');return;}
      else if(j.state==='failed'){r.set('Failed: '+(j.error||'could not be read'),'bad');return;}
      if(++n>400)throw bad('no answer from your computer');
      return wait(700).then(poll);
    });
  }
  return poll().catch(function(e){r.set('Failed: '+(e&&e.ours?e.message:'no connection to your computer'),'bad');});
}
function sendPicture(file){
  var r=row(file.name||'Picture');
  queue=queue.then(function(){
    r.set('Preparing…');
    return convert(file).then(function(blob){return send('upload',blob,blob.type||file.type,r);});
  });
}
function pick(ev){
  var files=ev.target.files;
  for(var i=0;i<files.length;i++)sendPicture(files[i]);
  ev.target.value='';
}
document.getElementById('cam').addEventListener('change',pick);
document.getElementById('gal').addEventListener('change',pick);
document.getElementById('go').addEventListener('click',function(){
  var box=document.getElementById('txt'),text=box.value;
  if(!text.trim())return;
  var r=row('Text');box.value='';
  queue=queue.then(function(){return send('text',new Blob([text],{type:'text/plain;charset=utf-8'}),'text/plain;charset=utf-8',r);});
});
function ping(){
  fetch('status',{cache:'no-store',credentials:'omit'}).then(function(resp){
    document.getElementById('conn').textContent=resp.ok?'Connected to your computer.':'This address has stopped working.';
  }).catch(function(){document.getElementById('conn').textContent='No connection to your computer.';});
}
ping();setInterval(ping,10000);
})();
</script></body></html>
)HTML");
    return page;
}

} // namespace clipitto::phone
