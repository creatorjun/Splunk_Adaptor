// src/main/resources/web/app.js
const byId = id => document.getElementById(id);
let apiToken = '';
let status = null;
let dirty = false;
let editingVersion = null;
let loading = false;
const POLL_INTERVAL_MS = 60000;
let minuteHistory = [];
let historyError = null;
let nextRefreshAt = 0;
let pollTimer = null;
let hoveredPoint = null;
let chartGeometry = null;
const chartSeries = [
  { key: 'cpuPercent', label: 'CPU', color: '#0e8072', visible: true },
  { key: 'memoryPercent', label: 'Memory', color: '#6175ce', visible: true },
  { key: 'diskPercent', label: 'Disk', color: '#d69538', visible: true }
];
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

function drawChart() {
  const canvas = byId('usage-chart');
  const ctx = canvas.getContext('2d');
  const width = canvas.clientWidth;
  const height = canvas.clientHeight;
  if (!width || !height) return;
  const scale = window.devicePixelRatio || 1;
  canvas.width = Math.round(width * scale);
  canvas.height = Math.round(height * scale);
  ctx.scale(scale, scale);
  ctx.clearRect(0, 0, width, height);
  const left = 47, top = 16, right = width - 22, bottom = height - 32;
  const end = Math.max(Date.now(), ...minuteHistory.map(point => Date.parse(point.capturedAt)));
  const start = minuteHistory.length > 1 ? Math.max(end - 3600000, Date.parse(minuteHistory[0].capturedAt)) : end - 60000;
  const x = time => left + (time - start) / Math.max(60000, end - start) * (right - left);
  const y = value => bottom - value / 100 * (bottom - top);
  chartGeometry = { x, y, left, right, top, bottom };
  ctx.font = '11px "Segoe UI", "Malgun Gothic", sans-serif';
  ctx.textAlign = 'right';
  ctx.textBaseline = 'middle';
  for (let value = 0; value <= 100; value += 20) {
    ctx.strokeStyle = '#edf1f5';
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(left, y(value));
    ctx.lineTo(right, y(value));
    ctx.stroke();
    ctx.fillStyle = '#8492a2';
    ctx.fillText(`${value}%`, left - 10, y(value));
  }
  ctx.textAlign = 'center';
  let lastLabel = null;
  for (let index = 0; index <= 4; index++) {
    const time = start + Math.max(60000, end - start) * index / 4;
    const label = new Date(time).toLocaleTimeString('ko-KR', { hour: '2-digit', minute: '2-digit', hour12: false });
    if (label !== lastLabel) ctx.fillText(label, left + (right - left) * index / 4, height - 12);
    lastLabel = label;
  }
  for (const series of chartSeries.filter(series => series.visible)) {
    ctx.strokeStyle = series.color;
    ctx.lineWidth = 2.2;
    ctx.lineJoin = 'round';
    ctx.beginPath();
    let previous = null;
    for (const point of minuteHistory) {
      const time = Date.parse(point.capturedAt);
      const value = point[series.key];
      if (value == null) { previous = null; continue; }
      if (previous == null || time - previous > 90000) ctx.moveTo(x(time), y(value));
      else ctx.lineTo(x(time), y(value));
      previous = time;
    }
    ctx.stroke();
    for (const point of minuteHistory) {
      if (point[series.key] == null) continue;
      ctx.beginPath();
      ctx.arc(x(Date.parse(point.capturedAt)), y(point[series.key]), minuteHistory.length < 10 ? 3 : 2, 0, Math.PI * 2);
      ctx.fillStyle = series.color;
      ctx.fill();
    }
  }
  if (hoveredPoint && minuteHistory.includes(hoveredPoint)) {
    const px = x(Date.parse(hoveredPoint.capturedAt));
    ctx.beginPath();
    ctx.setLineDash([4, 4]);
    ctx.moveTo(px, top);
    ctx.lineTo(px, bottom);
    ctx.strokeStyle = '#b8c4d0';
    ctx.lineWidth = 1;
    ctx.stroke();
    ctx.setLineDash([]);
  }
  byId('chart-empty').hidden = minuteHistory.length > 0;
  byId('history-status').textContent = historyError ? `분별 기록 오류: ${historyError}`
    : minuteHistory.length ? `${minuteHistory.length}개 측정값 · 마지막 기록 ${date(minuteHistory.at(-1).capturedAt)}`
    : '첫 측정 준비 중입니다. 기록은 브라우저를 닫아도 서버에서 계속 수집됩니다.';
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
  const errors = [...(snapshot?.errors || []), ...(status.lastError ? [status.lastError] : []), ...(historyError ? [`분별 기록: ${historyError}`] : []), ...(stale ? ['측정값 갱신이 지연되고 있습니다.'] : [])];
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
  }
  drawChart();
  renderEvents(status.recentEvents);
  if (!dirty) fillSettings();
  else if (editingVersion !== status.settingsVersion) byId('settings-status').textContent = '다른 화면에서 설정이 변경되었습니다. 다시 불러오기를 눌러 확인하세요.';
  byId('save-settings').disabled = false;
  byId('reload-settings').disabled = false;
}

