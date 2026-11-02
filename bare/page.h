#pragma once

#include <Arduino.h>

static const char PAGE_HTML[] = R"PAGE(<!doctype html><html lang=en><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><title>KP3S</title>
<style>
:root{--f:#111;--b:#fff;--l:#ccc;--d:#666}
@media(prefers-color-scheme:dark){:root{--f:#ddd;--b:#15171a;--l:#444;--d:#999}}
body{font:15px/1.4 system-ui,sans-serif;color:var(--f);background:var(--b);margin:0 auto;padding:1rem;max-width:44rem}
h1{font-size:1.2rem;margin:0}h2{font-size:.85rem;text-transform:uppercase;color:var(--d);margin:0 0 .4rem}
small{font-weight:400;color:var(--d);font-size:.8rem}
section{border-top:1px solid var(--l);padding:.8rem 0}
input,button,select{font:inherit;padding:.3rem .5rem;color:var(--f);background:var(--b);border:1px solid var(--l);border-radius:4px}
button{cursor:pointer}button:hover{border-color:var(--d)}
#bn p{margin:.3rem 0;padding:.5rem .7rem;border-radius:4px;font-weight:600;color:#000}
.nx{background:#ff8a80}.nw{background:#ffd166}.ng{background:#8ce99a}.ni{background:#0001;color:var(--f);font-weight:400}
.b{height:6px;background:#0002;border-radius:3px;overflow:hidden;margin:.4rem 0}
.b i{display:block;height:100%;width:0;background:#4c9aff;transition:width .3s}
pre{white-space:pre-wrap;word-break:break-word;background:#0001;padding:.5rem;border-radius:4px;max-height:11rem;overflow:auto;margin:.4rem 0;font-size:.8rem}
.f{display:flex;gap:.5rem;align-items:center;padding:.2rem 0;border-bottom:1px solid var(--l)}
.f b{flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;font-weight:600}
.f span{color:var(--d);font-size:.8rem}
#ms{min-height:1.2em;color:var(--d);font-size:.85rem;margin:.4rem 0}
form{display:inline-flex;gap:.4rem;flex-wrap:wrap;align-items:center;margin:0}
</style>

<h1>KP3S module <small id=hd>connecting</small></h1>
<div id=bn></div>
<div id=ms></div>

<section><h2>Print</h2>
<div id=pg>nothing seen yet</div><div class=b><i id=pb></i></div>
<button id=can>cancel print</button> <button id=clr>clear log</button>
<pre id=lg></pre></section>

<section><h2>Transfer to printer card</h2>
<div id=tx>idle</div><div class=b><i id=tb></i></div></section>

<section><h2>Module card</h2>
<form id=uf><input type=file name=f required><button>upload</button></form>
<button id=rf>refresh</button><div class=b><i id=ub></i></div>
<div id=fl>loading</div></section>

<section><h2>Console</h2>
<form id=gf><input id=gc placeholder=M115 required size=22><button>send</button></form>
<form id=bf>baud <input id=br type=number value=115200 size=7><button>set</button></form>
<pre id=go></pre></section>

<section><h2>Module firmware</h2>
<form id=ff><input type=file name=firmware required><button>flash and reboot</button></form>
<div class=b><i id=fb></i></div></section>

<script>
var $=function(i){return document.getElementById(i)}
var q=Promise.resolve(),st=null,tr=null,off=0,P={b:0,m:0,at:0,done:0,log:''}
try{P=Object.assign(P,JSON.parse(localStorage.kp3s||'{}'))}catch(e){}
function save(){try{localStorage.kp3s=JSON.stringify(P)}catch(e){}}
function esc(s){return String(s).replace(/[&<>"]/g,function(c){return{'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]})}
function pc(a,b){return b>0?Math.min(100,Math.round(a*100/b)):0}
function kb(n){return n>=1024?Math.round(n/1024)+'k':n+''}
function hms(s){var h=s/3600|0,m=s/60%60|0;return h?h+'h'+m+'m':m+'m'+s%60+'s'}
function note(t){$('ms').textContent=(t||'').trim().split('\n')[0]}

function one(u,o){
  q=q.then(function(){
    return fetch(u,o).then(function(r){
      return r.text().then(function(t){return{s:r.status,t:t}})
    },function(){return{s:0,t:''}})
  })
  return q
}
function post(u,fd,bar){
  q=q.then(function(){
    return new Promise(function(done){
      var x=new XMLHttpRequest()
      x.upload.onprogress=function(e){if(e.lengthComputable)$(bar).style.width=pc(e.loaded,e.total)+'%'}
      x.onload=function(){done({s:x.status,t:x.responseText})}
      x.onerror=x.onabort=x.ontimeout=function(){done({s:0,t:''})}
      x.open('POST',u);x.send(fd)
    })
  })
  return q
}

function bump(t){
  if(!t)return
  var re=/SD printing byte (\d+)\/(\d+)/g,m,got=0
  while(m=re.exec(t)){P.b=+m[1];P.m=+m[2];got=1}
  if(got){P.at=Date.now();P.done=0}
  if(/Done printing/i.test(t)){P.done=1;P.at=Date.now()}
  if(/\S/.test(t))P.log=(P.log+t.replace(/\r/g,'')).slice(-2500)
  save()
}
function busy(){return tr&&tr.phase&&'idle done failed'.indexOf(tr.phase)<0}
function printing(){return(P.m>0&&!P.done&&Date.now()-P.at<25000)||(st&&st.printing)}

function draw(){
  $('hd').textContent=off>1?'offline':(st?'slot '+st.slot+' · up '+hms(st.uptime)+' · heap '+kb(st.heap)+' (block '+kb(st.block)+')':'connecting')
  var n=[]
  if(off>1)n.push(['x','the module is not answering, '+off+' tries'])
  else if(st&&!st.card)n.push(['x','the module has no card'])
  if(busy())n.push(['w','transfer '+tr.phase+' '+pc(tr.written,tr.total)+'% of '+(tr.name||'?')])
  if(printing())n.push(['g','printing '+pc(P.b,P.m)+'% · '+P.b+'/'+P.m+' bytes'])
  if(!n.length&&st)n.push(['i','idle'])
  $('bn').innerHTML=n.map(function(x){return'<p class=n'+x[0]+'>'+esc(x[1])+'</p>'}).join('')

  $('tx').textContent=tr?tr.phase+' · '+tr.written+'/'+tr.total+' bytes · '+tr.packets+' packets, '+tr.resends+' resends · '+tr.took+(tr.trouble!='none'?' · trouble: '+tr.trouble:''):'unknown'
  $('tb').style.width=tr?pc(tr.written,tr.total)+'%':'0'
  $('pg').textContent=P.m?(P.done?'done · ':'')+P.b+' / '+P.m+' bytes ('+pc(P.b,P.m)+'%)':'nothing seen yet'
  $('pb').style.width=pc(P.b,P.m)+'%'
  if($('lg').textContent!=P.log){$('lg').textContent=P.log;$('lg').scrollTop=1e6}
}

function pick(t,k){var m=t.match(new RegExp('^'+k+' +(.*)$','m'));return m?m[1]:''}
function cycle(){
  one('/api/status').then(function(r){
    if(r.s==200){try{st=JSON.parse(r.t);off=0}catch(e){off++}}else off++
    return one('/printer/send/status')
  }).then(function(r){
    if(r.s==200){
      var w=pick(r.t,'written').split(' ')
      tr={phase:pick(r.t,'phase'),name:pick(r.t,'name'),written:+w[0]||0,total:+w[2]||0,
          packets:(pick(r.t,'packets').split(',')[0]||'0'),resends:(pick(r.t,'packets').split('resends')[1]||'').trim(),
          took:pick(r.t,'took'),trouble:pick(r.t,'trouble')||'none'}
    }
    if(busy())return{s:0,t:''}
    return one('/printer/progress')
  }).then(function(r){
    if(r.s==200)bump(r.t)
    draw()
    setTimeout(cycle,off?5000:(busy()?1200:(printing()?3000:4000)))
  })
}

function files(){
  $('fl').textContent='loading'
  one('/files').then(function(r){
    if(r.s!=200){$('fl').textContent=(r.t||'the module did not answer').trim();return}
    var rows=r.t.replace(/\s+$/,'').split('\n').map(function(l){
      var i=l.lastIndexOf('  ');if(i<0)return''
      var n=l.slice(0,i).replace(/^\//,''),z=+l.slice(i+2)||0
      return'<div class=f><b>'+esc(n)+'</b><span>'+kb(z)+'</span>'+
        '<button data-s="'+esc(n)+'">to printer</button><button data-p="'+esc(n)+'">print</button></div>'
    }).join('')
    $('fl').innerHTML=rows||esc(r.t.trim())
  })
}

$('fl').onclick=function(e){
  var s=e.target.getAttribute('data-s'),p=e.target.getAttribute('data-p')
  if(s){
    if(!confirm('send '+s+' to the printer card?'))return
    note('asking for '+s)
    one('/printer/send?name='+encodeURIComponent(s),{method:'POST'}).then(function(r){note(r.t||'no answer')})
  }else if(p){
    if(!confirm('print '+p+'?'))return
    one('/printer/print?name='+encodeURIComponent(p),{method:'POST'}).then(function(r){note(r.t||'no answer');bump(r.t);draw()})
  }
}
$('rf').onclick=files
$('clr').onclick=function(){P.log='';P.m=0;P.b=0;P.done=0;save();draw()}
$('can').onclick=function(){
  if(!confirm('cancel the running print?'))return
  one('/printer/cancel',{method:'POST'}).then(function(r){note('cancel sent');$('go').textContent=r.t||'no answer';bump(r.t)})
}
$('uf').onsubmit=function(e){
  e.preventDefault()
  var f=e.target.f.files[0];if(!f)return
  if(busy()){note('a transfer is running, wait for it');return}
  note('uploading '+f.name)
  var fd=new FormData();fd.append('f',f,f.name)
  post('/upload',fd,'ub').then(function(r){
    note(r.s==200?'stored '+f.name:(r.t||'the upload failed').trim())
    $('ub').style.width='0';files()
  })
}
$('ff').onsubmit=function(e){
  e.preventDefault()
  var f=e.target.firmware.files[0];if(!f)return
  if(!confirm('flash '+f.name+' and reboot the module?'))return
  if(printing())if(!confirm('a print looks to be running. the printer prints from its own card, so it should survive. go on?'))return
  note('flashing '+f.name)
  var fd=new FormData();fd.append('firmware',f,f.name)
  post('/update',fd,'fb').then(function(r){note(r.s==200?'flashed, the module is rebooting':(r.t||'the flash failed').trim())})
}
$('gf').onsubmit=function(e){
  e.preventDefault()
  var c=$('gc').value.trim();if(!c)return
  one('/gcode?cmd='+encodeURIComponent(c)).then(function(r){
    $('go').textContent=r.t||'the module did not answer';bump(r.t);draw()
  })
}
$('bf').onsubmit=function(e){
  e.preventDefault()
  var n=+$('br').value
  if(!confirm('move both ends to '+n+' baud?'))return
  one('/printer/baud?rate='+n,{method:'POST'}).then(function(r){$('go').textContent=r.t||'no answer'})
}
draw();files();cycle()
</script>
)PAGE";

static String pageHtml() { return String(PAGE_HTML); }
