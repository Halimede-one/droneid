'use strict';
const $ = id => document.getElementById(id);
const escape = value => String(value ?? '').replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const drone = '<svg class="drone-icon" viewBox="0 0 40 40" aria-hidden="true"><circle cx="10" cy="10" r="6"/><circle cx="30" cy="10" r="6"/><circle cx="10" cy="30" r="6"/><circle cx="30" cy="30" r="6"/><path d="m12 12 16 16m0-16L12 28"/><rect x="16" y="15" width="8" height="10" rx="3"/></svg>';
const receiverIcon = '<svg viewBox="0 0 42 38" aria-hidden="true"><path d="M7 16a17 17 0 0 1 28 0M12 20a11 11 0 0 1 18 0M17 24a5 5 0 0 1 8 0"/><circle cx="21" cy="28" r="2"/><path d="M21 30v5m-7 0h14"/></svg>';
let state = {devices:[]}, selected = localStorage.getItem('droneid.device'), tab = 'nearby';
let token = '', firmware = {}, events, online = false, logRows = [], logCursor = 0, logBusy = false, scrollPaused = false;
const drafts = new Map(), completed = new Set();
const uartDrafts = new Map();
const guardChoices = new Map();
const deviceRoles = new Map();
let guardModels = [], fixture = {adapters:[]};
const pendingMonitors = new Set();
const identifying = new Set();
const uartDefaults = {enabled:false,baud:115200,tx_pin:11,rx_pin:12,system_id:1,component_id:242,fc_component_id:1,target_system:255,target_component:190,max_rate:5};
const uartField = key => $('uart-'+key.replaceAll('_','-'));
const uartSettings = d => Object.fromEntries(Object.entries(uartDefaults).map(([k,v])=>[k,d.status?.uart?.[k]??v]));
for(const id of ['uart-tx-pin','uart-rx-pin']) [1,0,25,7,23,24,11,12,8,9,10].forEach((pin,i)=>{
  const option=document.createElement('option');option.value=pin;option.textContent=`D${i} / GPIO${pin}`;$(id).append(option);
});
let previousDeviceError = null;
const current = () => state.devices.find(d => d.id === selected);
const busy = d => d?.job?.state === 'running';
const isGuard = d => d.device_type==='uart_guard' || d.status?.device_type==='uart_guard';
const chosenRole = d => deviceRoles.get(d.id) ?? (isGuard(d) ? 'guard' : 'scout');
const nameOf = d => isGuard(d) ? `UART guard · ${d.serial.slice(-5)}` : d.status?.broadcast_id || `XIAO · ${d.serial.slice(-5)}`;
const age = seconds => seconds == null ? 'not received' : seconds < 1 ? 'just now' : seconds < 60 ? `${Math.floor(seconds)}s ago` : `${Math.floor(seconds/60)}m ago`;

