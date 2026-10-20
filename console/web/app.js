'use strict';

const HISTORY = 120;
// BIOS 的适配器门槛：性能要 >180W，狂暴要 >230W
const NEED_WATTS = { 'balanced-performance': 180, performance: 230 };
const LIMIT_UNIT = { TDC: 'A', EDC: 'A', Tctl: '°C' };

const $ = (sel, root = document) => root.querySelector(sel);
const $$ = (sel, root = document) => [...root.querySelectorAll(sel)];

const tiles = {
  cpu: { unit: '°C', digits: 0, get: s => s.cpu_temp },
  power: { unit: 'W', digits: 1, get: s => s.smu?.limits.find(l => l.name === 'PPT Fast')?.value },
  fan1: { unit: 'RPM', digits: 0, get: s => s.fans[0] },
  fan2: { unit: 'RPM', digits: 0, get: s => s.fans[1] },
};
for (const [key, t] of Object.entries(tiles)) {
  t.el = $(`.tile[data-key="${key}"]`);
  t.history = [];
  t.hover = null;
  initSpark(t);
}

let state = null;
let pendingProfile = null;

function connect() {
  const es = new EventSource('api/events');
  es.onmessage = e => {
    document.body.classList.remove('offline');
    state = JSON.parse(e.data);
    render(state);
  };
  es.onerror = () => document.body.classList.add('offline');
}

function render(s) {
  renderSupply(s);
  renderModes(s);
  for (const t of Object.values(tiles)) renderTile(t, t.get(s));
  renderLimits(s.smu?.limits ?? []);
  renderCores(s.smu?.cores ?? []);
  renderLocks(s.locks);
}

function renderSupply(s) {
  let text = '';
  if (s.ac) text = s.adapter_w ? `适配器 ${s.adapter_w}W` : '适配器';
  else if (s.battery) text = `电池 ${s.battery.capacity}% · ${s.battery.power_w.toFixed(1)}W`;
  $('#supply').textContent = text;
}

function renderModes(s) {
  for (const b of $$('#modes button')) {
    const p = b.dataset.profile;
    const need = NEED_WATTS[p];
    b.disabled = s.profile == null || (need != null && (s.ac === false || (s.adapter_w != null && s.adapter_w <= need)));
    b.classList.toggle('active', p === s.profile);
    b.classList.toggle('pending', p === pendingProfile);
  }
}

function renderTile(t, v) {
  t.el.hidden = v == null;
  if (v == null) return;
  t.history.push(v);
  if (t.history.length > HISTORY) t.history.shift();
  setValue($('.value', t.el), v, t);
  drawSpark(t);
}

function setValue(el, v, t) {
  el.textContent = v.toFixed(t.digits);
  const unit = document.createElement('small');
  unit.textContent = t.unit;
  el.append(unit);
}

function initSpark(t) {
  const svg = $('.spark', t.el);
  const ns = 'http://www.w3.org/2000/svg';
  for (const cls of ['area', 'line', 'cross', 'dot']) {
    const el = document.createElementNS(ns, cls === 'cross' ? 'line' : cls === 'dot' ? 'circle' : 'path');
    el.setAttribute('class', cls);
    svg.append(el);
  }
  const tip = document.createElement('span');
  tip.className = 'tip';
  t.el.append(tip);

  svg.addEventListener('pointermove', e => {
    const r = svg.getBoundingClientRect();
    t.hover = (e.clientX - r.left) / r.width;
    drawSpark(t);
  });
  svg.addEventListener('pointerleave', () => {
    t.hover = null;
    drawSpark(t);
  });
}

