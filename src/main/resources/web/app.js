// src/main/resources/web/app.js
const byId = id => document.getElementById(id);
let apiToken = '';
let status = null;
let dirty = false;
let editingVersion = null;
let loading = false;
let lastCaptured = null;
const history = { cpu: [], memory: [] };
const percent = value => value == null ? '—' : value.toFixed(1);
const bytes = value => `${(value / 1024 ** 3).toFixed(1)} GB`;
const date = value => new Date(value).toLocaleString('ko-KR', { hour12: false });

function message(text, error = false) {
  byId('message').textContent = text;
  byId('message').classList.toggle('error', error);
  byId('message').hidden = !text;
}

async function api(path, options = {}) {
  const response = await fetch(path, { ...options, signal: AbortSignal.timeout(10000),
    headers: { Authorization: `Bearer ${apiToken}`, 'Content-Type': 'application/json', ...options.headers } });
  const body = await response.json();
  if (!response.ok) {
    const error = new Error(body.error || `요청 실패 (${response.status})`);
    error.status = response.status;
    throw error;
  }
  return body;
}

function drawChart(name) {
  const canvas = byId(`${name}-chart`);
  const ctx = canvas.getContext('2d');
  const values = history[name];
  ctx.clearRect(0, 0, canvas.width, canvas.height);
  if (values.length < 2) return;
  const points = values.map((value, index) => [index * canvas.width / (values.length - 1), 65 - value * .6]);
  ctx.beginPath();
  points.forEach(([x, y], index) => index ? ctx.lineTo(x, y) : ctx.moveTo(x, y));
  ctx.strokeStyle = '#25948a';
  ctx.lineWidth = 2;
  ctx.stroke();
  ctx.lineTo(canvas.width, 70);
  ctx.lineTo(0, 70);
  ctx.closePath();
  const fill = ctx.createLinearGradient(0, 0, 0, 70);
  fill.addColorStop(0, '#0e80722b');
  fill.addColorStop(1, '#0e807202');
  ctx.fillStyle = fill;
  ctx.fill();
}

function renderMetric(name, value, active, rule) {
  byId(`${name}-value`).textContent = percent(value);
  byId(`${name}-threshold`).textContent = `${rule.thresholdPercent}%`;
  const pill = byId(`${name}-state`);
  pill.textContent = value == null ? '측정 대기' : !rule.enabled ? '알림 해제' : active ? '도달' : value >= rule.thresholdPercent ? '확인 중' : '정상';
  pill.className = `pill ${value == null || !rule.enabled ? '' : active ? 'warn' : 'good'}`;
  const meter = byId(`${name}-meter`);
  meter.style.width = `${value == null ? 0 : Math.max(0, Math.min(100, value))}%`;
  meter.classList.toggle('warn', active);
}

function renderEvents(events) {
  const body = byId('event-body');
  body.replaceChildren();
  byId('event-count').textContent = `${events.length}건`;
  if (!events.length) {
    const row = body.insertRow();
    const cell = row.insertCell();
    cell.colSpan = 5;
    cell.className = 'empty';
    cell.textContent = '아직 발생한 이벤트가 없습니다.';
    return;
  }
  for (const event of events) {
    const row = body.insertRow();
    row.insertCell().textContent = date(event.occurredAt);
    row.insertCell().textContent = event.resource;
    const badge = document.createElement('span');
    badge.className = `pill ${event.type === 'RECOVERED' ? 'good' : 'warn'}`;
    badge.textContent = { THRESHOLD_REACHED: '임계치 도달', THRESHOLD_REMINDER: '재알림', RECOVERED: '복구' }[event.type] || event.type;
    row.insertCell().append(badge);
    row.insertCell().textContent = `${percent(event.usedPercent)}% / ${event.thresholdPercent}%`;
    row.insertCell().textContent = event.type === 'RECOVERED' ? '로그 기록' : 'warn / JSON';
  }
}

function fillSettings() {
  if (!status) return;
  for (const name of ['cpu', 'memory', 'disk']) {
    const rule = status.settings[name];
    byId(`${name}-enabled`).checked = rule.enabled;
    byId(`${name}-limit`).value = rule.thresholdPercent;
    byId(`${name}-hysteresis`).value = rule.hysteresisPercent;
  }
  byId('sample-interval').value = status.settings.sampleIntervalSeconds;
  byId('consecutive').value = status.settings.consecutiveSamples;
  byId('repeat-interval').value = status.settings.repeatIntervalSeconds;
  byId('disk-paths').value = status.settings.diskPaths.join('\n');
  editingVersion = status.settingsVersion;
  dirty = false;
  byId('settings-status').textContent = `설정 버전 ${status.settingsVersion} · 저장된 설정을 표시 중입니다.`;
}