function helloOf(status) {
  try { return (status?.hello_hex || '').match(/.{2}/g)?.map(x => String.fromCharCode(parseInt(x,16))).join('') || ''; }
  catch { return ''; }
}
function settingsOf(d) {
  return {broadcast_id:d.status?.broadcast_id || '', hello:helloOf(d.status), role:d.status?.role || 'both',
    transport:d.status?.tx_transport === 'ble_legacy' ? 'ble' : 'wifi'};
}
function error(message) { $('error').textContent = message || ''; $('error').hidden = !message; }
async function api(path, body) {
  const response = await fetch(path, body === undefined ? {} : {method:'POST',headers:{'Content-Type':'application/json','X-DroneID-Token':token},body:JSON.stringify(body)});
  const data = await response.json();
  if (!response.ok) throw new Error(data.error || `Request failed (${response.status})`);
  return data;
}
async function refresh() {
  try {
    const data = await api('/api/state'); token = data.token; firmware = data.firmware; guardModels = data.guards || []; fixture = data.guard_fixture || {adapters:[]}; renderGuardOptions();
    online = true; update(data); error(null);
  } catch(e) { online = false; error(e.message); renderConnection(); }
}
function renderConnection() {
  $('server-state').textContent = online ? 'Local USB server connected' : 'Reconnecting to local server';
  $('server-dot').classList.toggle('offline',!online);
}
function update(data) {
  state = data;
  if (!state.devices.some(d => d.id === selected)) selected = state.devices[0]?.id;
  if (selected) localStorage.setItem('droneid.device',selected);
  for (const d of state.devices) {
    if (d.job?.state === 'complete' && !completed.has(d.job.id)) {
      completed.add(d.job.id);
      if (d.job.kind === 'configure') drafts.delete(d.id);
      if (d.job.kind === 'uart') uartDrafts.delete(d.id);
      if (['flash','guard-flash'].includes(d.job.kind)) {deviceRoles.delete(d.id);drafts.delete(d.id);uartDrafts.delete(d.id);}
    }
  }
  render();
}
function renderDevices() {
  $('device-count').textContent = state.devices.filter(d=>d.present).length;
  if (!state.devices.length) { $('devices').innerHTML='<p class="muted">No boards connected.<br>Plug in a USB data cable.</p>'; return; }
  const ids = new Set(state.devices.map(d=>d.id));
  for (const child of [...$('devices').children]) if (!ids.has(child.dataset.device)) child.remove();
  for (const d of state.devices) {
    let button = [...$('devices').children].find(x=>x.dataset.device===d.id);
    if (!button) {
      button=document.createElement('button'); button.className='device-button'; button.dataset.device=d.id;
      button.addEventListener('click',()=>selectDevice(d.id)); $('devices').append(button);
    }
    button.setAttribute('aria-pressed',String(d.id===selected));
    const mode = isGuard(d) ? 'Independent UART guard' : {rx:'Receive only',tx:'Broadcast only',both:'Receive + broadcast'}[d.status?.role] || 'Firmware not detected';
    const status = !d.present ? 'DISCONNECTED' : busy(d) ? `${d.job.kind.toUpperCase()} IN PROGRESS` : d.monitoring ? 'USB CONNECTED' : 'MONITOR PAUSED';
    const content = `<span class="device-mini">▤</span><span><strong>${escape(nameOf(d))}</strong><small>${escape(d.tty)} · ${escape(mode)}</small><em>${escape(status)}</em></span>`;
    if (button.innerHTML !== content) button.innerHTML=content;
  }
}
function selectDevice(id) {
  $('guard-fixture-confirm').checked=false;
  selected=id; if(isGuard(current()))switchTab('settings'); localStorage.setItem('droneid.device',id); logRows=[]; logCursor=0; error(null); render();
  if(tab==='logs') pollLogs();
}
function render() {
  renderConnection(); renderDevices();
  const d=current(); $('no-device').hidden=!!d; $('workspace').hidden=!d;
  if (!d) return;
  $('selected-name').textContent=nameOf(d);
  $('selected-meta').textContent=`${d.tty}  /  ${d.serial}`;
  const input=d.usb_input;
  $('usb-input-health').hidden=!(input?.rejected_lines || input?.reader_failures);
  $('usb-input-health').textContent=input ? `USB input: ${input.rejected_lines} rejected lines · ${input.reader_failures} reader retries. Last issue: ${input.last_error || 'none'}` : '';
  const live=d.present && d.monitoring && d.connection==='monitoring' && !busy(d);
  $('device-state').textContent=!d.present ? 'Disconnected' : busy(d) ? 'Working…' : live ? (d.status?'Monitoring':'USB open · awaiting firmware') : 'Monitor paused';
  $('device-state').classList.toggle('offline',!live);
  $('monitor-toggle').textContent=d.monitoring?'Pause monitor':'Start monitor';
  $('monitor-toggle').disabled=!online||!d.present||busy(d)||pendingMonitors.has(d.id);
  const flashing=d.status?.identifying===true && d.status_age_s<5;
  $('identify').disabled=!online||!d.present||busy(d)||!d.status?.identify_supported||identifying.has(d.id)||flashing;
  $('identify').textContent=identifying.has(d.id)?'Sending…':flashing?'Identifying…':'Identify';
  $('identify').title=isGuard(d)?'Manage this guard in Device settings':d.status?.identify_supported?'Flash this board’s LED for 5 seconds over USB':'Flash the current firmware to enable Identify';
  $('download').href=`/api/devices/${d.id}/download`;
  renderNearby(d,live); renderDeviceRole(d); renderSettings(d); renderUart(d); renderGuards(d); renderJob(d);
  if(d.error && !busy(d)) { error(d.error); previousDeviceError=d.error; }
  else if(state.discovery_error) error(state.discovery_error);
  else if(previousDeviceError && $('error').textContent===previousDeviceError) { error(null); previousDeviceError=null; }
}
function renderNearby(d,live) {
  if(isGuard(d)) {
    $('nearby-count').textContent='0'; $('metric-active').textContent='—';
    $('metric-rssi').textContent='—'; $('metric-received').textContent='—';
    $('metric-radio').textContent='Radios disabled';
    $('receiver').textContent='UART GUARD';
    $('scan-state').textContent=live?(d.status?.ready?'Guard running':'Guard not ready'):'Guard status unavailable';
    $('contacts').innerHTML='<div class="empty-contacts"><h3>Independent UART guard</h3><p>Select the radio sensor to see nearby broadcasts. Open Live log for guard status and rejection counters.</p></div>';
    const s=d.status||{};
    $('health').innerHTML=`<span>Sensor rejects <b>${escape(s.sensor?.rejected??'—')}</b></span><span>Input drops <b>${escape(s.sensor?.dropped_bytes??'—')}</b></span><span>FC output frames <b>${escape(s.fc?.output_frames??'—')}</b></span>`;
    return;
  }
  const contacts=d.contacts||[], active=contacts.filter(c=>c.age_s<5 && live), rssis=active.map(c=>c.rssi_dbm).filter(x=>typeof x==='number');
  $('nearby-count').textContent=active.length;
  $('metric-active').textContent=active.length;
  $('metric-rssi').innerHTML=`${rssis.length?Math.max(...rssis):'—'}<small>dBm</small>`;
  $('metric-received').textContent=(d.status?.rid_observations||0).toLocaleString();
  $('metric-radio').textContent=d.status?.role==='tx'?'Broadcast only':live?'BLE + Wi-Fi':'Paused';
  $('receiver').innerHTML=`${receiverIcon}<strong>${escape(nameOf(d))}</strong><span>${d.status?.role==='both'?'RX + TX':d.status?.role==='tx'?'TRANSMITTER':'RECEIVER'}</span>`;
  $('scan-state').textContent=live ? ({ble:'BLE receive window',wifi_6:'Wi-Fi · channel 6',wifi_149:'Wi-Fi · channel 149',idle:'Radio idle'}[d.status?.rx_phase]||'Waiting for status') : 'Monitor inactive';
  if(!contacts.length) {
    const title = !live?'Monitoring is paused':d.status?.role==='tx'?'This board is broadcasting':'Listening for nearby broadcasts';
    const text = !live?'Start the monitor to see received identities and messages.':d.status?.role==='tx'?'Choose “Receive + broadcast” in Device settings to listen too.':'Power another board and enable broadcasting.<br>Its ID, hello message, and signal will appear here.';
    $('contacts').innerHTML=`<div class="empty-contacts"><h3>${title}</h3><p>${text}</p></div>`;
  } else {
    $('contacts').innerHTML=contacts.map(c=>{
      const rssi=typeof c.rssi_dbm==='number'?c.rssi_dbm:null;
      const bars=rssi===null?0:rssi>=-50?5:rssi>=-65?4:rssi>=-80?3:rssi>=-95?2:1;
      const points=(c.history||[]).map((v,i,a)=>`${a.length===1?0:i*100/(a.length-1)},${22-Math.max(0,Math.min(1,(v+105)/80))*20}`).join(' ');
      const protocol=c.transport==='ble_legacy'?'BLE':c.transport==='wifi_nan'?'Wi-Fi NAN':'Wi-Fi Beacon';
      const message=c.hello==null?'Waiting for hello message':c.hello===''?'No hello message':`“${c.hello}”`;
      return `<article class="contact ${c.age_s>=5||!live?'stale':''}"><div class="contact-top">${drone}<div><strong>${escape(c.id||'Waiting for ID')}</strong><small>${escape(protocol)}${c.channel?` · CH ${escape(c.channel)}`:''}</small></div></div><p class="hello ${c.hello_age_s>10?'old':''}" title="Hello ${escape(age(c.hello_age_s))}">${escape(message)}</p><div class="contact-strength"><span class="signal-bars" aria-hidden="true">${[1,2,3,4,5].map(n=>`<i class="${n<=bars?'on':''}"></i>`).join('')}</span><strong>${rssi===null?'—':rssi} <small>dBm</small></strong><svg class="spark" viewBox="0 0 100 24" preserveAspectRatio="none" aria-label="Recent signal strength"><polyline points="${points}"/></svg></div><div class="contact-foot"><span class="mono">${escape(c.source_address)}</span><span>${escape(age(c.age_s))}</span></div></article>`;
    }).join('');
  }
  const s=d.status||{};
  $('health').innerHTML=`<span>USB <b>${live?'live':'paused'}</b></span><span>Radio errors <b class="${s.radio_errors?'problem':''}">${escape(s.radio_errors??'—')}</b></span><span>Queue drops <b class="${s.rx_queue_dropped?'problem':''}">${escape(s.rx_queue_dropped??'—')}</b></span><span>USB drops <b class="${s.usb_records_dropped?'problem':''}">${escape(s.usb_records_dropped??'—')}</b></span><span>Firmware <b>${escape(s.firmware||'unknown')}</b></span>`;
}
function renderDeviceRole(d) {
  const role=chosenRole(d), running=isGuard(d)?'guard':d.status?.configuration_version===1?'scout':null;
  for(const value of ['scout','guard']) {
    const input=$('device-role-'+value);input.checked=role===value;input.disabled=busy(d);
    input.closest('label').classList.toggle('selected',role===value);
  }
  $('scout-settings').hidden=role!=='scout';$('guard-settings').hidden=role!=='guard';
  $('running-role').textContent=running?`Running: ${running==='guard'?'Guard':'Scout'}`:'Firmware not identified';
  const change=running&&running!==role;
  $('role-change-note').hidden=!change;
  $('role-change-note').textContent=role==='scout'?'Installing Scout replaces the Guard and removes this board’s UART filtering. Disconnect it from the flight controller before changing roles.':'Installing Guard replaces the Scout application. This board will stop receiving and broadcasting drone IDs; use a separate Scout for radio traffic.';
}
for(const value of ['scout','guard']) $('device-role-'+value).addEventListener('change',()=>{
  const d=current();if(!d||busy(d))return;
  deviceRoles.set(d.id,value);$('guard-fixture-confirm').checked=false;render();
});

