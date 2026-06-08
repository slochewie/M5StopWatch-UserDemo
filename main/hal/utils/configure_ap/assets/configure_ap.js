const ids = [
  'device_name',
  'wifi_ssid',
  'wifi_password',
  'mqtt_username',
  'mqtt_password',
  'counter_topic'
];

const el = id => document.getElementById(id);

function st(s) {
  el('status').textContent = s;
}

function mqttUri() {
  const prefix = el('mqtt_encrypt').checked ? 'mqtts://' : 'mqtt://';
  const host = el('mqtt_host').value.trim();
  const port = el('mqtt_port').value.trim() || '1883';
  return prefix + host + ':' + port;
}

function updatePreview() {
  el('mqtt_preview').textContent = mqttUri();
}

function parseMqttUri(uri) {
  let u = uri || '';
  let secure = false;

  if (u.startsWith('mqtts://')) {
    secure = true;
    u = u.slice(8);
  } else if (u.startsWith('mqtt://')) {
    u = u.slice(7);
  }

  let host = u;
  let port = '1883';

  const slash = host.indexOf('/');
  if (slash >= 0) {
    host = host.slice(0, slash);
  }

  const colon = host.lastIndexOf(':');
  if (colon > 0) {
    port = host.slice(colon + 1) || '1883';
    host = host.slice(0, colon);
  }

  el('mqtt_encrypt').checked = secure;
  el('mqtt_host').value = host;
  el('mqtt_port').value = port || '1883';
  updatePreview();
}

function payload() {
  const b = {};
  ids.forEach(k => b[k] = el(k).value);
  b.mqtt_uri = mqttUri();
  return b;
}

function showSaved() {
  el('saved_card').classList.remove('hidden');
}

async function load() {
  const r = await fetch('/config');
  const c = await r.json();

  ids.forEach(k => {
    if (c[k] !== undefined) {
      el(k).value = c[k];
    }
  });

  parseMqttUri(c.mqtt_uri || '');
  st('AP: ' + (c.ap_ssid || '') + '\nURL: ' + (c.ap_url || ''));
}

async function save(e) {
  e.preventDefault();

  const r = await fetch('/config', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify(payload())
  });

  st(await r.text());
  showSaved();
}

async function testMqtt() {
  st('Testing MQTT connection...');

  const r = await fetch('/mqtt/test', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify(payload())
  });

  st(await r.text());
}

async function scan() {
  st('Scanning...');

  const r = await fetch('/wifi/scan');
  const j = await r.json();
  const s = el('ssid_select');

  s.innerHTML = '';

  j.networks.forEach(n => {
    const o = document.createElement('option');
    o.value = n.ssid;
    o.textContent = n.ssid + ' RSSI ' + n.rssi;
    s.appendChild(o);
  });

  st('Scan complete');
}

async function reboot() {
  st('Rebooting device...');
  await fetch('/reboot', {method: 'POST'});
}

el('form').addEventListener('submit', save);
el('reload').addEventListener('click', load);
el('scan').addEventListener('click', scan);
el('mqtt_test').addEventListener('click', testMqtt);
el('reboot').addEventListener('click', reboot);

el('ssid_select').addEventListener('change', () => {
  if (el('ssid_select').value) {
    el('wifi_ssid').value = el('ssid_select').value;
  }
});

el('close').addEventListener('click', async () => {
  await fetch('/close', {method: 'POST'});
  st('Portal closing');
});

['mqtt_encrypt', 'mqtt_host', 'mqtt_port'].forEach(id => {
  el(id).addEventListener('input', updatePreview);
  el(id).addEventListener('change', updatePreview);
});

load().catch(e => st('Load failed: ' + e));
