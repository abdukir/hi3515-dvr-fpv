'use strict';
const $=(s,e=document)=>e.querySelector(s),$$=(s,e=document)=>[...e.querySelectorAll(s)];
const pad=n=>String(n).padStart(2,'0');

/* theme + nav */
$('#theme').onclick=()=>{const r=document.documentElement;const cur=r.getAttribute('data-theme')||(matchMedia('(prefers-color-scheme: dark)').matches?'dark':'light');r.setAttribute('data-theme',cur==='dark'?'light':'dark');};
$$('.navbtn').forEach(b=>b.onclick=()=>{$$('.navbtn').forEach(x=>x.classList.remove('active'));b.classList.add('active');$$('.view').forEach(v=>v.classList.remove('active'));$('#v-'+b.dataset.view).classList.add('active');
  const v=b.dataset.view;if(v==='live')fit();else if(v==='files')initFiles();else if(v==='term')initShell();else if(v==='rec')initClips();});

let NCH=4, streamType=0;
const CH=[]; const wall=$('#wall'), stage=$('#stage');

/* ---- build camera cells (with real <video>) ---- */
function buildCells(){
  wall.innerHTML='';CH.length=0;
  for(let i=0;i<NCH;i++){
    const c={n:i+1,idx:i,ws:null,jmuxer:null,frames:0,lastFrames:0,rec:false};
    const el=document.createElement('div');el.className='cam';
    el.innerHTML=`<video muted autoplay playsinline></video>
      <div class="badge"><span class="chip">CH ${c.n}</span><span class="chip state"><span class="dot" data-dot></span><span data-st>…</span></span></div>
      <div class="tr"></div>
      <div class="metrics" data-metrics>—</div>
      <div class="ctl">
        <button class="cambtn rb" title="Record"><svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="6" fill="currentColor" stroke="none"/></svg></button>
        <button class="cambtn snap" title="Snapshot"><svg viewBox="0 0 24 24"><path d="M23 19a2 2 0 0 1-2 2H3a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h4l2-3h6l2 3h4a2 2 0 0 1 2 2z"/><circle cx="12" cy="13" r="4"/></svg></button>
      </div>`;
    c.el=el;c.video=el.querySelector('video');
    el.querySelector('.rb').onclick=e=>{e.stopPropagation();toggleRecord(c);};
    el.querySelector('.snap').onclick=e=>{e.stopPropagation();snapshot(c);};
    el.addEventListener('dblclick',e=>{if(e.target.closest('.cambtn'))return;toggleSolo(i);});
    wall.appendChild(el);CH.push(c);
  }
}

/* ---- streaming: one WS + jMuxer per visible channel ---- */
function startStream(c){
  if(c.ws)return;
  const proto=location.protocol==='https:'?'wss':'ws';
  const jm=new JMuxer({node:c.video,mode:'video',flushingTime:0,fps:30,clearBuffer:true,debug:false});
  const ws=new WebSocket(`${proto}://${location.host}/stream?ch=${c.idx}&stream=${streamType}`);
  ws.binaryType='arraybuffer';
  ws.onmessage=ev=>{c.frames++;jm.feed({video:new Uint8Array(ev.data)});setState(c,'live');};
  ws.onclose=()=>setState(c,'off');
  ws.onerror=()=>setState(c,'off');
  c.ws=ws;c.jmuxer=jm;setState(c,'conn');
}
function stopStream(c){
  if(c.ws){try{c.ws.close();}catch(e){}c.ws=null;}
  if(c.jmuxer){try{c.jmuxer.destroy();}catch(e){}c.jmuxer=null;}
  setState(c,'off');
}
function setState(c,s){
  const dot=c.el.querySelector('[data-dot]'),st=c.el.querySelector('[data-st]');
  dot.className='dot'+(s==='live'?' live':'');
  st.textContent=s==='live'?'LIVE':s==='conn'?'…':'OFF';
  c.el.classList.toggle('online',s==='live');
}
function snapshot(c){
  if(!c.video.videoWidth)return;
  const cv=document.createElement('canvas');cv.width=c.video.videoWidth;cv.height=c.video.videoHeight;
  cv.getContext('2d').drawImage(c.video,0,0);
  const a=document.createElement('a');a.download=`CH${c.n}_snap.png`;a.href=cv.toDataURL('image/png');a.click();
}

/* sync which channels are streaming to the visible set */
function syncStreams(){
  const vis=visibleChannels();
  CH.forEach((c,i)=>{ if(vis.includes(i)) startStream(c); else stopStream(c); });
}

