'use strict';
const $=(s,e=document)=>e.querySelector(s),$$=(s,e=document)=>[...e.querySelectorAll(s)];
const pad=n=>String(n).padStart(2,'0');
const api=(p,o)=>fetch(p,o).then(r=>r.json().catch(()=>({})));
const post=(p)=>api(p,{method:'POST'});

/* theme + nav */
$('#theme').onclick=()=>{const r=document.documentElement;const cur=r.getAttribute('data-theme')||(matchMedia('(prefers-color-scheme: dark)').matches?'dark':'light');r.setAttribute('data-theme',cur==='dark'?'light':'dark');};
$$('.navbtn').forEach(b=>b.onclick=()=>{$$('.navbtn').forEach(x=>x.classList.remove('active'));b.classList.add('active');$$('.view').forEach(v=>v.classList.remove('active'));$('#v-'+b.dataset.view).classList.add('active');
  const v=b.dataset.view;if(v==='live')fit();else if(v==='files')initFiles();else if(v==='term')initShell();else if(v==='rec')initClips();else if(v==='mon')initMonitor();else if(v==='set')initConfig();});

let NCH=4, ENC=1, INFO=null;
const CH=[]; const wall=$('#wall'), stage=$('#stage');
const isEncoded=i=>i<ENC;

/* ---- build camera cells (with real <video>) ---- */
function buildCells(){
  wall.innerHTML='';CH.length=0;
  for(let i=0;i<NCH;i++){
    const c={n:i+1,idx:i,ws:null,jmuxer:null,frames:0,lastFrames:0,rec:false,enc:isEncoded(i)};
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

/* ---- streaming: one WS + jMuxer per visible, ENCODED channel ----
 * The firmware encodes only `enc_channels` channels (default 1: all the bandwidth goes to
 * the FPV camera). Opening a socket for a non-encoded channel would wait forever for a
 * keyframe and hold one of the device's 12 client slots, so we don't — the cell says so. */
function startStream(c){
  if(c.ws)return;
  if(!c.enc){setState(c,'noenc');return;}
  const proto=location.protocol==='https:'?'wss':'ws';
  const jm=new JMuxer({node:c.video,mode:'video',flushingTime:0,fps:(INFO&&INFO.fps)||30,clearBuffer:true,debug:false});
  const ws=new WebSocket(`${proto}://${location.host}/stream?ch=${c.idx}`);
  ws.binaryType='arraybuffer';
  ws.onmessage=ev=>{c.frames++;jm.feed({video:new Uint8Array(ev.data)});setState(c,'live');};
  ws.onclose=ev=>setState(c,ev&&ev.code===4404?'noenc':'off');
  ws.onerror=()=>setState(c,'off');
  c.ws=ws;c.jmuxer=jm;setState(c,'conn');
}
function stopStream(c){
  if(c.ws){try{c.ws.close();}catch(e){}c.ws=null;}
  if(c.jmuxer){try{c.jmuxer.destroy();}catch(e){}c.jmuxer=null;}
  setState(c,c.enc?'off':'noenc');
}
function setState(c,s){
  const dot=c.el.querySelector('[data-dot]'),st=c.el.querySelector('[data-st]');
  dot.className='dot'+(s==='live'?' live':'');
  st.textContent=s==='live'?'LIVE':s==='conn'?'…':s==='noenc'?'NOT ENCODED':'OFF';
  c.el.classList.toggle('online',s==='live');
  c.el.style.opacity=(s==='noenc')?'.55':'';
  if(s==='noenc')c.el.querySelector('[data-metrics]').textContent='enc_channels='+ENC;
}
function snapshot(c){
  if(!c.video.videoWidth)return;
  const cv=document.createElement('canvas');cv.width=c.video.videoWidth;cv.height=c.video.videoHeight;
  cv.getContext('2d').drawImage(c.video,0,0);
  const a=document.createElement('a');a.download=`CH${c.n}_snap.png`;a.href=cv.toDataURL('image/png');a.click();
}
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
  try{const j=await post(`/api/record?ch=${c.idx}&on=${on?1:0}`);if(j.info)applyInfo(j.info);}
  catch(e){pollInfo();}
}
function fmtDur(s){s=Math.max(0,s|0);const h=(s/3600)|0,m=((s%3600)/60)|0;return h?`${h}h ${pad(m)}m`:`${m}m ${pad(s%60)}s`;}
function fmtMb(mb){return mb>=1024?(mb/1024).toFixed(1)+' GB':mb+' MB';}

/* single source of truth: everything the header/live/monitor tabs show comes from INFO */
function applyInfo(i){
  INFO=i;
  const newEnc=i.encChannels||1;
  if(newEnc!==ENC){ENC=newEnc;CH.forEach((c,k)=>{c.enc=isEncoded(k);});syncStreams();}
  setConn(true);
  let n=0;
  CH.forEach((c,k)=>{const on=!!i.rec[k];c.rec=on;
    c.el.querySelector('.rb').classList.toggle('rec-on',on);
    c.el.querySelector('.tr').innerHTML=on
      ?`<span class="chip state" style="color:#ffd2d2"><span class="dot rec"></span>REC ${fmtDur(i.recSecs[k]||0)} · ${fmtMb(i.recMb[k]||0)}</span>`:'';
    if(on)n++;});
  $('#recpill').style.display=n?'inline-flex':'none';$('#recn').textContent=n;
  $('#pbpill').style.display=i.playing?'inline-flex':'none';
  $('#diskfree').textContent=fmtMb(i.diskFreeMb);
  $('#dvrclock').textContent=i.time?i.time.slice(11):'--:--:--';
  $('#encpill').textContent=`${i.width}×${i.height} ${i.fps}fps GOP${i.gop} ${i.rcMode===3?'QP'+i.qp:i.bitrate+'k'} · ${i.std} · ${i.encChannels}ch`;
  if($('#v-mon').classList.contains('active'))renderMonitor(i);
  if($('#v-set').classList.contains('active'))renderDevInfo(i);
}
async function pollInfo(){
  try{const r=await fetch('/api/info');if(!r.ok){setConn(false);return;}applyInfo(await r.json());}
  catch(e){setConn(false);}
}
function setConn(ok){const d=$('#conndot'),l=$('#connlbl');d.className='dot'+(ok?' live':'');l.textContent=ok?'Online':'DVR offline';}

/* fps + live-edge snap */
setInterval(()=>{
  CH.forEach(c=>{
    if(c.el.style.display==='none'||!c.enc)return;
    const f=c.frames-c.lastFrames;c.lastFrames=c.frames;
    const m=c.el.querySelector('[data-metrics]');
    m.textContent=f?`${Math.round(f/2)} fps`:'—';
    if(c.video.buffered.length){const end=c.video.buffered.end(c.video.buffered.length-1);if(end-c.video.currentTime>0.6)c.video.currentTime=end-0.05;}
  });
},2000);
setInterval(pollInfo,2000);

/* ---- boot ---- */
fetch('/api/config').then(r=>r.json()).then(cfg=>{NCH=cfg.channels||4;ENC=cfg.encChannels||1;boot();}).catch(()=>boot());
function boot(){buildCells();setLayout('2x2');pollInfo();setTimeout(fit,120);}

/* ================= FILE MANAGER ================= */
let curPath='/root/rec/a1';
function initFiles(){ loadDir(curPath); }
function fmt(n){ if(!n)return''; const u=['B','KB','MB','GB'];let i=0,v=n;while(v>=1024&&i<3){v/=1024;i++;}return (i?v.toFixed(1):v)+' '+u[i]; }
async function loadDir(p){
  const tb=$('#filetable');tb.innerHTML='<tr><td colspan="4" style="color:var(--dim)">loading…</td></tr>';
  try{
    const d=await api('/api/files?path='+encodeURIComponent(p));
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
async function delFile(f){ if(!confirm('Delete '+f+' ?'))return; await api('/api/fs',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({action:'delete',path:f})}); loadDir(curPath); }
$('#frefresh').onclick=()=>loadDir(curPath);
$('#fmkdir').onclick=async()=>{const n=prompt('New folder name:');if(!n)return;await api('/api/fs',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({action:'mkdir',path:curPath+'/'+n})});loadDir(curPath);};
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

/* ================= RECORDINGS =================
 * Inventory now comes from the firmware's LIST command (no telnet round-trip), so each
 * entry carries its real device path — play in the browser (TS->MP4 remux) or on the
 * DVR's own monitor (VDEC->VO), download, or delete. */
let clipsInit=false;
function initClips(){
  if(!clipsInit){ clipsInit=true; $('#recch').onchange=loadRecordings; $('#recrefresh').onclick=loadRecordings;
    const ps=document.querySelector('.pscrub'); if(ps)ps.style.display='none'; }
  loadRecordings();
}
async function loadRecordings(){
  const ch=$('#recch').value;
  const list=$('#reclist');list.innerHTML='<div style="color:var(--dim);padding:20px">loading…</div>';
  $('#recspans').innerHTML='';
  try{
    const d=await api('/api/recordings?ch='+encodeURIComponent(ch));
    if(d.error){list.innerHTML='<div style="color:var(--err);padding:20px">'+d.error+'</div>';return;}
    const recs=d.recordings||[];
    const total=recs.reduce((a,r)=>a+r.size,0);
    $('#recsub').textContent=`${recs.length} recording(s) · ${fmt(total)} on SATA`;
    list.innerHTML=recs.map(r=>{
      const q='path='+encodeURIComponent(r.path);
      return '<div class="clip"><div class="thumb" style="background:#000 center/cover no-repeat url(/api/rec/thumb?'+q+')" onclick="playRec(\''+r.path+'\','+r.ch+')">'+
        '<span class="osd">'+r.time+'</span><span class="ch">CH 0'+(r.ch+1)+'</span>'+
        '<div class="ov"><svg viewBox="0 0 24 24"><path d="M8 5v14l11-7z"/></svg></div></div>'+
        '<div class="info"><div class="txt"><div class="t">'+r.date+' '+r.time+'</div><div class="s">'+fmt(r.size)+' · '+r.file+'</div></div>'+
        '<span class="sp"></span>'+
        '<button class="rowbtn" title="play on the DVR\'s VGA monitor" onclick="playOnMonitor(\''+r.path+'\')">▶ monitor</button> '+
        '<button class="rowbtn" onclick="location.href=\'/api/rec/mp4?'+q+'&dl=1\'">↓ MP4</button> '+
        '<button class="rowbtn" onclick="delRec(\''+r.path+'\')">Delete</button></div></div>';
    }).join('')||'<div style="color:var(--dim);padding:20px">no recordings — start one from the Live tab</div>';
  }catch(e){list.innerHTML='<div style="color:var(--err);padding:20px">'+e.message+'</div>';}
}
function playRec(devPath,ch){
  $('#pch').textContent='CH '+(ch+1);$('#pname').textContent=devPath.split('/').pop();
  $('#scrim').classList.add('show');
  const v=$('#pvideo');
  $('#ploading').style.display='grid';$('#ploading').textContent='preparing MP4…';
  v.src='/api/rec/mp4?path='+encodeURIComponent(devPath);
  v.load();
  v.oncanplay=()=>{$('#ploading').style.display='none';v.play().catch(()=>{});};
  v.onerror=()=>{$('#ploading').style.display='grid';$('#ploading').textContent='could not prepare this recording';};
}
async function playOnMonitor(devPath){
  await post('/api/playback?op=open&path='+encodeURIComponent(devPath));
  $$('.navbtn').forEach(b=>{if(b.dataset.view==='mon')b.click();});
}
async function delRec(devPath){
  if(!confirm('Delete '+devPath.split('/').pop()+' from the DVR?'))return;
  const j=await post('/api/rec/delete?path='+encodeURIComponent(devPath));
  if(j.error)alert('Delete failed: '+j.error);
  loadRecordings();
}
$('#pclose').onclick=()=>{$('#scrim').classList.remove('show');const v=$('#pvideo');v.pause();v.src='';};
$('#scrim').onclick=e=>{if(e.target===$('#scrim'))$('#pclose').onclick();};
window.playRec=playRec;window.loadRecordings=loadRecordings;window.playOnMonitor=playOnMonitor;window.delRec=delRec;

/* ================= MONITOR (the DVR's own VGA output) ================= */
const PICROWS=[['bright','Brightness'],['contrast','Contrast'],['sat','Saturation'],['hue','Hue']];
let monInit=false;
function initMonitor(){
  if(!monInit){
    monInit=true;
    $('#picrows').innerHTML=PICROWS.map(([k,label])=>
      `<div style="display:flex;align-items:center;gap:10px;margin:6px 0">
         <span style="width:96px;font-size:13px">${label}</span>
         <input type="range" min="0" max="255" step="1" data-pic="${k}" style="flex:1">
         <span class="mono" data-picv="${k}" style="width:34px;text-align:right;font-size:12px;color:var(--dim)">—</span>
       </div>`).join('');
    $$('[data-pic]').forEach(r=>{
      r.oninput=()=>{$(`[data-picv="${r.dataset.pic}"]`).textContent=r.value;};
      r.onchange=async()=>{applyPic(await post(`/api/picture?k=${r.dataset.pic.toUpperCase()}&v=${r.value}`));};
    });
    $('#picreset').onclick=async()=>{applyPic(await post('/api/picture?k=RESET'));};
    $$('#monchseg button').forEach(b=>b.onclick=async()=>{
      /* Don't light the button until the device agrees. Switching re-latches VI->VO by
         respawning, so the firmware refuses it while recording — marking it selected
         first would leave the UI claiming a channel the DVR never moved to. */
      $('#osdstate').textContent='switching channel — the DVR restarts its pipeline…';
      const j=await post('/api/display?ch='+b.dataset.ch);
      if(j&&j.ok){
        $$('#monchseg button').forEach(x=>x.classList.remove('on'));b.classList.add('on');
        $('#osdstate').textContent='channel '+b.dataset.ch+' — pipeline restarting…';
      }else{
        $('#osdstate').textContent=(j&&j.reply==='ERR recording')
          ? 'refused — stop recording first (switching restarts the pipeline)'
          : 'channel switch failed'+((j&&j.reply)?': '+j.reply:'');
      }
      pollInfo();
    });
    $$('#osdshowseg button').forEach(b=>b.onclick=async()=>{
      $$('#osdshowseg button').forEach(x=>x.classList.remove('on'));b.classList.add('on');
      await post('/api/osd?op=show&v='+b.dataset.v);
    });
    $$('[data-osd]').forEach(b=>b.onclick=async()=>{
      const j=await post('/api/osd?op=key&k='+b.dataset.osd);
      $('#osdstate').textContent=j.reply||'—';pollInfo();
    });
    $$('[data-pb]').forEach(b=>b.onclick=()=>post('/api/playback?op='+b.dataset.pb).then(pollInfo));
    $('#pbseek').onchange=()=>post('/api/playback?op=seek&pm='+$('#pbseek').value);
    $$('#pbspeed button').forEach(b=>b.onclick=()=>{
      $$('#pbspeed button').forEach(x=>x.classList.remove('on'));b.classList.add('on');
      post('/api/playback?op=speed&v='+b.dataset.v);});
    $('#monrefresh').onclick=()=>{pollInfo();loadPic();};
    loadPic();
  }
  if(INFO)renderMonitor(INFO);
}
function renderMonitor(i){
  $$('#monchseg button').forEach(b=>b.classList.toggle('on',+b.dataset.ch===i.displayChannel));
  $$('#osdshowseg button').forEach(b=>{if(!$$('#osdshowseg button').some(x=>x.classList.contains('on')))b.classList.toggle('on',b.dataset.v==='1');});
  const st=$('#osdstate');
  if(!st.textContent.startsWith('switching'))
    st.textContent=(i.playing?'playing on monitor':'live')+' · menu '+(i.osdOpen?'open':'closed')+' · CH '+(i.displayChannel+1);
}
function applyPic(p){
  if(!p||p.error)return;
  PICROWS.forEach(([k])=>{const r=$(`[data-pic="${k}"]`);if(r&&p[k]!==undefined){r.value=p[k];$(`[data-picv="${k}"]`).textContent=p[k];}});
}
async function loadPic(){ try{applyPic(await api('/api/picture'));}catch(e){} }

/* ================= CONFIG ================= */
let confInit=false;
function initConfig(){
  if(!confInit){
    confInit=true;
    $('#confreload').onclick=loadConf;
    $('#confsave').onclick=()=>saveConf(false);
    $('#confrestart').onclick=()=>saveConf(true);
    loadConf();
  }
  if(INFO)renderDevInfo(INFO);
}
function renderDevInfo(i){
  const rc=i.rcMode===3?`FIXQP (qp ${i.qp})`:`VBR cap ${i.bitrate} kbps`;
  const rows=[
    ['Firmware','control protocol v'+i.ver],
    ['Uptime',fmtDur(i.uptime)],
    ['Device clock',i.time||'—'],
    ['Standard',i.std],
    ['Encoder',`${i.width}×${i.height} @ ${i.fps} fps · GOP ${i.gop} · ${rc}`],
    ['Encoded channels',i.encChannels+' of '+NCH],
    ['VGA showing','CH '+(i.displayChannel+1)],
    ['Live viewers',i.clients],
    ['Disk free',fmtMb(i.diskFreeMb)],
    ['Encoder packs',i.packs.toLocaleString()],
  ];
  $('#devinfo').innerHTML=rows.map(([k,v])=>
    `<tr><td style="color:var(--dim);padding:3px 10px 3px 0;white-space:nowrap">${k}</td><td class="mono" style="padding:3px 0">${v}</td></tr>`).join('');
  $('#setclock').textContent=i.time||'—';
  $$('#stdseg button').forEach(b=>b.classList.toggle('on',b.dataset.std===i.std));
  $$('#sndseg button').forEach(b=>b.classList.toggle('on',(b.dataset.snd==='1')===!!i.sound));
}
async function loadConf(){
  $('#confstat').textContent='loading…';
  const j=await api('/api/conf');
  if(j.error){$('#confstat').textContent=j.error;return;}
  $('#conftext').value=j.text||'';
  $('#confstat').textContent=j.text?'loaded from the device':'no dvr.conf on the device yet — built-in defaults are in use';
}
async function saveConf(restart){
  $('#confstat').textContent='saving…';
  const r=await fetch('/api/conf',{method:'POST',headers:{'Content-Type':'text/plain'},body:$('#conftext').value});
  const j=await r.json().catch(()=>({}));
  if(!r.ok||j.error){$('#confstat').textContent='save failed: '+(j.error||r.status);return;}
  if(!restart){$('#confstat').textContent='saved — restart the DVR to apply';return;}
  $('#confstat').textContent='saved, restarting the DVR…';
  await post('/api/restart');
  setTimeout(()=>{pollInfo();$('#confstat').textContent='restarted';},9000);
}
if($('#btnsync'))$('#btnsync').onclick=async()=>{
  $('#setclock').textContent='syncing…';
  const j=await post('/api/time');
  $('#setclock').textContent=j.ok?'synced':'sync failed';
  setTimeout(pollInfo,500);
};
if($('#sndseg'))$$('#sndseg button').forEach(b=>b.onclick=async()=>{
  $$('#sndseg button').forEach(x=>x.classList.remove('on'));b.classList.add('on');
  await post('/api/sound?on='+b.dataset.snd);
  setTimeout(pollInfo,400);
});
if($('#stdseg'))$$('#stdseg button').forEach(b=>b.onclick=async()=>{
  if(!confirm('Switch to '+b.dataset.std+'? The DVR restarts its capture pipeline.'))return;
  const j=await post('/api/std?v='+b.dataset.std);
  /* same as the channel switcher: the firmware refuses this while recording, so only
     show it as selected once it actually took */
  if(j&&j.ok){ $$('#stdseg button').forEach(x=>x.classList.remove('on'));b.classList.add('on'); }
  else if(j&&j.reply==='ERR recording') alert('Stop recording first — changing the video standard restarts the capture pipeline.');
  else alert('Standard change failed'+((j&&j.reply)?': '+j.reply:''));
  pollInfo();
});
