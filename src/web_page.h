#pragma once
#include <pgmspace.h>

// Settings page served at http://192.168.4.1 (setup) or http://clock.local.
// Static: every value comes from /api/state, so nothing user-supplied is ever
// spliced into this HTML, and the stored Wi-Fi password is never sent back.
// Text is in Traditional Chinese and English; the choice is kept in
// localStorage and defaults to the browser language.
static const char WEB_PAGE[] PROGMEM = R"rawliteral(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Clock</title>
<style>
:root{color-scheme:dark;--bg:#000;--card:#1c1c1e;--line:#3a3a3c;--text:#f4efe6;--dim:#8e8e93;--accent:#ff9f0a;--ok:#30d158;--warn:#ff453a}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);font:16px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI","PingFang TC","Noto Sans TC",sans-serif}
main{max-width:520px;margin:0 auto;padding:16px}
header{display:flex;align-items:center;justify-content:space-between;gap:12px}
h1{font-size:22px;margin:6px 0 14px}
h2{font-size:14px;color:var(--dim);margin:0 0 6px;font-weight:600}
section{background:var(--card);border-radius:14px;padding:14px 16px;margin-bottom:14px}
label{display:block;margin:10px 0 4px;color:var(--dim);font-size:14px}
input,select{width:100%;padding:11px 12px;border-radius:10px;border:1px solid var(--line);background:#000;color:var(--text);font-size:16px}
input+input{margin-top:8px}
.row{display:flex;gap:10px}.row>div{flex:1;min-width:0}
.check{display:flex;align-items:center;gap:10px;margin:12px 0 4px}
.check input{width:22px;height:22px;flex:none}.check label{margin:0;color:var(--text);font-size:16px}
button{width:100%;padding:13px;border:0;border-radius:12px;background:var(--accent);color:#000;font-size:17px;font-weight:700}
button:disabled{opacity:.5}
.lang{width:auto;padding:6px 14px;font-size:14px;background:var(--card);color:var(--text);border:1px solid var(--line)}
#msg{margin:12px 0;min-height:1.5em}
.ok{color:var(--ok)}.err{color:var(--warn)}
.kv{display:grid;grid-template-columns:auto 1fr;gap:2px 14px;font-size:14px}
.kv span:nth-child(odd){color:var(--dim)}
small{color:var(--dim);display:block;margin-top:6px}
</style></head><body><main>
<header><h1 data-i18n="title"></h1><button type="button" class="lang" id="lang"></button></header>
<section><h2 data-i18n="status"></h2><div class="kv" id="st"></div></section>
<form id="f" autocomplete="off">
<section><h2 data-i18n="wifi"></h2>
<label for="ssid" data-i18n="ssid"></label>
<input id="ssid" list="nets" maxlength="32" required>
<datalist id="nets"></datalist>
<small id="scan"></small>
<label for="pass" data-i18n="password"></label>
<input id="pass" type="password" maxlength="64">
<small id="passhint"></small>
</section>
<section><h2 data-i18n="time"></h2>
<label for="tz" data-i18n="tz"></label>
<select id="tz">
<option value="CST-8" data-i18n="tz_taipei"></option>
<option value="HKT-8" data-i18n="tz_hk"></option>
<option value="&lt;+08&gt;-8" data-i18n="tz_sg"></option>
<option value="JST-9" data-i18n="tz_tokyo"></option>
<option value="KST-9" data-i18n="tz_seoul"></option>
<option value="GMT0BST,M3.5.0/1,M10.5.0" data-i18n="tz_london"></option>
<option value="CET-1CEST,M3.5.0,M10.5.0/3" data-i18n="tz_paris"></option>
<option value="EST5EDT,M3.2.0,M11.1.0" data-i18n="tz_ny"></option>
<option value="PST8PDT,M3.2.0,M11.1.0" data-i18n="tz_la"></option>
<option value="UTC0" data-i18n="tz_utc"></option>
<option value="custom" data-i18n="tz_custom"></option>
</select>
<input id="tzc" maxlength="63" style="display:none;margin-top:8px">
<div class="check"><input type="checkbox" id="h12"><label for="h12" data-i18n="h12"></label></div>
<div class="check"><input type="checkbox" id="date"><label for="date" data-i18n="showDate"></label></div>
<label for="ntp1" data-i18n="ntp"></label>
<input id="ntp1" maxlength="63" autocapitalize="off" spellcheck="false">
<input id="ntp2" maxlength="63" autocapitalize="off" spellcheck="false">
<input id="ntp3" maxlength="63" autocapitalize="off" spellcheck="false">
<small data-i18n="ntpHint"></small>
</section>
<section><h2 data-i18n="bright"></h2>
<div class="row">
<div><label for="lday" data-i18n="lday"></label><select id="lday"></select></div>
<div><label for="lnight" data-i18n="lnight"></label><select id="lnight"></select></div>
</div>
<div class="check"><input type="checkbox" id="night"><label for="night" data-i18n="night"></label></div>
<div class="row">
<div><label for="ns" data-i18n="start"></label><input type="time" id="ns" required></div>
<div><label for="ne" data-i18n="end"></label><input type="time" id="ne" required></div>
</div>
</section>
<section><h2 data-i18n="sleepT"></h2>
<div class="check"><input type="checkbox" id="sleep"><label for="sleep" data-i18n="sleep"></label></div>
<div class="row">
<div><label for="ss" data-i18n="offAt"></label><input type="time" id="ss" required></div>
<div><label for="se" data-i18n="onAt"></label><input type="time" id="se" required></div>
</div>
<small data-i18n="sleepHint"></small>
</section>
<section><h2 data-i18n="burn"></h2>
<div class="check"><input type="checkbox" id="swap"><label for="swap" data-i18n="swap"></label></div>
<small data-i18n="burnHint"></small>
</section>
<section><h2 data-i18n="rotT"></h2>
<select id="rot">
<option value="4" data-i18n="rot_auto"></option>
<option value="0" data-i18n="rot_0"></option>
<option value="1" data-i18n="rot_1"></option>
<option value="2" data-i18n="rot_2"></option>
<option value="3" data-i18n="rot_3"></option>
</select>
</section>
<button id="save" type="submit" data-i18n="save"></button>
<div id="msg"></div>
</form>
<section><h2 data-i18n="keys"></h2><div class="kv" id="keys"></div></section>
</main>
<script>
const T={
zh:{title:'時鐘設定',status:'狀態',time_l:'時間',notSynced:'（未校時）',source:'來源',lastSync:'上次對時',wifi_l:'Wi-Fi',notSet:'未設定',offline:'未連線',ip:'IP',power:'電源',charging:'充電中',fw:'韌體',
wifi:'Wi-Fi（僅支援 2.4 GHz）',ssid:'網路名稱',scanning:'搜尋附近網路中…',scanFound:'找到 {n} 個網路，可從清單選擇。',scanNone:'沒有找到網路，請手動輸入。',scanFail:'無法搜尋網路。',open:'開放',
password:'密碼',passKept:'已儲存密碼；不變更請留空。',time:'時間',tz:'時區',tz_taipei:'台北（UTC+8）',tz_hk:'香港（UTC+8）',tz_sg:'新加坡（UTC+8）',tz_tokyo:'東京（UTC+9）',tz_seoul:'首爾（UTC+9）',tz_london:'倫敦',tz_paris:'巴黎／柏林',tz_ny:'紐約',tz_la:'洛杉磯',tz_utc:'UTC',tz_custom:'自訂（POSIX TZ）',tzcPh:'例如 CST-8',
h12:'12 小時制',showDate:'顯示星期與日期',ntp:'對時伺服器（最多 3 個）',ntpHint:'填主機名稱或 IP，依序使用；三個都留空會恢復預設。',
bright:'亮度與夜間模式',lday:'白天亮度',lnight:'夜間亮度',night:'夜間自動調暗',start:'開始',end:'結束',
sleepT:'定時關閉螢幕',sleep:'在下列時段關閉螢幕',offAt:'關閉',onAt:'開啟',sleepHint:'關閉期間按任一鍵或輕觸螢幕，會亮 30 秒。開始與結束相同時視為停用。',
burn:'防烙印',swap:'星期與秒數每小時左右換邊',burnHint:'另外整個畫面每分鐘移動 1 像素（範圍 -6～+5），一直開啟。',
rotT:'螢幕方向',rot_auto:'自動（重力感應）',rot_0:'USB 朝下',rot_1:'USB 朝左',rot_2:'USB 朝上',rot_3:'USB 朝右',
save:'儲存',saving:'儲存中…',saved:'已儲存。',savedWifi:'已儲存，正在連線 Wi-Fi…',connected:'已連上 {ssid}，時鐘會自動對時。',openLater:'之後可用 http://clock.local 或 http://{ip} 開啟本頁。',
failed:'連線失敗：{reason}，請檢查後重新儲存。',unknown:'無法確認連線結果，請看時鐘螢幕；失敗時長按 BOOT 3 秒重新設定。',lost:'連線中斷，請重新整理頁面。',
'WRONG PASSWORD':'密碼錯誤','NETWORK NOT FOUND':'找不到這個網路（需 2.4 GHz）','SIGNAL LOST':'訊號中斷','TIMEOUT':'連線逾時','CONNECT FAILED':'無法連線',
e_ssid:'Wi-Fi 名稱需 1–32 個字元',e_pass:'Wi-Fi 密碼需 8–63 個字元（開放網路請留空）',e_tz:'時區格式不正確',e_night:'夜間時段格式不正確',e_sleep:'關閉螢幕時段格式不正確',e_level:'亮度超出範圍',e_rot:'螢幕方向不正確',e_ntp:'對時伺服器格式不正確（只能有英數字、點、連字號，最長 63 字元）',e_nvs:'儲存失敗（快閃記憶體寫入錯誤）',
keys:'按鍵',k_key:'亮度 ＋（長按：切換螢幕方向）',k_boot:'亮度 －（長按 3 秒：Wi-Fi 設定）',k_pwr:'螢幕開／關（長按 6 秒：關機）',k_tapL:'輕觸螢幕',k_tap:'顯示／關閉狀態頁',k_offL:'定時關閉中',k_off:'按任一鍵或輕觸：亮 30 秒',langBtn:'English'},
en:{title:'Clock settings',status:'Status',time_l:'Time',notSynced:' (not synced)',source:'Source',lastSync:'last sync',wifi_l:'Wi-Fi',notSet:'not set',offline:'offline',ip:'IP',power:'Power',charging:'charging',fw:'Firmware',
wifi:'Wi-Fi (2.4 GHz only)',ssid:'Network name',scanning:'Scanning for networks…',scanFound:'Found {n} networks; pick one from the list.',scanNone:'No networks found; type the name.',scanFail:'Network scan failed.',open:'open',
password:'Password',passKept:'A password is saved; leave empty to keep it.',time:'Time',tz:'Time zone',tz_taipei:'Taipei (UTC+8)',tz_hk:'Hong Kong (UTC+8)',tz_sg:'Singapore (UTC+8)',tz_tokyo:'Tokyo (UTC+9)',tz_seoul:'Seoul (UTC+9)',tz_london:'London',tz_paris:'Paris / Berlin',tz_ny:'New York',tz_la:'Los Angeles',tz_utc:'UTC',tz_custom:'Custom (POSIX TZ)',tzcPh:'e.g. CST-8',
h12:'12-hour clock',showDate:'Show weekday and date',ntp:'Time servers (up to 3)',ntpHint:'Host names or IPs, used in order; leave all three empty to restore the defaults.',
bright:'Brightness and night mode',lday:'Day brightness',lnight:'Night brightness',night:'Dim at night',start:'Start',end:'End',
sleepT:'Screen-off schedule',sleep:'Turn the screen off between',offAt:'Off at',onAt:'On at',sleepHint:'Meanwhile a button press or tap shows the clock for 30 s. Same start and end disables it.',
burn:'Burn-in protection',swap:'Move weekday and seconds to the other side every hour',burnHint:'The whole face also moves 1 px every minute (range -6 to +5); always on.',
rotT:'Screen orientation',rot_auto:'Automatic (accelerometer)',rot_0:'USB port down',rot_1:'USB port left',rot_2:'USB port up',rot_3:'USB port right',
save:'Save',saving:'Saving…',saved:'Saved.',savedWifi:'Saved; connecting to Wi-Fi…',connected:'Connected to {ssid}; the clock will set its time.',openLater:'Later, open this page at http://clock.local or http://{ip}.',
failed:'Connection failed: {reason}. Check and save again.',unknown:'Could not confirm the result; check the clock. If it failed, hold BOOT for 3 s to set up again.',lost:'Connection lost; reload the page.',
'WRONG PASSWORD':'wrong password','NETWORK NOT FOUND':'network not found (2.4 GHz needed)','SIGNAL LOST':'signal lost','TIMEOUT':'timed out','CONNECT FAILED':'could not connect',
e_ssid:'The Wi-Fi name must be 1–32 characters.',e_pass:'The Wi-Fi password must be 8–63 characters (empty for open networks).',e_tz:'Invalid time zone.',e_night:'Invalid night hours.',e_sleep:'Invalid screen-off hours.',e_level:'Brightness out of range.',e_rot:'Invalid orientation.',e_ntp:'Invalid time server (letters, digits, dots and hyphens; up to 63 characters).',e_nvs:'Saving failed (flash write error).',
keys:'Buttons',k_key:'Brightness + (hold: change orientation)',k_boot:'Brightness − (hold 3 s: Wi-Fi setup)',k_pwr:'Screen on/off (hold 6 s: power off)',k_tapL:'Tap the screen',k_tap:'Show or hide the status page',k_offL:'During screen-off hours',k_off:'Any button or tap: on for 30 s',langBtn:'中文'}};
const $=id=>document.getElementById(id);
let lang='en',cur=null,last=null,polling=false,scanState=null;
try{lang=localStorage.getItem('lang')||''}catch(e){lang=''}
if(lang!=='zh'&&lang!=='en')lang=(navigator.language||'').toLowerCase().startsWith('zh')?'zh':'en';
function t(k,v){let s=(T[lang]&&T[lang][k])||T.en[k]||k;if(v)for(const n in v)s=s.split('{'+n+'}').join(v[n]);return s}
function esc(s){return String(s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]))}
function hm(m){return String(Math.floor(m/60)).padStart(2,'0')+':'+String(m%60).padStart(2,'0')}
function applyLang(){
document.documentElement.lang=lang==='zh'?'zh-Hant':'en';
document.title=t('title');
document.querySelectorAll('[data-i18n]').forEach(e=>{e.textContent=t(e.dataset.i18n)});
$('lang').textContent=t('langBtn');$('tzc').placeholder=t('tzcPh');
['ntp1','ntp2','ntp3'].forEach((id,i)=>{$(id).placeholder=['tock.stdtime.gov.tw','time.stdtime.gov.tw','pool.ntp.org'][i]});
$('keys').innerHTML=[['KEY',t('k_key')],['BOOT',t('k_boot')],['PWR',t('k_pwr')],[t('k_tapL'),t('k_tap')],[t('k_offL'),t('k_off')]].map(r=>'<span>'+esc(r[0])+'</span><span>'+esc(r[1])+'</span>').join('');
if(last)status(last);
if(cur)$('passhint').textContent=cur.has_pass?t('passKept'):'';
showScan();
}
$('lang').onclick=()=>{lang=lang==='zh'?'en':'zh';try{localStorage.setItem('lang',lang)}catch(e){}applyLang()};
for(const id of['lday','lnight'])for(let i=1;i<=8;i++){const o=document.createElement('option');o.value=i;o.textContent=i+' / 8';$(id).appendChild(o)}
$('tz').onchange=()=>{$('tzc').style.display=$('tz').value==='custom'?'block':'none'};
function status(s){
last=s;
const w=s.wifi.connected?s.wifi.ssid+' ('+s.wifi.rssi+' dBm)':(s.wifi.ssid?t('offline')+(s.wifi.err?' · '+t(s.wifi.err):''):t('notSet'));
const rows=[[t('time_l'),s.time+(s.valid?'':t('notSynced'))],[t('source'),s.source+(s.last_sync?', '+t('lastSync')+' '+s.last_sync:'')],
[t('wifi_l'),w],[t('ip'),s.wifi.ip||'-'],[t('power'),s.batt.present?s.batt.pct+'%'+(s.batt.charging?' '+t('charging'):''):'USB'],[t('fw'),s.fw]];
$('st').innerHTML=rows.map(r=>'<span>'+esc(r[0])+'</span><span>'+esc(r[1])+'</span>').join('');
}
function fill(s){
const c=s.cfg;cur=c;
$('ssid').value=c.ssid;$('passhint').textContent=c.has_pass?t('passKept'):'';
const opt=[...$('tz').options].find(o=>o.value===c.tz);
if(opt){$('tz').value=c.tz;$('tzc').style.display='none'}else{$('tz').value='custom';$('tzc').value=c.tz;$('tzc').style.display='block'}
$('h12').checked=c.h12;$('date').checked=c.date;$('night').checked=c.night;
$('ns').value=hm(c.ns);$('ne').value=hm(c.ne);
$('lday').value=c.lday+1;$('lnight').value=c.lnight+1;$('rot').value=c.rot;
$('sleep').checked=c.sleep;$('ss').value=hm(c.ss);$('se').value=hm(c.se);$('swap').checked=c.swap;
['ntp1','ntp2','ntp3'].forEach((id,i)=>{$(id).value=c.ntp[i]||''});
}
async function load(){const r=await fetch('/api/state');const s=await r.json();status(s);if(!cur)fill(s);return s}
function showScan(){
const e=$('scan');
if(!scanState){e.textContent=t('scanning');return}
if(scanState.fail){e.textContent=t('scanFail');return}
e.textContent=scanState.n?t('scanFound',{n:scanState.n}):t('scanNone');
}
async function scan(){
try{const r=await fetch('/api/scan');const j=await r.json();
if(j.scanning){setTimeout(scan,1500);return}
$('nets').innerHTML=j.nets.map(n=>'<option value="'+esc(n.ssid)+'">'+esc(n.rssi+' dBm'+(n.open?' · '+t('open'):''))+'</option>').join('');
scanState={n:j.nets.length};
}catch(e){scanState={fail:true}}
showScan();
}
async function watch(){
if(polling)return;polling=true;
for(let i=0;i<40;i++){
await new Promise(r=>setTimeout(r,1500));
let s;try{s=await load()}catch(e){continue}
const p=s.setup.phase;
if(p==='ok'){$('msg').className='ok';$('msg').textContent=t('connected',{ssid:s.wifi.ssid})+(s.wifi.ip?' '+t('openLater',{ip:s.wifi.ip}):'');break}
if(p==='fail'){$('msg').className='err';$('msg').textContent=t('failed',{reason:t(s.setup.reason||'CONNECT FAILED')});break}
if(p==='off')break;
if(i===39)$('msg').textContent=t('unknown');
}
polling=false;
}
$('f').onsubmit=async e=>{
e.preventDefault();
const tz=$('tz').value==='custom'?$('tzc').value.trim():$('tz').value;
const b=new URLSearchParams({ssid:$('ssid').value,pass:$('pass').value,tz:tz,h12:$('h12').checked?1:0,
date:$('date').checked?1:0,night:$('night').checked?1:0,ns:$('ns').value,ne:$('ne').value,
lday:$('lday').value-1,lnight:$('lnight').value-1,rot:$('rot').value,
sleep:$('sleep').checked?1:0,ss:$('ss').value,se:$('se').value,swap:$('swap').checked?1:0,
ntp1:$('ntp1').value.trim(),ntp2:$('ntp2').value.trim(),ntp3:$('ntp3').value.trim()});
$('save').disabled=true;$('msg').className='';$('msg').textContent=t('saving');
try{
const r=await fetch('/api/settings',{method:'POST',body:b});const j=await r.json();
if(!j.ok){$('msg').className='err';$('msg').textContent=t('e_'+j.error);return}
$('pass').value='';cur=null;
if(j.wifi_changed){$('msg').textContent=t('savedWifi');watch()}
else{$('msg').className='ok';$('msg').textContent=t('saved');load()}
}catch(err){$('msg').className='err';$('msg').textContent=t('lost')}
finally{$('save').disabled=false}
};
applyLang();
load().then(scan).catch(()=>{});
setInterval(()=>{if(!polling)load().catch(()=>{})},5000);
</script></body></html>
)rawliteral";