/* ---- layouts ---- */
const LAY={'1x1':{cols:1,rows:1,ar:[4,3],count:1},'2x1':{cols:2,rows:1,ar:[8,3],count:2},'2x2':{cols:2,rows:2,ar:[4,3],count:4}};
let curLay='2x2',page=0;
function pageCount(){return Math.ceil(NCH/LAY[curLay].count);}
function visibleChannels(){const c=LAY[curLay].count,start=(page*c)%NCH,out=[];for(let i=0;i<c;i++)out.push((start+i)%NCH);return out;}
function applyLayout(){
  const L=LAY[curLay];if(page>=pageCount())page=0;
  $$('#layseg button').forEach(b=>b.classList.toggle('on',b.dataset.lay===curLay));
  wall.style.gridTemplateColumns=`repeat(${L.cols},1fr)`;wall.style.gridTemplateRows=`repeat(${L.rows},1fr)`;
  const vis=visibleChannels();
  CH.forEach((c,i)=>c.el.style.display=vis.includes(i)?'':'none');
  $('#chnav').style.display=(curLay==='2x2')?'none':'inline-flex';
  $('#chlabel').textContent=vis.map(i=>'CH '+CH[i].n).join(' · ');
  syncStreams();fit();
}
function setLayout(lay,focusIdx){curLay=lay;if(focusIdx!=null)page=Math.floor(focusIdx/LAY[lay].count);applyLayout();}
let soloReturn=null;
function toggleSolo(idx){
  if(curLay==='1x1'){const r=soloReturn||{lay:'2x2',page:0};soloReturn=null;curLay=r.lay;page=r.page;applyLayout();}
  else{soloReturn={lay:curLay,page:page};curLay='1x1';page=idx;applyLayout();}
}
$$('#layseg button').forEach(b=>b.onclick=()=>{soloReturn=null;setLayout(b.dataset.lay);});
$('#chprev').onclick=()=>{page=(page-1+pageCount())%pageCount();applyLayout();};
$('#chnext').onclick=()=>{page=(page+1)%pageCount();applyLayout();};
$$('#streamseg button').forEach(b=>b.onclick=()=>{$$('#streamseg button').forEach(x=>x.classList.remove('on'));b.classList.add('on');streamType=parseInt(b.dataset.s,10);CH.forEach(stopStream);syncStreams();});

function fit(){
  const availW=stage.clientWidth-28,availH=stage.clientHeight-28;if(availW<=0||availH<=0)return;
  const[aw,ah]=LAY[curLay].ar;let w=availW,h=w*ah/aw;if(h>availH){h=availH;w=h*aw/ah;}
  wall.style.width=Math.floor(w)+'px';wall.style.height=Math.floor(h)+'px';
}
new ResizeObserver(fit).observe(stage);window.addEventListener('resize',fit);

/* ---- fullscreen (real API + CSS fallback) ---- */
let maxed=false;
function updateFsLabel(){$('#fsbtn').lastChild.textContent=(document.fullscreenElement||maxed)?' Exit':' Fullscreen';}
function setMax(on){maxed=on;$('#v-live').classList.toggle('maximized',on);updateFsLabel();setTimeout(fit,80);}
$('#fsbtn').onclick=async()=>{
  const el=$('#v-live'),isFs=document.fullscreenElement||maxed;
  if(!isFs){if(el.requestFullscreen){try{await el.requestFullscreen();updateFsLabel();return;}catch(e){}}setMax(true);}
  else{if(document.fullscreenElement){try{await document.exitFullscreen();}catch(e){}}if(maxed)setMax(false);}
};
document.addEventListener('fullscreenchange',()=>{updateFsLabel();setTimeout(fit,60);});
document.addEventListener('keydown',e=>{if(e.key==='Escape'&&maxed)setMax(false);});