async function refresh() {
  if (!apiToken || loading) return;
  clearTimeout(pollTimer);
  loading = true;
  try {
    const [latestStatus, history] = await Promise.all([api('/api/status'), api('/api/history')]);
    status = latestStatus;
    minuteHistory = history.points;
    historyError = history.lastError;
    hoveredPoint = null;
    byId('chart-tooltip').hidden = true;
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
    if (apiToken) {
      nextRefreshAt = Date.now() + POLL_INTERVAL_MS;
      pollTimer = setTimeout(refresh, POLL_INTERVAL_MS);
    }
  }
}

function disconnect() {
  clearTimeout(pollTimer);
  apiToken = '';
  status = null;
  dirty = false;
  editingVersion = null;
  nextRefreshAt = 0;
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
for (const button of document.querySelectorAll('[data-series]')) {
  button.addEventListener('click', () => {
    const series = chartSeries.find(series => series.key === button.dataset.series);
    series.visible = !series.visible;
    button.setAttribute('aria-pressed', String(series.visible));
    drawChart();
  });
}
byId('usage-chart').addEventListener('pointermove', event => {
  if (!minuteHistory.length || !chartGeometry) return;
  const bounds = event.currentTarget.getBoundingClientRect();
  const px = event.clientX - bounds.left;
  hoveredPoint = minuteHistory.reduce((nearest, point) =>
    Math.abs(chartGeometry.x(Date.parse(point.capturedAt)) - px) < Math.abs(chartGeometry.x(Date.parse(nearest.capturedAt)) - px) ? point : nearest);
  const tooltip = byId('chart-tooltip');
  tooltip.replaceChildren();
  const title = document.createElement('strong');
  title.textContent = date(hoveredPoint.capturedAt);
  tooltip.append(title);
  for (const series of chartSeries.filter(series => series.visible)) {
    const row = document.createElement('div');
    row.textContent = `${series.label}: ${percent(hoveredPoint[series.key])}%`;
    row.style.color = series.color;
    tooltip.append(row);
  }
  if (hoveredPoint.errors.length) {
    const error = document.createElement('small');
    error.textContent = hoveredPoint.errors.join(' / ');
    tooltip.append(error);
  }
  tooltip.hidden = false;
  tooltip.style.left = `${Math.min(bounds.width - tooltip.offsetWidth - 8, Math.max(8, px + 12))}px`;
  tooltip.style.top = '12px';
  drawChart();
});
byId('usage-chart').addEventListener('pointerleave', () => {
  hoveredPoint = null;
  byId('chart-tooltip').hidden = true;
  drawChart();
});
new ResizeObserver(drawChart).observe(byId('usage-chart'));
setInterval(() => {
  byId('poll-countdown').textContent = apiToken && nextRefreshAt ? `다음 조회 ${Math.max(0, Math.ceil((nextRefreshAt - Date.now()) / 1000))}초` : '1분 간격';
}, 1000);
