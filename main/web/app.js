'use strict';
const $ = id => document.getElementById(id);
let token = sessionStorage.getItem('token') || '', tab = 'status', polling = false, uploading = false, rebooting = false, bootId = '';
function notify(text, error = false) { $('message').textContent = text; $('message').classList.toggle('error', error); }
function loginView() { $('main').hidden = true; $('login').hidden = false; $('logout').hidden = true; }
async function api(path, data, raw = false) {
  const headers = token ? {Authorization: `Bearer ${token}`} : {};
  if (data !== undefined) headers['Content-Type'] = raw ? 'application/octet-stream' : 'application/json';
  const response = await fetch(`/api/${path}`, {method: data === undefined ? 'GET' : 'POST', headers,
    body: data === undefined ? undefined : raw ? data : JSON.stringify(data), cache: 'no-store', signal: AbortSignal.timeout(raw ? 610000 : 20000)});
  const result = await response.json();
  if (!response.ok) {
    if (response.status === 401) { token = ''; sessionStorage.removeItem('token'); loginView(); }
    throw new Error(result.message || `HTTP ${response.status}`);
  }
  return result;
}
function action(fn) { return async event => { if (event) event.preventDefault(); try { await fn(event); } catch (error) { notify(error.message, true); } }; }
const groups = {
  network: [['hostname','Hostname'],['dhcp','Получать адрес по DHCP','bool'],['ip','Статический IP'],['mask','Маска сети'],['gateway','Шлюз'],['dns','DNS-сервер'],['vlan','VLAN ID (0 = без тега)','number',0,4094]],
  serial: [['baud','Скорость, бод','number',1200,115200],['data_bits','Биты данных','select',['5','6','7','8']],['parity','Чётность','select',['none','even','odd']],['stop_bits','Стоповые биты','select',['1','2']]],
  mail: [['smtp','Отправлять письмо после загрузки','bool'],['smtp_host','SMTP-сервер'],['smtp_port','Порт SMTP','number',1,65535],['mail_from','Отправитель'],['mail_to','Получатель']],
  system: [['web_port','Порт HTTPS','number',1,65535],['ssh','SSH включён','bool'],['ssh_port','Порт SSH','number',1,65535],['telnet','Telnet включён (пароль и данные без шифрования)','bool'],['telnet_port','Порт Telnet','number',1,65535]]
};
function settingsForms(settings) {
  for (const [group, fields] of Object.entries(groups)) {
    const form = $(`${group}-form`); form.replaceChildren();
    for (const [name, title, kind, a, b] of fields) {
      const label = document.createElement('label'); label.textContent = title;
      const input = document.createElement(kind === 'select' ? 'select' : 'input'); input.name = name;
      if (kind === 'select') for (const value of a) { const option = document.createElement('option'); option.value = value; option.textContent = value; input.append(option); }
      else if (kind === 'bool') { input.type = 'checkbox'; input.checked = settings[name]; }
      else if (kind === 'number') { input.type = 'number'; input.min = a; input.max = b; input.required = true; }
      else { input.type = 'text'; input.maxLength = 253; }
      if (kind !== 'bool') input.value = settings[name]; label.append(input); form.append(label);
    }
    const button = document.createElement('button'); button.textContent = 'Сохранить'; form.append(button);
    form.onsubmit = action(async () => {
      const values = {};
      for (const [name,,kind] of fields) { const input = form.elements.namedItem(name); values[name] = kind === 'bool' ? input.checked : (kind === 'number' || name === 'data_bits' || name === 'stop_bits') ? Number(input.value) : input.value; }
      await api('settings', values); notify('Сохранено. Изменения применятся после перезагрузки.');
    });
  }
}
async function updateStatus() {
  const s = await api('status'); bootId = s.boot_id;
  const state = s.resources & 8 ? 'Передача файла' : s.resources & 1 ? 'Терминал подключён' : 'Свободен';
  const values = {'Hostname':s.hostname,'IP':s.ip,'Ethernet':s.network?'Доступен':'Нет адреса/связи','microSD':s.microSD?'Смонтирована':'Недоступна','Serial':state,'SSH':s.ssh?`Порт ${s.ssh_port}`:'Выключен/недоступен','Telnet':s.telnet?`Порт ${s.telnet_port}`:'Выключен/недоступен','HTTPS':`Порт ${s.web_port}`,'Время работы':`${s.uptime} с`,'Свободная память':`${s.free_heap} байт`};
  $('status-data').replaceChildren();
  for (const [key,value] of Object.entries(values)) { const dt=document.createElement('dt'),dd=document.createElement('dd');dt.textContent=key;dd.textContent=value;$('status-data').append(dt,dd); }
  $('transfer-state').textContent = `Передача: ${s.transfer}; ${s.sent} / ${s.size} байт`;
  $('storage-state').textContent = `Карта: ${s.storage}`;
}
function tableButton(cell, title, handler) { const b=document.createElement('button');b.type='button';b.textContent=title;b.onclick=action(handler);cell.append(b); }
async function loadUsers() {
  const list=await api('users');$('user-list').replaceChildren();
  for(const u of list) {
    const tr=document.createElement('tr');for(const text of [u.name,u.enabled?'Да':'Нет']){const td=document.createElement('td');td.textContent=text;tr.append(td);}
    const td=document.createElement('td');
    tableButton(td,'Изменить',()=>{const f=$('user-form');f.elements.name.value=u.name;f.elements.password.value='';f.elements.enabled.checked=u.enabled;f.elements.public_key.value=u.public_key;});
    tableButton(td,'Удалить',async()=>{if(!confirm(`Удалить пользователя ${u.name}?`))return;await api('users/delete',{name:u.name});signedOut('Пользователь удалён. Войдите снова.');});
    tr.append(td);$('user-list').append(tr);
  }
}
async function loadFiles() {
  const list=await api('files');$('file-list').replaceChildren();
  for(const f of list) {
    const tr=document.createElement('tr');for(const text of [f.name,f.size]){const td=document.createElement('td');td.textContent=text;tr.append(td);}
    const td=document.createElement('td');tableButton(td,'Выбрать',()=>{$('transfer-form').elements.name.value=f.name;});
    tableButton(td,'Удалить',async()=>{if(!confirm(`Удалить ${f.name}?`))return;await api('files/delete',{name:f.name});await loadFiles();});tr.append(td);$('file-list').append(tr);
  }
}
async function showTab(name) {
  tab=name;document.querySelectorAll('main>section').forEach(s=>s.hidden=s.id!==name);
  document.querySelectorAll('[data-tab]').forEach(b=>{if(b.dataset.tab===name)b.setAttribute('aria-current','page');else b.removeAttribute('aria-current');});
  if(name==='users')await loadUsers();if(name==='files')await loadFiles();if(name==='status'||name==='files')await updateStatus();
}
async function enter() {const settings=await api('settings');settingsForms(settings);$('login').hidden=true;$('main').hidden=false;$('logout').hidden=false;await showTab('status');}
function signedOut(message) {token='';sessionStorage.removeItem('token');loginView();notify(message);}
$('login').onsubmit=action(async()=>{const f=$('login');const response=await api('login',{name:f.elements.name.value,password:f.elements.password.value});f.elements.password.value='';token=response.token;sessionStorage.setItem('token',token);notify('');await enter();});
$('logout').onclick=action(async()=>{await api('logout',{});signedOut('Вы вышли.');});
document.querySelectorAll('[data-tab]').forEach(b=>b.onclick=action(()=>showTab(b.dataset.tab)));
$('user-form').onsubmit=action(async()=>{const f=$('user-form');await api('users',{name:f.elements.name.value,password:f.elements.password.value,enabled:f.elements.enabled.checked,public_key:f.elements.public_key.value.trim()});f.elements.password.value='';signedOut('Пользователь сохранён. Войдите снова.');});
$('pubkey-file').onchange=action(async()=>{const file=$('pubkey-file').files[0];if(!file)return;if(file.size>4096)throw new Error('Ключ слишком большой');$('user-form').elements.public_key.value=(await file.text()).trim();});
$('refresh-files').onclick=action(loadFiles);
$('upload-form').onsubmit=action(async()=>{const file=$('upload-file').files[0];if(!file)return;if(file.size>64*1024*1024||!/^[A-Za-z0-9_-][A-Za-z0-9._-]{0,59}$/.test(file.name))throw new Error('Недопустимое имя или размер файла');uploading=true;const b=$('upload-form').querySelector('button');b.disabled=true;notify('Загрузка файла…');try{await api(`upload?name=${encodeURIComponent(file.name)}`,file,true);notify('Файл загружен.');await loadFiles();}finally{uploading=false;b.disabled=false;}});
$('transfer-form').onsubmit=action(async()=>{const f=$('transfer-form');await api('transfer',{name:f.elements.name.value,protocol:f.elements.protocol.value});notify('Передача запущена. Ожидание приёмника.');await updateStatus();});
$('cancel-transfer').onclick=action(async()=>{await api('transfer/cancel',{});notify('Запрошена отмена передачи.');});
$('format').onclick=action(async()=>{if(prompt('Все файлы microSD будут удалены. Введите FORMAT для подтверждения:')!=='FORMAT')return;await api('format',{confirm:'FORMAT'});$('file-list').replaceChildren();notify('Форматирование запущено. Результат появится в состоянии карты.');await updateStatus();});
$('tls-form').onsubmit=action(async()=>{const cert=$('tls-cert').files[0],key=$('tls-key').files[0];if(!cert||!key)return;if(cert.size+key.size>24000)throw new Error('Сертификат и ключ слишком большие');await api('tls',{cert:await cert.text(),key:await key.text()});$('tls-form').reset();notify('Сертификат сохранён. Перезагрузите устройство для применения.');});
$('reboot').onclick=action(async()=>{
  if(!confirm('Перезагрузить донгл? Все терминальные и веб-соединения будут разорваны. Настройки и файлы сохранятся.'))return;
  const oldBoot=(await api('health')).boot_id;await api('reboot',{confirm:'REBOOT'});rebooting=true;notify('Перезагрузка. Ожидание ответа устройства…');
  const deadline=Date.now()+90000;
  while(Date.now()<deadline){await new Promise(resolve=>setTimeout(resolve,2500));try{const h=await api('health');if(h.boot_id!==oldBoot){rebooting=false;signedOut('Устройство перезагрузилось. Войдите снова.');return;}}catch{}}
  rebooting=false;notify('Устройство пока не ответило. Если менялись IP, VLAN или порт HTTPS, откройте новый адрес. Состояние перезагрузки не подтверждено.',true);
});
setInterval(async()=>{if(!token||document.hidden||polling||uploading||rebooting||!['status','files'].includes(tab))return;polling=true;try{await updateStatus();}catch(e){notify(e.message,true);}finally{polling=false;}},3000);
if(token)action(enter)();
