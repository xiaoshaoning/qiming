// Qiming Waveform Viewer
const { invoke } = window.__TAURI__?.core ?? {};
let sessionId = null;
let signalNames = [];
let checkedSignals = new Set();
const canvas = document.getElementById('waveform-canvas');
const ctx = canvas.getContext('2d');
const ROW_H = 22, LABEL_W = 180;
function draw() {
  const rows = [...checkedSignals].filter(s => signalNames.includes(s));
  const H = Math.max(rows.length * ROW_H + 30, 200);
  canvas.width = canvas.parentElement.clientWidth;
  canvas.height = H;
  ctx.fillStyle = '#1a1a2e';
  ctx.fillRect(0, 0, canvas.width, canvas.height);
  ctx.strokeStyle = '#0f3460';
  ctx.lineWidth = 0.5;
  for (let x = LABEL_W; x < canvas.width; x += 100) {
    ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, H); ctx.stroke();
  }
  ctx.fillStyle = '#e0e0e0';
  ctx.font = '11px monospace';
  for (let r = 0; r < rows.length; r++)
    ctx.fillText(rows[r], 4, 15 + r * ROW_H + ROW_H / 2 + 4);
}
async function tauriCall(cmd, args) {
  if (window.__TAURI__?.core?.invoke)
    return await window.__TAURI__.core.invoke(cmd, args);
  try {
    const resp = await fetch('http://localhost:9876/mcp', {
      method: 'POST', headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({jsonrpc:'2.0', id:1, method: cmd.replace('cmd_',''), params:args})
    });
    return await resp.json();
  } catch(e) { return null; }
}
async function refreshWave() {
  draw();
  if (checkedSignals.size === 0 || !sessionId) return;
  try {
    const resp = await tauriCall('cmd_query_wave', { sessionId });
    const entries = resp?.result?.entries || resp?.entries || [];
    const rows = [...checkedSignals].filter(s => signalNames.includes(s));
    for (let r = 0; r < rows.length; r++) {
      const sig = rows[r];
      const sigEntries = entries.filter(e => e.signal === sig);
      let prevX = LABEL_W, prevV = 'X';
      for (const e of sigEntries) {
        const v = e.value || 'X';
        const color = v === '0' ? '#4ecca3' : v === '1' ? '#e94560' : v === 'Z' ? '#36a' : '#888';
        ctx.fillStyle = color;
        const y = 15 + r * ROW_H;
        const x = LABEL_W + (e.time_fs || 0) * 0.01;
        ctx.fillRect(prevX, y + 2, Math.max(x - prevX, 1), ROW_H - 4);
        prevV = v; prevX = x;
      }
      ctx.fillStyle = prevV === '0' ? '#4ecca3' : '#888';
      ctx.fillRect(prevX, 15 + r * ROW_H + 2, canvas.width - prevX, ROW_H - 4);
    }
  } catch(e) { console.error(e); }
}
function setStatus(s) {
  document.getElementById('status').textContent = s;
  document.getElementById('status').className = s === 'running' ? 'running' : s === 'error' ? 'error' : '';
}
async function compile() {
  const src = document.getElementById('src-editor').value.trim();
  if (!src) return;
  setStatus('compiling...');
  try {
    const r = await tauriCall('cmd_session_compile', { source: src, name: 'top' });
    const data = r?.result || r;
    if (!data?.session_id) throw new Error('compile failed');
    sessionId = data.session_id;
    document.getElementById('compile-msg').innerHTML = 'Compiled OK';
    setStatus('compiled');
  } catch(e) {
    document.getElementById('compile-msg').innerHTML = 'Error: ' + e;
    setStatus('error');
  }
}
async function elaborate() {
  if (!sessionId) return;
  setStatus('elaborating...');
  try {
    await tauriCall('cmd_session_elaborate', { sessionId });
    document.getElementById('compile-msg').innerHTML = 'Elaborated OK';
    await refreshSignals();
    setStatus('elaborated');
  } catch(e) {
    document.getElementById('compile-msg').innerHTML = 'Error: ' + e;
    setStatus('error');
  }
}
async function refreshSignals() {
  if (!sessionId) return;
  try {
    const r = await tauriCall('cmd_session_get_signals', { sessionId });
    const data = r?.result || r;
    signalNames = data.signals || [];
    const container = document.getElementById('signal-list');
    const max = parseInt(document.getElementById('max-signals').value) || 32;
    const d = signalNames.slice(0, max);
    container.innerHTML = d.map(s => {
      const ck = checkedSignals.has(s) ? 'checked' : '';
      return '<label><input type="checkbox" '+ck+' data-sig="'+s+'"/>'+s+'</label>';
    }).join('');
    if (signalNames.length > max)
      container.innerHTML += '<em>...'+(signalNames.length-max)+' more</em>';
    container.querySelectorAll('input').forEach(cb => {
      cb.onchange = () => {
        if (cb.checked) checkedSignals.add(cb.dataset.sig);
        else checkedSignals.delete(cb.dataset.sig);
        refreshWave();
      };
    });
  } catch(e) { console.error(e); }
}
async function stepDelta() {
  if (!sessionId) return;
  setStatus('running');
  try { await tauriCall('cmd_session_simulate', { sessionId }); await refreshWave(); }
  catch(e) { setStatus('error'); }
}
async function runN(n) {
  setStatus('running');
  for (let i = 0; i < n; i++) {
    try { await tauriCall('cmd_session_simulate', { sessionId }); } catch(e) { break; }
  }
  await refreshWave();
  setStatus('idle');
}

function loadExample() { var L = String.fromCharCode(10); var v = [ "module counter(input clk, input rst, output reg [3:0] count);", "  always @(posedge clk or posedge rst)", "    if (rst) count <= 0;", "    else count <= count + 1;", "endmodule" ]; document.getElementById("src-editor").value = v.join(L);}

window.addEventListener('DOMContentLoaded', () => {
  document.getElementById('btn-compile').onclick = compile;
  document.getElementById('btn-elaborate').onclick = elaborate;
  document.getElementById('btn-step').onclick = stepDelta;
  document.getElementById('btn-run10').onclick = () => runN(10);
  document.getElementById('btn-run100').onclick = () => runN(100);
  document.getElementById('btn-reset').onclick = () => {
    sessionId = null; checkedSignals.clear(); signalNames = [];
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    document.getElementById('signal-list').innerHTML = 'Compile and elaborate to see signals';
    document.getElementById('compile-msg').innerHTML = '';
    setStatus('idle');
  };
  document.getElementById('btn-load-example').onclick = loadExample;
  document.getElementById('btn-refresh-signals').onclick = refreshSignals;
  document.getElementById('max-signals').onchange = refreshSignals;
  new ResizeObserver(() => refreshWave()).observe(document.getElementById('waveform-container'));
  draw();
});