/* ---- record control (DVR-side) ---- */
async function toggleRecord(c){
  const on=!c.rec;
  c.el.querySelector('.rb').classList.toggle('rec-on',on);   // optimistic
  try{
    const r=await fetch(`/api/record?ch=${c.idx}&on=${on?1:0}`,{method:'POST'});
    const j=await r.json();
    if(j.state)applyRecState(j.state);
  }catch(e){/* revert on failure */ pollRecState();}
}
function applyRecState(state){
  let n=0;
  CH.forEach((c,i)=>{const on=!!state[i];c.rec=on;c.el.querySelector('.rb').classList.toggle('rec-on',on);
    c.el.querySelector('.tr').innerHTML=on?'<span class="chip state" style="color:#ffd2d2"><span class="dot rec"></span>REC</span>':'';if(on)n++;});
  $('#recpill').style.display=n?'inline-flex':'none';$('#recn').textContent=n;
}
async function pollRecState(){
  try{const r=await fetch('/api/recordstate');if(r.ok){const j=await r.json();applyRecState(j.state);setConn(true);}else setConn(false);}
  catch(e){setConn(false);}
}
function setConn(ok){const d=$('#conndot'),l=$('#connlbl');d.className='dot'+(ok?' live':'');l.textContent=ok?'Online':'DVR offline';}

/* fps + live-edge snap */
setInterval(()=>{
  CH.forEach(c=>{
    if(c.el.style.display==='none')return;
    const f=c.frames-c.lastFrames;c.lastFrames=c.frames;
    const m=c.el.querySelector('[data-metrics]');
    m.textContent=f?`${Math.round(f/2)} fps`:'—';
    if(c.video.buffered.length){const end=c.video.buffered.end(c.video.buffered.length-1);if(end-c.video.currentTime>0.6)c.video.currentTime=end-0.05;}
  });
},2000);
setInterval(pollRecState,3000);
setInterval(()=>{const d=new Date();$('#dvrclock').textContent=`${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`;},1000);

/* ---- boot ---- */
fetch('/api/config').then(r=>r.json()).then(cfg=>{NCH=cfg.channels||4;boot();}).catch(()=>boot());
function boot(){buildCells();setLayout('2x2');pollRecState();setTimeout(fit,120);}