function drawSpark(t) {
  const svg = $('.spark', t.el);
  const w = svg.clientWidth, h = svg.clientHeight;
  const data = t.history;
  const [area, line, cross, dot] = svg.children;
  const tip = $('.tip', t.el);
  if (data.length < 2 || w === 0) return;

  let lo = Math.min(...data), hi = Math.max(...data);
  if (hi - lo < 1) { hi += 0.5; lo -= 0.5; }
  const step = w / (HISTORY - 1);
  const x0 = w - (data.length - 1) * step;
  const x = i => x0 + i * step;
  const y = v => 2 + (h - 4) * (1 - (v - lo) / (hi - lo));

  const pts = data.map((v, i) => `${x(i).toFixed(1)},${y(v).toFixed(1)}`);
  line.setAttribute('d', 'M' + pts.join('L'));
  area.setAttribute('d', `M${x0},${h}L${pts.join('L')}L${w},${h}Z`);

  let i = data.length - 1;
  if (t.hover != null) i = Math.round((t.hover * w - x0) / step);
  const show = t.hover != null && i >= 0 && i < data.length;
  cross.style.display = dot.style.display = show ? '' : 'none';
  tip.replaceChildren();
  if (!show) return;

  const cx = x(i), cy = y(data[i]);
  cross.setAttribute('x1', cx); cross.setAttribute('x2', cx);
  cross.setAttribute('y1', 0); cross.setAttribute('y2', h);
  dot.setAttribute('cx', cx); dot.setAttribute('cy', cy); dot.setAttribute('r', 4);
  const val = document.createElement('b');
  val.textContent = data[i].toFixed(t.digits) + t.unit;
  tip.append(val, ` · ${data.length - 1 - i}s 前`);
}

function renderLimits(limits) {
  const box = $('#limits');
  box.closest('.card').hidden = limits.length === 0;
  if (box.children.length !== limits.length) {
    box.replaceChildren(...limits.map(() => {
      const row = document.createElement('div');
      row.className = 'limit';
      row.innerHTML = '<span class="name"></span><div class="meter"><i></i></div><span class="num"></span>';
      return row;
    }));
  }
  limits.forEach((l, i) => {
    const row = box.children[i];
    const ratio = l.limit > 0 ? Math.min(l.value / l.limit, 1) : 0;
    const unit = LIMIT_UNIT[l.name] ?? 'W';
    $('.name', row).textContent = l.name;
    $('.meter i', row).style.width = `${ratio * 100}%`;
    $('.meter', row).classList.toggle('hot', ratio >= 0.9 && ratio < 0.99);
    $('.meter', row).classList.toggle('max', ratio >= 0.99);
    const num = $('.num', row);
    num.textContent = l.value.toFixed(1);
    const lim = document.createElement('span');
    lim.textContent = ` / ${l.limit.toFixed(0)}${unit}`;
    num.append(lim);
  });
}

function renderCores(cores) {
  const body = $('#cores tbody');
  $('#cores').closest('.card').hidden = cores.length === 0;
  if (body.children.length !== cores.length) {
    body.replaceChildren(...cores.map((_, i) => {
      const tr = document.createElement('tr');
      tr.innerHTML = `<td>${i}</td><td></td><td></td><td></td><td></td>`;
      return tr;
    }));
  }
  cores.forEach((c, i) => {
    const cells = body.children[i].children;
    cells[1].textContent = c.freq.toFixed(2);
    cells[2].textContent = c.volt.toFixed(3);
    cells[3].textContent = c.temp.toFixed(0);
    cells[4].textContent = c.power.toFixed(2);
  });
}

function renderLocks(locks) {
  const map = { touchpad_lock: 'touchpad', fn_lock: 'fn', win_lock: 'win' };
  $('.locks').hidden = locks == null;
  if (locks == null) return;
  for (const input of $$('.locks input')) {
    if (!input.matches(':disabled')) input.checked = locks[map[input.dataset.lock]];
  }
}

async function put(url, body) {
  const res = await fetch(url, {
    method: 'PUT',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
  });
  if (!res.ok) throw new Error((await res.text()) || res.statusText);
}

let toastTimer;
function toast(msg) {
  const el = $('#toast');
  el.textContent = msg;
  el.classList.add('show');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => el.classList.remove('show'), 3000);
}

$('#modes').addEventListener('click', async e => {
  const b = e.target.closest('button');
  if (!b || b.disabled || b.classList.contains('active')) return;
  pendingProfile = b.dataset.profile;
  if (state) renderModes(state);
  try {
    await put('api/profile', { profile: pendingProfile });
  } catch (err) {
    toast(`切换失败：${err.message}`);
  } finally {
    pendingProfile = null;
    if (state) renderModes(state);
  }
});

for (const input of $$('.locks input')) {
  input.addEventListener('change', async () => {
    input.disabled = true;
    try {
      await put(`api/locks/${input.dataset.lock}`, { on: input.checked });
    } catch (err) {
      input.checked = !input.checked;
      toast(`设置失败：${err.message}`);
    } finally {
      input.disabled = false;
    }
  });
}

connect();