function render() {
  const snapshot = status.snapshot;
  const stale = snapshot && Date.now() - Date.parse(snapshot.capturedAt) > (status.settings.sampleIntervalSeconds * 2 + 5) * 1000;
  const errors = [...(snapshot?.errors || []), ...(status.lastError ? [status.lastError] : []), ...(stale ? ['측정값 갱신이 지연되고 있습니다.'] : [])];
  byId('collection-errors').textContent = errors.join(' / ');
  byId('collection-errors').hidden = !errors.length;
  byId('connection').textContent = errors.length ? '수집 오류' : snapshot?.cpuPercent == null ? '측정 준비 중' : '모니터링 중';
  byId('connection').className = `connection ${errors.length ? 'error' : 'online'}`;
  if (snapshot) {
    byId('hostname').textContent = snapshot.host;
    byId('last-update').textContent = `마지막 측정 ${date(snapshot.capturedAt)}`;
    const diskValue = snapshot.disks.length ? Math.max(...snapshot.disks.map(disk => disk.usedPercent)) : null;
    const diskActive = Object.entries(status.states).some(([key, state]) => key.startsWith('disk:') && state.active);
    renderMetric('cpu', snapshot.cpuPercent, status.states.cpu?.active, status.settings.cpu);
    renderMetric('memory', snapshot.memory?.usedPercent, status.states.memory?.active, status.settings.memory);
    renderMetric('disk', diskValue, diskActive, status.settings.disk);
    byId('memory-capacity').textContent = snapshot.memory ? `${bytes(snapshot.memory.usedBytes)} / ${bytes(snapshot.memory.totalBytes)}` : '측정 불가';
    byId('disk-list').replaceChildren();
    for (const disk of snapshot.disks) {
      const row = document.createElement('div');
      row.className = 'disk-row';
      const path = document.createElement('span');
      path.textContent = disk.path;
      const usage = document.createElement('strong');
      usage.textContent = `${bytes(disk.usedBytes)} / ${bytes(disk.totalBytes)}`;
      row.append(path, usage);
      byId('disk-list').append(row);
    }
    if (snapshot.capturedAt !== lastCaptured) {
      for (const [name, value] of Object.entries({ cpu: snapshot.cpuPercent, memory: snapshot.memory?.usedPercent })) {
        if (value != null) history[name].push(value);
        if (history[name].length > 60) history[name].shift();
        drawChart(name);
      }
      lastCaptured = snapshot.capturedAt;
    }
  }
  renderEvents(status.recentEvents);
  if (!dirty) fillSettings();
  else if (editingVersion !== status.settingsVersion) byId('settings-status').textContent = '다른 화면에서 설정이 변경되었습니다. 다시 불러오기를 눌러 확인하세요.';
  byId('save-settings').disabled = false;
  byId('reload-settings').disabled = false;
}

async function refresh() {
  if (!apiToken || loading) return;
  loading = true;
  try {
    status = await api('/api/status');
    byId('auth-panel').hidden = true;
    byId('disconnect').hidden = false;
    render();
  } catch (error) {
    byId('connection').textContent = '연결 오류';
    byId('connection').className = 'connection error';
    if (error.status === 401) disconnect();
    message(error.message, true);
  } finally {
    loading = false;
  }
}

function disconnect() {
  apiToken = '';
  byId('token').value = '';
  byId('auth-panel').hidden = false;
  byId('disconnect').hidden = true;
  byId('save-settings').disabled = true;
  byId('reload-settings').disabled = true;
  byId('connection').textContent = '연결 해제';
  byId('connection').className = 'connection';
  message('연결이 해제되었습니다. 모니터링 서비스는 계속 동작합니다.');
}

byId('auth-form').addEventListener('submit', async event => {
  event.preventDefault();
  apiToken = byId('token').value.trim();
  message('');
  await refresh();
  byId('token').value = '';
});
byId('settings-form').addEventListener('input', () => {
  dirty = true;
  byId('settings-status').textContent = '아직 저장하지 않은 변경 사항이 있습니다.';
});
byId('reload-settings').addEventListener('click', async () => {
  await refresh();
  fillSettings();
  message('현재 저장된 설정을 불러왔습니다.');
});
byId('disconnect').addEventListener('click', disconnect);
byId('settings-form').addEventListener('submit', async event => {
  event.preventDefault();
  if (!apiToken || editingVersion == null) return;
  const settings = {
    sampleIntervalSeconds: Number(byId('sample-interval').value),
    consecutiveSamples: Number(byId('consecutive').value),
    repeatIntervalSeconds: Number(byId('repeat-interval').value),
    diskPaths: byId('disk-paths').value.split('\n').map(path => path.trim()).filter(Boolean)
  };
  for (const name of ['cpu', 'memory', 'disk']) {
    settings[name] = { enabled: byId(`${name}-enabled`).checked,
      thresholdPercent: Number(byId(`${name}-limit`).value), hysteresisPercent: Number(byId(`${name}-hysteresis`).value) };
    if (settings[name].hysteresisPercent >= settings[name].thresholdPercent) {
      message('복구 여유폭은 임계치보다 작아야 합니다.', true);
      return;
    }
  }
  byId('save-settings').disabled = true;
  try {
    status = await api('/api/settings', { method: 'PUT', body: JSON.stringify({ expectedVersion: editingVersion, settings }) });
    dirty = false;
    render();
    message('설정을 저장했습니다. 다음 측정부터 새 임계치로 판정합니다.');
  } catch (error) {
    message(error.message, true);
  } finally {
    byId('save-settings').disabled = !apiToken;
  }
});
setInterval(refresh, 1000);