/* ================= FILE MANAGER ================= */
let curPath='/root/rec/a1';
function initFiles(){ loadDir(curPath); }
function fmt(n){ if(!n)return''; const u=['B','KB','MB','GB'];let i=0,v=n;while(v>=1024&&i<3){v/=1024;i++;}return (i?v.toFixed(1):v)+' '+u[i]; }
async function loadDir(p){
  const tb=$('#filetable');tb.innerHTML='<tr><td colspan="4" style="color:var(--dim)">loading…</td></tr>';
  try{
    const d=await (await fetch('/api/files?path='+encodeURIComponent(p))).json();
    if(d.error){tb.innerHTML='<tr><td colspan="4" style="color:var(--err)">'+d.error+'</td></tr>';return;}
    curPath=d.path;renderCrumb(d.path);
    const FOLDER='<svg viewBox="0 0 24 24"><path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/></svg>';
    const FILE='<svg viewBox="0 0 24 24"><path d="M14 3H6a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V9z"/><path d="M14 3v6h6"/></svg>';
    const rows=d.entries.sort((a,b)=>(b.dir-a.dir)||a.name.localeCompare(b.name)).map(e=>{
      const full=(d.path==='/'?'':d.path)+'/'+e.name;
      const esc=full.replace(/'/g,"\\'");
      if(e.name==='..'){const up=d.path.replace(/\/[^/]+\/?$/,'')||'/';
        return '<tr><td><div class="fname dir" style="cursor:pointer" onclick="loadDir(\''+up.replace(/'/g,"\\'")+'\')"><svg viewBox="0 0 24 24"><path d="M9 6l-6 6 6 6M3 12h13a4 4 0 0 1 0 8"/></svg><b>..</b></div></td><td></td><td></td><td></td></tr>';}
      const nameCell=e.dir
        ?'<div class="fname dir" style="cursor:pointer" onclick="loadDir(\''+esc+'\')">'+FOLDER+'<b>'+e.name+'</b></div>'
        :'<div class="fname">'+FILE+'<b>'+e.name+'</b></div>';
      const act=e.dir?''
        :'<button class="rowbtn" onclick="location.href=\'/api/download?path='+encodeURIComponent(full)+'\'">Download</button> <button class="rowbtn" onclick="delFile(\''+esc+'\')">Delete</button>';
      return '<tr><td>'+nameCell+'</td><td class="mono" style="color:var(--dim)">'+fmt(e.size)+'</td><td class="mono" style="color:var(--dim)">'+(e.mtime||'')+'</td><td style="text-align:right">'+act+'</td></tr>';
    }).join('');
    tb.innerHTML=rows||'<tr><td colspan="4" style="color:var(--dim)">empty</td></tr>';
  }catch(e){tb.innerHTML='<tr><td colspan="4" style="color:var(--err)">'+e.message+'</td></tr>';}
}
function renderCrumb(p){
  const parts=p.split('/').filter(Boolean);let acc='';
  const links=['<b style="cursor:pointer" onclick="loadDir(\'/\')">/</b>'];
  parts.forEach(seg=>{acc+='/'+seg;links.push('<b style="cursor:pointer" onclick="loadDir(\''+acc.replace(/'/g,"\\'")+'\')">'+seg+'</b>');});
  $('#crumb').innerHTML=links.join(' <span style="color:var(--dim-2)">/</span> ');
}
async function delFile(f){ if(!confirm('Delete '+f+' ?'))return; await fetch('/api/fs',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({action:'delete',path:f})}); loadDir(curPath); }
$('#frefresh').onclick=()=>loadDir(curPath);
$('#fmkdir').onclick=async()=>{const n=prompt('New folder name:');if(!n)return;await fetch('/api/fs',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({action:'mkdir',path:curPath+'/'+n})});loadDir(curPath);};
window.loadDir=loadDir;window.delFile=delFile;

/* ================= WEB TERMINAL ================= */
let shWs=null, shHist=[], shHi=0;
function initShell(){ if(shWs&&shWs.readyState<=1)return; connectShell(); }
function connectShell(){
  $('#termbody').textContent='';
  $('#shstat').textContent='connecting…';$('#shdot').className='dot';
  const proto=location.protocol==='https:'?'wss':'ws';
  const ws=new WebSocket(proto+'://'+location.host+'/shell');shWs=ws;
  ws.onopen=()=>{$('#shstat').textContent='root@'+location.hostname;$('#shdot').className='dot live';};
  ws.onmessage=ev=>appendTerm(ev.data);
  ws.onclose=()=>{$('#shstat').textContent='disconnected';$('#shdot').className='dot';};
  ws.onerror=()=>{$('#shstat').textContent='error';};
}
function appendTerm(s){
  const body=$('#termbody');
  const clean=s.replace(/\x1b\[[0-9;?]*[a-zA-Z]/g,'').replace(/\r/g,'');
  body.appendChild(document.createTextNode(clean));
  body.scrollTop=body.scrollHeight;
}
$('#terminput').addEventListener('keydown',e=>{
  if(e.key==='Enter'){const v=e.target.value;if(shWs&&shWs.readyState===1)shWs.send(v+'\n');if(v.trim()){shHist.push(v);shHi=shHist.length;}e.target.value='';}
  else if(e.key==='ArrowUp'){if(shHi>0){shHi--;e.target.value=shHist[shHi]||'';}e.preventDefault();}
  else if(e.key==='ArrowDown'){if(shHi<shHist.length){shHi++;e.target.value=shHist[shHi]||'';}e.preventDefault();}
});
$('#shreconnect').onclick=connectShell;

/* ================= RECORDINGS ================= */
let clipsInit=false;
function initClips(){ if(!clipsInit){clipsInit=true;$('#recch').onchange=loadRecordings;$('#recrefresh').onclick=loadRecordings;} loadRecordings(); }
function tstr(ep){const d=new Date(ep*1000);return d.toLocaleDateString()+' '+pad(d.getHours())+':'+pad(d.getMinutes())+':'+pad(d.getSeconds());}
function tclock(ep){const d=new Date(ep*1000);return pad(d.getHours())+':'+pad(d.getMinutes())+':'+pad(d.getSeconds());}
function durstr(s){s=Math.max(0,Math.round(s));return s<60?s+'s':Math.floor(s/60)+'m '+pad(s%60)+'s';}
async function loadRecordings(){
  const ch=parseInt($('#recch').value,10);
  const list=$('#reclist');list.innerHTML='<div style="color:var(--dim);padding:20px">loading…</div>';
  $('#recspans').innerHTML='';
  try{
    const d=await (await fetch('/api/recordings?ch='+ch)).json();
    if(d.error){list.innerHTML='<div style="color:var(--err);padding:20px">'+d.error+'</div>';return;}
    const segs=d.segments||[];
    $('#recsub').textContent=segs.length+' segment(s) · '+d.files.length+' files on SATA';
    if(segs.length){
      const t0=segs[0].startTs,t1=Math.max(...segs.map(s=>s.endTs)),span=Math.max(1,t1-t0);
      $('#recspans').innerHTML='<div class="card" style="padding:12px 14px"><div style="font-size:11px;letter-spacing:.08em;text-transform:uppercase;color:var(--dim-2);margin-bottom:8px">Timeline · '+tstr(t0)+' → '+tclock(t1)+'</div>'+
        '<div class="timeline">'+segs.map((s,i)=>{const L=(s.startTs-t0)/span*100,W=Math.max(0.6,(s.endTs-s.startTs)/span*100);
          return '<span class="seg" style="left:'+L.toFixed(2)+'%;width:'+W.toFixed(2)+'%" title="'+tstr(s.startTs)+' · '+durstr(s.dur)+'" onclick="playSeg('+ch+','+i+')"></span>';}).join('')+'</div></div>';
    }
    CLIPSEGS=segs;
    list.innerHTML=segs.map((s,i)=>
      '<div class="clip"><div class="thumb ns" onclick="playSeg('+ch+','+i+')">'+
        '<span class="osd">'+tclock(s.startTs)+'</span><span class="ch">CH 0'+(ch+1)+'</span>'+
        '<span class="dur">'+durstr(s.dur)+'</span>'+
        '<div class="ov"><svg viewBox="0 0 24 24"><path d="M8 5v14l11-7z"/></svg></div></div>'+
        '<div class="info"><div class="txt"><div class="t">'+tstr(s.startTs)+'</div><div class="s">'+durstr(s.dur)+' · '+fmt(s.bytes)+' · '+s.file+'</div></div>'+
        '<span class="sp"></span><button class="rowbtn" onclick="location.href=\'/api/download?path=/root/rec/a'+(ch+1)+'/'+s.file+'\'">↓ raw</button></div></div>'
    ).join('')||'<div style="color:var(--dim);padding:20px">no recorded segments</div>';
  }catch(e){list.innerHTML='<div style="color:var(--err);padding:20px">'+e.message+'</div>';}
}
let CLIPSEGS=[], CUR=null, seeking=false;
function hms(s){s=Math.max(0,Math.round(s));return pad(Math.floor(s/60))+':'+pad(s%60);}
function playSeg(ch,i){
  const s=CLIPSEGS[i];if(!s)return;
  CUR={ch,seg:s,loadT:0};
  $('#pch').textContent='CH '+(ch+1);$('#pname').textContent=tstr(s.startTs)+' · '+durstr(s.dur);
  $('#scrim').classList.add('show');
  const seek=$('#pseek');seek.max=Math.max(1,s.dur);seek.value=0;$('#ptime').textContent='00:00 / '+hms(s.dur);
  loadAt(0,true);
}
// (re)load the clip starting `t` seconds into the segment; the backend seeks server-side.
function loadAt(t,autoplay){
  if(!CUR)return;
  CUR.loadT=Math.max(0,Math.floor(t));
  const v=$('#pvideo');
  $('#ploading').style.display='grid';$('#ploading').textContent='preparing clip…';
  v.src='/api/clip?ch='+CUR.ch+'&start='+CUR.seg.startOff+'&end='+CUR.seg.endOff+(CUR.loadT>0?'&t='+CUR.loadT:'');
  v.load();
  v.oncanplay=()=>{$('#ploading').style.display='none';if(autoplay)v.play().catch(()=>{});};
  v.onerror=()=>{$('#ploading').style.display='grid';$('#ploading').textContent='could not prepare this clip';};
}
// full-segment scrubber: seek within the loaded window instantly, else reload at that time
$('#pseek').addEventListener('input',()=>{seeking=true;const t=+$('#pseek').value;$('#ptime').textContent=hms(t)+' / '+hms(CUR?CUR.seg.dur:0);});
$('#pseek').addEventListener('change',()=>{
  seeking=false;if(!CUR)return;const t=+$('#pseek').value;const v=$('#pvideo');const dur=v.duration||0;
  if(t>=CUR.loadT&&t<=CUR.loadT+dur)v.currentTime=t-CUR.loadT; else loadAt(t,true);
});
$('#pvideo').addEventListener('timeupdate',()=>{
  if(seeking||!CUR)return;const g=CUR.loadT+($('#pvideo').currentTime||0);
  $('#pseek').value=g;$('#ptime').textContent=hms(g)+' / '+hms(CUR.seg.dur);
});
$('#pclose').onclick=()=>{$('#scrim').classList.remove('show');const v=$('#pvideo');v.pause();v.src='';CUR=null;};
$('#scrim').onclick=e=>{if(e.target===$('#scrim'))$('#pclose').onclick();};
window.playSeg=playSeg;window.loadRecordings=loadRecordings;
