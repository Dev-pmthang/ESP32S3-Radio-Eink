/* ESP32-S3 Radio dashboard (minimal): polls GET /api/status every 1.5 s. */
(function () {
  'use strict';

  var POLL_MS = 1500;
  var $ = function (id) { return document.getElementById(id); };
  var state = { volumeDragging: false, lastVolumeSent: -1, stationIndex: -1, mono: false, dispType: '', dispRot: -1, inflight: false };

  function setBadge(el, text, cls) { el.textContent = text; el.className = 'badge' + (cls ? ' ' + cls : ''); }

  function post(path, body) {
    return fetch(path, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body || {})
    }).then(function (r) { return r.json().then(function (j) { j._status = r.status; return j; }); });
  }

  function render(d) {
    var pb = $('pb-state');
    switch (d.state) {
      case 'playing': setBadge(pb, 'playing', 'ok'); break;
      case 'buffering': setBadge(pb, 'buffering', 'info'); break;
      case 'connecting': setBadge(pb, 'connecting', 'warn'); break;
      case 'error': setBadge(pb, 'error', 'err'); break;
      default: setBadge(pb, 'stopped', '');
    }
    $('station-name').textContent = d.station.name || '---';
    $('stream-title').textContent = d.title || '';
    $('error-line').textContent = d.error || '';

    var toggle = $('btn-toggle');
    toggle.textContent = d.state === 'stopped' ? 'Play' : 'Stop';
    toggle.dataset.action = d.state === 'stopped' ? 'resume' : 'stop';

    if (!state.volumeDragging && d.volume !== state.lastVolumeSent) {
      $('volume').value = d.volume;
      $('volume-val').textContent = d.volume + '%';
    }
    state.mono = !!d.mono;
    $('btn-mono').textContent = d.mono ? 'Mono: on' : 'Mono: off';

    if (d.battery) {
      $('bat-badge').textContent = 'BAT ' + d.battery.pct + '%' + (d.battery.simulated ? ' (sim)' : '');
    }
    if (d.display) {
      if (d.display.type && d.display.type !== state.dispType) {
        state.dispType = d.display.type;
        $('disp-type').value = d.display.type;
      }
      if (typeof d.display.rotation === 'number' && d.display.rotation !== state.dispRot) {
        state.dispRot = d.display.rotation;
        $('disp-rot').value = String(d.display.rotation);
      }
    }

    var ws = $('wifi-state');
    if (d.wifi.connected) { setBadge(ws, 'connected', 'ok'); }
    else if (d.wifi.ap_active) { setBadge(ws, 'setup hotspot', 'warn'); }
    else { setBadge(ws, 'connecting', ''); }
    $('wifi-line').textContent = (d.wifi.ssid || '(no network)') + '  ' + d.wifi.ip +
      (d.wifi.ap_active ? '  |  hotspot ESP32-S3-Radio @ 192.168.4.1' : '');

    if (d.station.index !== state.stationIndex) {
      state.stationIndex = d.station.index;
      markActiveStation();
    }
  }

  function poll() {
    if (state.inflight) { return; }
    state.inflight = true;
    fetch('/api/status', { cache: 'no-store' })
      .then(function (r) { return r.json(); })
      .then(render)
      .catch(function () { setBadge($('pb-state'), 'offline', 'err'); })
      .then(function () { state.inflight = false; });
  }
  poll();
  setInterval(poll, POLL_MS);

  function markActiveStation() {
    var items = $('stations').children;
    for (var i = 0; i < items.length; i++) {
      items[i].classList.toggle('active', Number(items[i].dataset.index) === state.stationIndex);
    }
  }

  fetch('/api/stations', { cache: 'no-store' })
    .then(function (r) { return r.json(); })
    .then(function (d) {
      var ul = $('stations');
      ul.innerHTML = '';
      d.stations.forEach(function (s) {
        var li = document.createElement('li');
        li.dataset.index = s.index;
        li.title = s.url;
        li.innerHTML = '<div><div class="s-name"></div><div class="s-genre"></div></div><span class="s-idx"></span>';
        li.querySelector('.s-name').textContent = s.name;
        li.querySelector('.s-genre').textContent = s.genre;
        li.querySelector('.s-idx').textContent = '#' + (s.index + 1);
        li.addEventListener('click', function () { post('/api/play', { station: s.index }).then(poll); });
        ul.appendChild(li);
      });
      state.stationIndex = d.current;
      markActiveStation();
    })
    .catch(function () {});

  $('btn-prev').addEventListener('click', function () { post('/api/previous').then(poll); });
  $('btn-next').addEventListener('click', function () { post('/api/next').then(poll); });
  $('btn-toggle').addEventListener('click', function () {
    post(this.dataset.action === 'resume' ? '/api/resume' : '/api/stop').then(poll);
  });
  $('btn-mono').addEventListener('click', function () { post('/api/mono', { mono: !state.mono }).then(poll); });

  $('disp-rot').addEventListener('change', function () {
    var r = Number(this.value);
    state.dispRot = r;
    $('disp-msg').textContent = 'Rotating screen to ' + r + ' deg...';
    post('/api/display', { rotation: r }).then(function (j) {
      $('disp-msg').textContent = j.ok ? ('Rotation set to ' + r + ' deg') : (j.error || 'failed');
    }).catch(function () { $('disp-msg').textContent = 'request failed'; });
  });

  $('disp-type').addEventListener('change', function () {
    var t = this.value;
    state.dispType = t;
    $('disp-msg').textContent = 'Saving display type & rebooting...';
    post('/api/display', { type: t }).then(function (j) {
      if (j.ok) {
        setTimeout(function () { location.reload(); }, 3500);
      } else {
        $('disp-msg').textContent = j.error || 'failed';
      }
    }).catch(function () { $('disp-msg').textContent = 'request failed'; });
  });

  var volTimer = null;
  var vol = $('volume');
  function sendVolume(v) { state.lastVolumeSent = v; post('/api/volume', { volume: v }).catch(function () {}); }
  vol.addEventListener('input', function () {
    state.volumeDragging = true;
    $('volume-val').textContent = vol.value + '%';
    clearTimeout(volTimer);
    volTimer = setTimeout(function () { sendVolume(Number(vol.value)); }, 150);
  });
  vol.addEventListener('change', function () {
    clearTimeout(volTimer);
    sendVolume(Number(vol.value));
    setTimeout(function () { state.volumeDragging = false; }, 400);
  });

  var scanBtn = $('btn-scan');
  var scanList = $('wifi-scan-list');
  scanBtn.addEventListener('click', function () {
    scanBtn.disabled = true;
    scanBtn.textContent = 'Scanning...';
    $('wifi-msg').textContent = 'scanning, a few seconds';
    fetch('/api/scan', { cache: 'no-store' })
      .then(function (r) { return r.json(); })
      .then(function (d) {
        if (!d.networks) { throw new Error(d.error || 'scan failed'); }
        scanList.innerHTML = '';
        var first = document.createElement('option');
        first.value = '';
        first.textContent = d.networks.length ? '-- choose a network --' : '-- no networks found --';
        scanList.appendChild(first);
        d.networks.forEach(function (n) {
          var o = document.createElement('option');
          o.value = n.ssid;
          o.textContent = n.ssid + ' (' + n.rssi + ' dBm' + (n.secure ? '' : ', open') + ')';
          scanList.appendChild(o);
        });
        $('wifi-msg').textContent = d.networks.length + ' network(s) found';
      })
      .catch(function (e) { $('wifi-msg').textContent = 'scan error: ' + e.message; })
      .then(function () { scanBtn.disabled = false; scanBtn.textContent = 'Scan networks'; });
  });
  scanList.addEventListener('change', function () {
    if (scanList.value) { $('wifi-ssid-in').value = scanList.value; $('wifi-pass-in').focus(); }
  });

  /* firmware OTA */
  fetch('/api/ota', { cache: 'no-store' })
    .then(function (r) { return r.json(); })
    .then(function (d) {
      $('fw-version').textContent = 'v' + d.version + ' on ' + d.running_partition + ' (' + d.built + ')';
    })
    .catch(function () {});

  $('btn-fw').addEventListener('click', function () {
    var f = $('fw-file').files[0];
    var msg = $('fw-msg');
    var bar = $('fw-progress');
    if (!f) { msg.textContent = 'choose a .bin file first'; return; }
    if (!confirm('Flash ' + f.name + ' (' + Math.round(f.size / 1024) + ' KB)? Playback stops and the radio restarts.')) { return; }
    var btn = this;
    btn.disabled = true;
    msg.textContent = 'uploading...';
    bar.style.width = '0%';
    var xhr = new XMLHttpRequest();
    xhr.open('POST', '/api/ota', true);
    xhr.setRequestHeader('Content-Type', 'application/octet-stream');
    xhr.upload.onprogress = function (e) {
      if (e.lengthComputable) { bar.style.width = Math.round(e.loaded * 100 / e.total) + '%'; }
    };
    xhr.onload = function () {
      var j = {};
      try { j = JSON.parse(xhr.responseText); } catch (e) { j = {}; }
      if (xhr.status === 200 && j.ok) {
        bar.style.width = '100%';
        msg.textContent = 'written. Restarting... the page reconnects in about 15 s.';
        setTimeout(function () { location.reload(); }, 15000);
      } else {
        msg.textContent = 'failed: ' + (j.error || ('HTTP ' + xhr.status));
        btn.disabled = false;
      }
    };
    xhr.onerror = function () { msg.textContent = 'upload failed (connection lost)'; btn.disabled = false; };
    xhr.send(f);
  });

  $('wifi-form').addEventListener('submit', function (ev) {
    ev.preventDefault();
    var ssid = $('wifi-ssid-in').value.trim();
    var pass = $('wifi-pass-in').value;
    var msg = $('wifi-msg');
    if (!ssid) { msg.textContent = 'SSID required'; return; }
    if (pass.length > 0 && pass.length < 8) { msg.textContent = 'password must be 8-63 chars or empty'; return; }
    msg.textContent = 'saving...';
    post('/api/wifi', { ssid: ssid, password: pass })
      .then(function (j) {
        msg.textContent = j.ok ? 'saved, connecting. Rejoin your home network and open the new IP.' : ('error: ' + (j.error || j._status));
        $('wifi-pass-in').value = '';
      })
      .catch(function () { msg.textContent = 'request failed (device may be switching networks)'; });
  });
})();