function renderSettings(d) {
  const supports=!isGuard(d)&&d.status?.configuration_version===1;
  $('upgrade-note').hidden=supports;
  $('upgrade-note').textContent=isGuard(d)?'Install Scout firmware to configure identity and broadcast settings.':'Flash the current firmware to enable ID and hello-message settings.';
  const value=drafts.get(d.id)||settingsOf(d);
  for(const [id,key] of [['broadcast-id','broadcast_id'],['hello','hello'],['role','role'],['transport','transport']]) {
    if($(id).value!==value[key]) $(id).value=value[key];
  }
  $('hello-count').textContent=`${value.hello.length} / 23`;
  const dirty=JSON.stringify(value)!==JSON.stringify(settingsOf(d));
  $('settings-fields').disabled=!supports||!d.present||!online||busy(d);
  $('save').disabled=!dirty||!supports||!d.present||!online||busy(d);
  $('save-state').textContent=busy(d)?'Working…':dirty?'Unsaved changes':'All changes saved';
  $('build-version').textContent=firmware.available?`v${firmware.version} · ${(firmware.size/1024/1024).toFixed(2)} MB`:'No build available';
  $('running-version').textContent=d.status?.firmware?`v${d.status.firmware}`:'Not detected';
  $('flash-serial').textContent=d.serial;
  const wrongModel=isGuard(d)&&d.status?.board_id&&d.status.board_id!=='xiao_esp32c5';
  $('build-note').textContent=wrongModel?'Scout requires a XIAO ESP32-C5. Select a C5 device to install this role.':firmware.available?`Build ${firmware.fingerprint.slice(0,12)} · settings preserved`:firmware.error||'Run idf.py build in an ESP-IDF terminal.';
  $('flash').disabled=chosenRole(d)!=='scout'||wrongModel||!firmware.available||!d.present||!online||busy(d);
}
function renderJob(d) {
  const job=d.job; $('job-panel').hidden=!job;
  if(!job)return;
  $('job-panel').classList.toggle('failed',job.state==='failed');
  $('job-icon').className=job.state==='running'?'spinner':'';
  $('job-icon').textContent=job.state==='complete'?'✓':job.state==='failed'?'!':'';
  $('job-title').textContent=job.state==='complete'?(job.kind==='flash'?'Firmware ready':'Settings saved'):job.state==='failed'?'Operation needs attention':job.kind==='flash'?'Flashing device…':'Saving configuration…';
  if(job.kind.startsWith('guard-')) $('job-title').textContent=job.state==='failed'?'Guard operation failed':job.state==='running'?'Guard operation running…':job.kind==='guard-flash'?'Guard firmware ready':job.kind==='guard-check'?'USB check passed':'Wired UART smoke test passed';
  $('job-message').textContent=job.error||job.lines.at(-1)||'Starting…';
  const pre=$('job-output'), atBottom=pre.scrollHeight-pre.scrollTop-pre.clientHeight<40;
  pre.textContent=job.lines.join('\n'); if(atBottom)pre.scrollTop=pre.scrollHeight;
}
function renderUart(d) {
  const value=uartDrafts.get(d.id)||uartSettings(d), s=d.status?.uart, supported=s?.configuration_version===1;
  for(const key of Object.keys(uartDefaults)) {
    if(uartField(key).value!==String(value[key])) uartField(key).value=String(value[key]);
  }
  const dirty=JSON.stringify(value)!==JSON.stringify(uartSettings(d));
  $('uart-upgrade').hidden=supported||isGuard(d);
  $('uart-fields').disabled=!supported||!d.present||!online||busy(d);
  $('uart-save').disabled=!dirty||!supported||!d.present||!online||busy(d);
  $('uart-save-state').textContent=busy(d)?'Working…':dirty?'Unsaved UART changes':'All UART changes saved';
  const fresh=d.present&&d.monitoring&&d.status_age_s<5;
  $('uart-health').textContent=isGuard(d)?'Guard pins, IDs and limits are set in its separate firmware build.':!s?'UART status unavailable':!fresh?'UART status is stale':!s.enabled?'UART disabled · USB bench active':!s.ready?'UART initialization failed':
    `${s.connected?'Controller heartbeat received':'Waiting for controller heartbeat'} · TX ${s.tx_frames} · RX ${s.rx_frames} · Dropped ${s.queue_drops+s.expired} · Errors ${s.errors+s.parse_errors}`;
}
function switchTab(value) {
  tab=value;
  for(const button of document.querySelectorAll('[data-tab]')) {
    const active=button.dataset.tab===tab; button.setAttribute('aria-selected',String(active)); button.tabIndex=active?0:-1;
    $('panel-'+button.dataset.tab).hidden=!active;
  }
  if(tab==='logs') {renderLogs();pollLogs();}
}
function renderLogs() {
  const filter=$('log-filter').value;
  const rows=logRows.filter(x=>filter==='all'||x.record.kind===filter);
  const pre=$('log-stream'); pre.textContent=rows.map(x=>JSON.stringify(x.record)).join('\n')||'Waiting for USB records…';
  if(!scrollPaused)pre.scrollTop=pre.scrollHeight;
  $('log-caption').textContent=`${rows.length} records shown · ${nameOf(current()||{serial:''})}`;
}
async function pollLogs() {
  if(tab!=='logs'||!selected||logBusy||!online)return;
  const id=selected; logBusy=true;
  try {
    const data=await api(`/api/devices/${id}/logs?after=${logCursor}`);
    if(id!==selected)return;
    logRows=[...logRows,...data.logs].slice(-500);logCursor=data.cursor;renderLogs();
  }catch(e){error(e.message);}finally{logBusy=false;}
}
document.querySelectorAll('[data-tab]').forEach(button=>{
  button.addEventListener('click',()=>switchTab(button.dataset.tab));
  button.addEventListener('keydown',e=>{
    if(!['ArrowLeft','ArrowRight'].includes(e.key))return;
    const all=[...document.querySelectorAll('[data-tab]')],i=all.indexOf(button),next=all[(i+(e.key==='ArrowRight'?1:all.length-1))%all.length];
    e.preventDefault();switchTab(next.dataset.tab);next.focus();
  });
});
$('settings-form').addEventListener('input',()=>{
  if(!current())return;
  drafts.set(selected,{broadcast_id:$('broadcast-id').value,hello:$('hello').value,role:$('role').value,transport:$('transport').value});
  renderSettings(current());
});
$('settings-form').addEventListener('submit',async e=>{
  e.preventDefault();const d=current();if(!d||busy(d))return;
  const value=drafts.get(d.id)||settingsOf(d);
  if(!/^[A-Za-z0-9._-]{1,19}$/.test(value.broadcast_id)){error('Use 1–19 letters, numbers, dots, underscores, or hyphens for the ID.');return;}
  if(!/^[\x20-\x7e]{0,23}$/.test(value.hello)){error('Hello messages support up to 23 printable ASCII characters.');return;}
  try{await api(`/api/devices/${d.id}/configure`,value);error(null);await refresh();}catch(e){error(e.message);}
});
$('flash').addEventListener('click',async()=>{
  const d=current();if(!d||busy(d)||$('flash').disabled||chosenRole(d)!=='scout')return;
  $('flash').disabled=true;
  try{await api(`/api/devices/${d.id}/provision`,{role:'scout',board:'xiao_esp32c5',fingerprint:firmware.fingerprint});error(null);await refresh();}catch(e){error(e.message);renderSettings(d);}
});
$('uart-form').addEventListener('input',()=>{
  if(!current())return;
  uartDrafts.set(selected,Object.fromEntries(Object.keys(uartDefaults).map(k=>[k,k==='enabled'?uartField(k).value==='true':Number(uartField(k).value)])));
  renderUart(current());
});
$('uart-form').addEventListener('submit',async e=>{
  e.preventDefault();const d=current();if(!d||busy(d))return;
  try{await api(`/api/devices/${d.id}/uart`,uartDrafts.get(d.id)||uartSettings(d));error(null);await refresh();}catch(e){error(e.message);}
});
$('monitor-toggle').addEventListener('click',async()=>{
  const d=current();if(!d||pendingMonitors.has(d.id))return;
  pendingMonitors.add(d.id);render();
  try{await api(`/api/devices/${d.id}/monitor`,{enabled:!d.monitoring});error(null);await refresh();}catch(e){error(e.message);}
  finally{pendingMonitors.delete(d.id);render();}
});
$('identify').addEventListener('click',async()=>{
  const d=current();if(!d||$('identify').disabled)return;
  identifying.add(d.id);render();
  try{
    const data=await api(`/api/devices/${d.id}/identify`,{});
    const device=state.devices.find(x=>x.id===d.id);
    if(device){device.status=data.status;device.status_age_s=0;}
    error(null);
  }catch(e){error(e.message);}
  finally{identifying.delete(d.id);render();}
});
$('refresh').addEventListener('click',refresh);
$('log-filter').addEventListener('change',renderLogs);
$('log-pause').addEventListener('click',()=>{scrollPaused=!scrollPaused;$('log-pause').textContent=scrollPaused?'Resume scroll':'Pause scroll';});
function renderGuardOptions() {
  const model=$('guard-model'), choice=model.value;
  model.innerHTML='<option value="">Select a model…</option>'+guardModels.filter(p=>p.supported).map(p=>`<option value="${escape(p.id)}">${escape(p.name)}</option>`).join('');
  model.value=choice;
  for(const id of ['guard-sensor-adapter','guard-fc-adapter']) {
    const field=$(id), previous=field.value;
    field.innerHTML='<option value="">Choose adapter…</option>'+(fixture.adapters||[]).map(a=>`<option value="${escape(a.id)}">${escape(a.label)}</option>`).join('');field.value=previous;
  }
  $('guard-models').innerHTML=guardModels.map(p=>`<article class="guard-model-card"><h4>${escape(p.name)}</h4><span class="badge">${p.supported?'Firmware profile':'Port required'}</span><p>${escape(p.note)}</p><a href="${escape(p.source)}" target="_blank" rel="noreferrer">Seeed documentation ↗</a></article>`).join('');
}
function guardChoice(d) { return guardModels.find(p=>p.id===(guardChoices.get(d.id)??d.status?.board_id)); }
function renderGuards(d) {
  const p=guardChoice(d), build=p?.build, s=d.status||{}, fresh=d.present&&d.monitoring&&d.status_age_s!=null&&d.status_age_s<5;
  $('guard-model').value=p?.id||'';
  $('guard-model').disabled=busy(d);
  $('guard-serial').textContent=d.serial;
  $('guard-model-note').textContent=p?.note||'Choose the board model printed on the physical device.';
  $('guard-build').textContent=build?.available?`v${build.version} · ${Math.round(build.size/1024)} KB`:'Not available';
  $('guard-running').textContent=isGuard(d)?`${s.board_id||'Legacy guard'} · v${s.firmware||'unknown'}`:'Guard firmware not detected';
  $('guard-build-note').textContent=build?.available?`Build ${build.fingerprint.slice(0,12)} · radio/network components excluded`:(build?.error||'Select a profile to see its local firmware build.');
  $('guard-build-command').textContent=p?`python guard/build.py --board ${p.id}`:'Build a selected profile in an ESP-IDF terminal.';
  const disabled=chosenRole(d)!=='guard'||!p||!online||!d.present||busy(d)||!build?.available;
  $('guard-flash').disabled=disabled;
  $('guard-check').disabled=disabled||!isGuard(d);
  $('guard-status').textContent=!isGuard(d)?'Waiting for guard firmware.':!fresh?'Status stale — start monitoring or reconnect USB.':`${s.ready===true?'Guard ready':'Guard not ready'} · ${s.radios_enabled===false?'Radios disabled':'Radio state unknown'} · ${s.policy_selftest===true?'Startup policy test passed':'Startup test not confirmed'}`;
  $('guard-counters').innerHTML=[['Sensor rejects',s.sensor?.rejected],['FC rejects',s.fc?.rejected],['Sensor drops',s.sensor?.dropped_bytes],['FC output frames',s.fc?.output_frames]].map(([label,value])=>`<div><span>${escape(label)}</span><b>${escape(fresh&&isGuard(d)?value??'—':'—')}</b></div>`).join('');
  $('guard-fixture-note').textContent=!fixture.available?'Install tools/requirements-mavlink.txt in the server’s Python environment, then refresh.':fixture.adapters.length<2?'Connect two uniquely identified FTDI / CP210x / CH34x / Prolific USB UART adapters, then refresh.':'Adapters must use 3.3 V logic. The server opens only the two selected fixture adapters.';
  $('guard-uart-test').disabled=disabled||!isGuard(d)||!fresh||!fixture.available||!$('guard-fixture-confirm').checked||!$('guard-sensor-adapter').value||!$('guard-fc-adapter').value||$('guard-sensor-adapter').value===$('guard-fc-adapter').value;
}
$('guard-model').addEventListener('change',()=>{const d=current();if(d){guardChoices.set(d.id,$('guard-model').value);$('guard-fixture-confirm').checked=false;renderGuards(d);}});
for(const id of ['guard-sensor-adapter','guard-fc-adapter','guard-fixture-confirm']) $(id).addEventListener('change',()=>{if(current())renderGuards(current());});
for(const action of ['guard-flash','guard-check','guard-uart-test']) $(action).addEventListener('click',async()=>{
  const d=current(), p=d&&guardChoice(d);if(!d||!p||busy(d)||$(action).disabled)return;
  $(action).disabled=true;
  const body={board:p.id,fingerprint:p.build?.fingerprint};
  const endpoint=action==='guard-flash'?'provision':action;
  if(action==='guard-flash')body.role='guard';
  if(action==='guard-uart-test')Object.assign(body,{sensor_adapter:$('guard-sensor-adapter').value,fc_adapter:$('guard-fc-adapter').value,fixture_confirmed:$('guard-fixture-confirm').checked});
  try{await api(`/api/devices/${d.id}/${endpoint}`,body);error(null);await refresh();}catch(e){error(e.message);renderGuards(d);}
});

async function start() {
  switchTab('nearby');
  await refresh();
  events=new EventSource('/api/events');
  events.addEventListener('state',e=>{online=true;update(JSON.parse(e.data));});
  events.onerror=()=>{online=false;renderConnection();};
  setInterval(pollLogs,1000);
}
start();
