'use strict';

/* ============================================================
   QUBIC EXPLORER — frontend
   Crypto-terminal aesthetic. Hash router + 1s overview polling.
   ============================================================ */

// ---------- format helpers ----------
const fmt = {
  n: v => (v == null || v === '' ? '—' : (''+v).replace(/\B(?=(\d{3})+(?!\d))/g, ',')),
  truncId: id => id ? (id.slice(0, 8) + '…' + id.slice(-6)) : '',
  esc: s => (''+s).replace(/[&<>"]/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c])),
  // Render a 60-char identity as a link to its detail page. trunc=false shows full.
  idLink: (id, trunc = true) => {
    if (!id) return '<span class="muted">—</span>';
    const e = fmt.esc(id);
    const label = trunc ? fmt.truncId(id) : fmt.esc(id);
    return `<a class="link id" href="#/id/${e}" title="${e}">${label}</a>`;
  },
  // base64 → hex string
  b64ToHex: b64 => {
    if (!b64) return '';
    try {
      const bin = atob(b64);
      let h = '';
      for (let i = 0; i < bin.length; i++) h += bin.charCodeAt(i).toString(16).padStart(2, '0');
      return h.toUpperCase();
    } catch (e) { return ''; }
  },
  hexDump: hex => {
    if (!hex) return '';
    const per = 32;
    const out = [];
    for (let i = 0; i < hex.length; i += per * 2) {
      out.push(hex.slice(i, i + per * 2));
    }
    return out.join('\n');
  },
};

// ---------- API ----------
const post = (url, body) => fetch(url, {
  method: 'POST',
  headers: {'Content-Type': 'application/json'},
  body: JSON.stringify(body),
}).then(r => r.ok ? r.json() : Promise.reject(new Error('HTTP ' + r.status)));

// NOTE: backend field names per rpc_queryv2_controller.h
//   getTickData      requires {tickNumber}
//   getTransactionByHash requires {hash}
//   getTransactionsForTick requires {tickNumber}
const api = {
  data:       ()       => fetch('/explorer/data').then(r => r.json()),
  tickData:   tick     => post('/query/v1/getTickData',           {tickNumber: tick}),
  tx:         (hash, tick) => post('/query/v1/getTransactionByHash', tick != null ? {hash, tickNumber: tick} : {hash}),
  txsForTick: tick     => post('/query/v1/getTransactionsForTick',{tickNumber: tick}),
  logs:       (offset, size, filters) => post('/query/v1/getEventLogs', {
    pagination: {offset: offset|0, size: size|0},
    ...(filters || {}),
  }),
  balance:    id       => fetch(`/live/v1/balances/${encodeURIComponent(id)}`).then(r => r.json()),
  transfers:  (id, lim) => post('/query/v1/getTransfersForIdentity', {identity: id, direction: 'both', limit: lim || 50}),
  contractCalls: opts   => post('/query/v1/getContractCalls', opts),
  votes:      tick     => post('/query/v1/getVotesForTick', {tickNumber: tick}),
};

// ---------- DOM ----------
const $view = () => document.getElementById('view');
const $led  = () => document.getElementById('conn-led');
const $cn   = () => document.getElementById('conn-text');
function setConn(ok, label) {
  $led().classList.toggle('dead', !ok);
  $cn().classList.toggle('dead', !ok);
  $cn().textContent = label || (ok ? 'LIVE' : 'OFFLINE');
}

// ---------- theme ----------
// Each theme is a {primary, secondary} accent pair; the rest of the palette
// (bg, text, semantic colors) stays fixed. Picker swaps these CSS vars + glows.
const THEMES = [
  { name: 'Bitcoin',   primary: '#f7931a', secondary: '#00d4ff' },
  { name: 'Synthwave', primary: '#ff5cf0', secondary: '#7c5cff' },
  { name: 'Matrix',    primary: '#22ee9a', secondary: '#9bff66' },
  { name: 'Inferno',   primary: '#ff5630', secondary: '#ffb547' },
  { name: 'Arctic',    primary: '#38bdf8', secondary: '#818cf8' },
];

function hexToRgba(hex, a) {
  const h = hex.replace('#', '');
  const r = parseInt(h.slice(0, 2), 16);
  const g = parseInt(h.slice(2, 4), 16);
  const b = parseInt(h.slice(4, 6), 16);
  return `rgba(${r},${g},${b},${a})`;
}

function applyTheme(t) {
  const s = document.documentElement.style;
  s.setProperty('--orange', t.primary);
  s.setProperty('--cyan', t.secondary);
  s.setProperty('--glow-orange', `0 0 12px ${hexToRgba(t.primary, 0.55)}, 0 0 32px ${hexToRgba(t.primary, 0.20)}`);
  s.setProperty('--glow-cyan',   `0 0 12px ${hexToRgba(t.secondary, 0.55)}, 0 0 32px ${hexToRgba(t.secondary, 0.18)}`);
  s.setProperty('--bg-glow-1', hexToRgba(t.primary, 0.06));
  s.setProperty('--bg-glow-2', hexToRgba(t.secondary, 0.05));
  try { localStorage.setItem('explorer-theme', t.name); } catch (e) {}
}

function buildThemePicker() {
  const host = document.getElementById('theme-picker');
  if (!host) return;
  let savedName = null;
  try { savedName = localStorage.getItem('explorer-theme'); } catch (e) {}
  const active = THEMES.find(t => t.name === savedName) || THEMES[0];
  applyTheme(active);
  host.innerHTML = THEMES.map(t => `
    <button class="swatch ${t.name === active.name ? 'active' : ''}" title="${t.name}" data-theme="${t.name}"
            style="background:linear-gradient(135deg, ${t.primary} 0 50%, ${t.secondary} 50% 100%)"></button>`).join('');
  host.querySelectorAll('.swatch').forEach(btn => {
    btn.addEventListener('click', () => {
      const t = THEMES.find(x => x.name === btn.dataset.theme);
      if (!t) return;
      applyTheme(t);
      host.querySelectorAll('.swatch').forEach(b => b.classList.toggle('active', b === btn));
    });
  });
}
document.addEventListener('DOMContentLoaded', buildThemePicker);

// ---------- router ----------
let pollTimer = null;
function stopPoll() { if (pollTimer) { clearInterval(pollTimer); pollTimer = null; } }
function startPoll(fn) { stopPoll(); fn(); pollTimer = setInterval(fn, 1000); }

function route() {
  stopPoll();
  const h = location.hash || '#/';
  const m = h.match(/^#\/(tick|tx|id|contracts|contract|logs|computors)?\/?(.*)$/);
  const view = m && m[1] ? m[1] : '';
  const arg  = m && m[2] ? m[2] : '';

  document.querySelectorAll('.topbar nav a').forEach(a => {
    a.classList.toggle('active', a.getAttribute('href') === (location.hash || '#/'));
  });

  switch (view) {
    case 'tick':      return renderTick(parseInt(arg, 10));
    case 'tx': {
      // arg can be "hash" or "hash/tick" — tick bounds the backend scan to one tick.
      const parts = (arg || '').split('/');
      const t = parts[1] ? (parseInt(parts[1], 10) || null) : null;
      return renderTx(parts[0], t);
    }
    case 'id':        return renderIdentity(arg);
    case 'contracts': return renderContracts(parseInt(arg, 10) || 0);
    case 'contract':  return renderContract(parseInt(arg, 10));
    case 'logs': {
      // arg can be "page" or "page/tickFilter"
      const parts = (arg || '').split('/');
      const p = parseInt(parts[0], 10) || 0;
      const t = parts[1] ? (parseInt(parts[1], 10) || null) : null;
      return renderLogs(p, t);
    }
    case 'computors': return renderComputors();
    default:          return startPoll(renderOverview);
  }
}
window.addEventListener('hashchange', route);
document.addEventListener('DOMContentLoaded', route);

// ---------- known contracts ----------
// Fallback table mirrors src/contract_core/contract_def.h:contractDescriptions[].
// Backend exposes the live list via /query/v1/getContracts — we fetch it at
// boot and replace this array. Fallback is here so the UI still labels things
// correctly if the endpoint is unavailable for any reason.
let KNOWN_CONTRACTS = [
  '', 'QX', 'QTRY', 'RANDOM', 'QUTIL', 'MLM',
  'GQMPROP', 'SWATCH', 'CCF', 'QEARN', 'QVAULT',
  'MSVAULT', 'QBAY', 'QSWAP', 'NOST', 'QDRAW',
  'RL', 'QBOND', 'QIP', 'QRAFFLE', 'QRWA',
  'QRP', 'QTF', 'QDUEL', 'PULSE', 'VOTTUN',
  'QUSINO', 'ESCROW',
];
let KNOWN_CONTRACTS_META = []; // [{index, name, constructionEpoch, destructionEpoch, stateSize}, ...]

async function loadContractList() {
  try {
    const r = await fetch('/query/v1/getContracts').then(r => r.json());
    if (r && Array.isArray(r.contracts)) {
      KNOWN_CONTRACTS_META = r.contracts;
      const arr = [];
      for (const c of r.contracts) arr[c.index] = c.name || '';
      KNOWN_CONTRACTS = arr;
    }
  } catch (e) { /* keep fallback */ }
}
loadContractList(); // fire-and-forget at module load

// Pretty contract label by index. Falls back to "#N" when unknown.
function contractLabel(idx) {
  if (idx == null) return '';
  const name = KNOWN_CONTRACTS[idx];
  if (name === undefined) return '#' + idx;
  if (name === '')        return 'NATIVE #0';
  return name;
}

// Identity encoding (four_q.h:1825): four 8-byte chunks → 14 chars each
// (char = byte % 26 + 'A'). Contract pubkeys are m256i(idx, 0, 0, 0), so
// chunks 1-3 are zero → identity chars 14..55 are all 'A'. Checksum at 56..59.
function destLooksLikeContract(idStr) {
  if (!idStr || idStr.length !== 60) return false;
  const s = idStr.toUpperCase();
  for (let i = 14; i < 56; i++) if (s[i] !== 'A') return false;
  return true;
}

// Decode chunk-0 (first 14 chars) of a contract identity back to its index.
// Safe up to ~2^53 (well past any plausible contract count).
function contractIdxFromId(idStr) {
  if (!idStr || idStr.length < 14) return null;
  const s = idStr.toUpperCase();
  let idx = 0, mul = 1;
  for (let j = 0; j < 14; j++) {
    const c = s.charCodeAt(j) - 65;
    if (c < 0 || c > 25) return null;
    idx += c * mul;
    mul *= 26;
    if (idx > 1e15) break;
  }
  return idx;
}

// Dest cell HTML: identity link, plus a contract-name badge when dest is a contract pubkey.
function destCell(id) {
  if (!destLooksLikeContract(id)) return fmt.idLink(id);
  const idx = contractIdxFromId(id);
  return `${fmt.idLink(id)} <span class="badge info">${fmt.esc(contractLabel(idx))} (#${idx})</span>`;
}

// Render the node-state digest panel (base64 digests + uint32 resource-test digests).
function renderStateBlock(s) {
  const dig = (key, label, tone) => {
    const v = s[key] || '';
    const hex = fmt.b64ToHex(v);
    return `
      <div class="state-row">
        <div class="state-label">${fmt.esc(label)}</div>
        <pre class="hex" style="font-size:0.88em;color:var(--${tone});margin:0">${fmt.esc(hex || '—')}</pre>
      </div>`;
  };
  const num = (key, label) => `
    <div class="state-row">
      <div class="state-label">${fmt.esc(label)}</div>
      <div class="mono" style="color:var(--cyan)">${fmt.n(s[key])}</div>
    </div>`;
  return `
    <div class="state-grid">
      <div class="state-col">
        <h3 class="state-h">SALTED (CURRENT)</h3>
        ${dig('saltedSpectrumDigest', 'SPECTRUM',  'orange')}
        ${dig('saltedUniverseDigest', 'UNIVERSE',  'cyan')}
        ${dig('saltedComputerDigest', 'COMPUTER',  'green')}
        ${num('saltedResourceTestingDigest', 'RESOURCE TESTING')}
        ${num('saltedTransactionBodyDigest', 'TX BODY')}
      </div>
      <div class="state-col">
        <h3 class="state-h">PREVIOUS (LAST TICK)</h3>
        ${dig('prevSpectrumDigest', 'SPECTRUM',  'orange')}
        ${dig('prevUniverseDigest', 'UNIVERSE',  'cyan')}
        ${dig('prevComputerDigest', 'COMPUTER',  'green')}
        ${num('prevResourceTestingDigest', 'RESOURCE TESTING')}
        ${num('prevTransactionBodyDigest', 'TX BODY')}
      </div>
    </div>
    <div class="state-footnote muted">resourceTestingDigest (live) ${fmt.n(s.resourceTestingDigest)}</div>
  `;
}

// ---------- overview ----------
let lastTick = null;
async function renderOverview() {
  let r;
  try { r = await api.data(); setConn(true, 'LIVE'); }
  catch (e) { setConn(false, 'OFFLINE'); $view().innerHTML = '<div class="empty-state">_connection lost_</div>'; return; }
  const h = r.header || {};

  const quorumMet = (h.alignedVotes || 0) >= 451;
  const quorumClass = `value quorum${quorumMet ? ' met' : ''}`;
  const modeClass = h.mainAuxStatus ? 'value mode' : 'value mode aux';

  // hero block
  const ticksTbl = (r.recentTicks || []).slice().reverse().map(t => `
    <tr>
      <td><a class="link" href="#/tick/${t.tick}">${fmt.n(t.tick)}</a></td>
      <td>${fmt.idLink(t.leader)}</td>
      <td class="num">${t.txCount}</td>
      <td class="muted">${fmt.esc(t.timestamp || '—')}</td>
      <td><span class="badge ${t.empty ? 'empty' : 'ok'}">${t.empty ? 'EMPTY' : 'FILLED'}</span></td>
    </tr>`).join('');

  const mp = r.mempool || {perTick: []};
  const mpMax = Math.max(1, ...mp.perTick.map(p => p.count));
  const mempool = mp.perTick.map(p => `
    <div class="spark-row">
      <span class="tick-n"><a class="link" href="#/tick/${p.tick}">${fmt.n(p.tick)}</a></span>
      <span class="count">${p.count}</span>
      <div><div class="bar" style="width:${(p.count / mpMax * 100) | 0}%"></div></div>
    </div>`).join('');

  const net = r.network || {};
  const min = r.mining || {topMiners: []};
  const minersTbl = min.topMiners.map((m, i) => `
    <tr>
      <td class="num">${String(i+1).padStart(2,'0')}</td>
      <td>${fmt.idLink(m.publicKey)}</td>
      <td class="num">${fmt.n(m.score)}</td>
    </tr>`).join('');

  const spec = r.spectrum || {};

  // computor dot-grid: highlight leader of current tick; click → identity detail
  const leaderIdx = (h.tick || 0) % 676;
  const cdots = (r.computors || []).map(c => `
    <a class="cdot" href="#/id/${fmt.esc(c.publicKey)}" data-idx="#${c.index}" data-leader="${c.index === leaderIdx}" title="#${c.index} ${fmt.esc(c.publicKey)}"></a>
  `).join('');

  $view().innerHTML = `
    <section class="hero">
      <div class="hero-tick">
        <div class="tick-number" id="tick-num" data-val="${h.tick || 0}">${fmt.n(h.tick)}</div>
        <dl class="tick-meta">
          <div><dt>EPOCH</dt><dd>${h.epoch ?? '—'}</dd></div>
          <div><dt>INITIAL</dt><dd>${fmt.n(h.initialTick)}</dd></div>
          <div><dt>IN-EPOCH</dt><dd>${fmt.n(h.ticksInCurrentEpoch)}</dd></div>
          <div><dt>LATEST</dt><dd>${fmt.n(h.latestCreatedTick)}</dd></div>
          <div><dt>SNAPSHOT</dt><dd>${h.isSavingSnapshot ? '⊗ saving' : '◌ idle'}</dd></div>
          <div><dt>SUPPLY</dt><dd class="id">${fmt.n(spec.circulatingSupply)} QU</dd></div>
        </dl>
      </div>
      <div class="hero-panel">
        <div class="hero-stat">
          <div class="label">QUORUM</div>
          <div class="${quorumClass}">${h.alignedVotes ?? 0} / 676 ${quorumMet ? '✓' : ''}</div>
        </div>
        <div class="hero-stat">
          <div class="label">MODE</div>
          <div class="${modeClass}">${h.mainAuxStatus ? '◆ MAIN' : '◇ AUX'}</div>
        </div>
        <div class="hero-stat">
          <div class="label">PEERS</div>
          <div class="value cyan">${net.connectedPeers || 0} <span class="muted" style="font-size:0.55em">↑${net.outgoing||0} ↓${net.incoming||0}</span></div>
        </div>
        <div class="hero-stat">
          <div class="label">ENTITIES</div>
          <div class="value cyan">${fmt.n(spec.activeAddresses)}</div>
        </div>
      </div>
    </section>

    <h2 class="section">RECENT TICKS</h2>
    <div class="scroll">
      <table>
        <thead><tr><th>TICK</th><th>LEADER</th><th class="num">TX</th><th>TIMESTAMP</th><th>STATUS</th></tr></thead>
        <tbody>${ticksTbl || '<tr><td colspan=5 class="muted">no ticks</td></tr>'}</tbody>
      </table>
    </div>

    <div class="grid-2" style="margin-top:2em">
      <div>
        <h2 class="section">MEMPOOL · ${fmt.n(mp.totalPending)} PENDING</h2>
        ${mempool || '<div class="empty-state">no pending tx</div>'}
      </div>
      <div>
        <h2 class="section">MINING · ${fmt.n(min.numberOfSolutions)} SOLUTIONS</h2>
        <table>
          <thead><tr><th class="num">#</th><th>MINER</th><th class="num">SCORE</th></tr></thead>
          <tbody>${minersTbl || '<tr><td colspan=3 class="muted">no miners</td></tr>'}</tbody>
        </table>
      </div>
    </div>

    <h2 class="section">NODE STATE · ETALON-TICK DIGESTS</h2>
    ${renderStateBlock(r.state || {})}

    <h2 class="section">COMPUTOR QUORUM · 676 NODES · LEADER #${leaderIdx}</h2>
    <div class="computor-grid">${cdots}</div>
  `;

  // count-up animation on tick number change
  if (lastTick !== null && h.tick !== lastTick) {
    const el = document.getElementById('tick-num');
    if (el) {
      el.style.transition = 'none';
      el.style.transform = 'translateY(-4px)';
      el.style.opacity = '0.6';
      requestAnimationFrame(() => {
        el.style.transition = 'transform 0.2s, opacity 0.2s';
        el.style.transform = 'translateY(0)';
        el.style.opacity = '1';
      });
    }
  }
  lastTick = h.tick;
}

// ---------- tick detail ----------
async function renderTick(n) {
  if (!Number.isFinite(n)) {
    $view().innerHTML = '<div class="empty-state">invalid tick</div>';
    return;
  }
  $view().innerHTML = `<p class="muted">› fetching tick ${fmt.n(n)}…</p>`;

  let td = null, txs = null, err = null;
  try { td = await api.tickData(n); } catch (e) { err = e; }
  try { txs = await api.txsForTick(n); } catch (e) { /* keep td */ }

  // Backend returns 404 with {code:..., message:"Tick data not found"} when empty.
  const notFound = !td || td.code === 404 || td.message;
  if (notFound) {
    $view().innerHTML = `
      <h2 class="section">TICK ${fmt.n(n)}</h2>
      <div class="empty-state">tick is empty or out of range${td && td.message ? ' · '+fmt.esc(td.message) : ''}</div>
      <p style="margin-top:1em"><a class="link" href="#/">‹ back to overview</a></p>
    `;
    return;
  }

  const txList = (txs && Array.isArray(txs.transactions)) ? txs.transactions : [];
  const txRows = txList.map(tx => `
    <tr>
      <td class="id"><a class="link" href="#/tx/${fmt.esc(tx.hash)}/${td.tickNumber ?? n}">${fmt.truncId(tx.hash)}</a></td>
      <td>${fmt.idLink(tx.source)}</td>
      <td>${destCell(tx.destination)}</td>
      <td class="num">${fmt.n(tx.amount)}</td>
      <td class="num">${tx.inputType ?? ''}</td>
      <td class="num">${tx.inputSize ?? ''}</td>
      <td>${tx.hash ? `<button class="logs-btn" onclick="window.__showTxLogs('${fmt.esc(tx.hash)}', ${td.tickNumber ?? n})">◈ LOGS</button>` : ''}</td>
    </tr>`).join('');

  const digests = (td.transactionDigests || []);

  $view().innerHTML = `
    <h2 class="section">TICK ${fmt.n(td.tickNumber ?? n)}</h2>
    <section class="hero" style="grid-template-columns:1fr">
      <div class="hero-tick" style="border-right:none">
        <div style="display:flex;justify-content:space-between;align-items:flex-start;gap:1em">
          <div class="tick-number" style="font-size:clamp(2rem,5vw,3.5rem)">${fmt.n(td.tickNumber ?? n)}</div>
          <button onclick="window.__showVotes(${td.tickNumber ?? n})"
                  style="padding:0.6em 1.25em;background:transparent;color:var(--cyan);
                         border:1px solid var(--cyan);font-family:var(--mono);font-size:0.9em;
                         letter-spacing:0.12em;cursor:pointer;text-transform:uppercase;
                         transition:all 0.15s">
            ◈ VIEW VOTES
          </button>
        </div>
        <dl class="tick-meta">
          <div><dt>EPOCH</dt><dd>${td.epoch ?? '—'}</dd></div>
          <div><dt>LEADER #</dt><dd>${td.computorIndex ?? (n % 676)}</dd></div>
          <div><dt>TIMESTAMP</dt><dd>${fmt.esc(td.timestamp || '—')}</dd></div>
          <div><dt>TX COUNT</dt><dd>${digests.length}</dd></div>
        </dl>
      </div>
    </section>

    <div class="grid-2" style="margin-top:1.5em">
      <div>
        <h2 class="section">TIMELOCK</h2>
        <pre class="hex">${fmt.esc(td.timelock || '—')}</pre>
      </div>
      <div>
        <h2 class="section">SIGNATURE</h2>
        <pre class="hex">${fmt.esc(td.signature || '—')}</pre>
      </div>
    </div>

    <h2 class="section">TRANSACTIONS · ${txList.length}</h2>
    ${txRows ? `<div class="scroll"><table>
      <thead><tr><th>HASH</th><th>SOURCE</th><th>DEST</th><th class="num">AMOUNT</th><th class="num">INTYPE</th><th class="num">INSIZE</th><th>LOGS</th></tr></thead>
      <tbody>${txRows}</tbody>
    </table></div>` : '<div class="empty-state">no transactions in tick</div>'}

    <p style="margin-top:2em"><a class="link" href="#/">‹ back to overview</a></p>
  `;
}

// ---------- transaction detail ----------
async function renderTx(hash, tick = null) {
  if (!hash || hash.length !== 60) {
    $view().innerHTML = '<div class="empty-state">invalid tx hash · need 60-char identity</div>';
    return;
  }
  $view().innerHTML = `<p class="muted">› fetching tx ${fmt.truncId(hash)}…</p>`;

  let tx = null;
  try { tx = await api.tx(hash, tick); } catch (e) {}

  const notFound = !tx || tx.code === 404 || (tx.message && !tx.hash);
  if (notFound) {
    $view().innerHTML = `
      <h2 class="section">TRANSACTION</h2>
      <div class="empty-state">not found · ${fmt.esc(hash)}${tx && tx.message ? ' · '+fmt.esc(tx.message) : ''}</div>
      <p style="margin-top:1em"><a class="link" href="#/">‹ back</a></p>
    `;
    return;
  }

  const isContract = destLooksLikeContract(tx.destination || '');
  const contractIdx = isContract ? contractIdxFromId(tx.destination) : null;
  const dispLabel = isContract && contractIdx != null
    ? `${contractLabel(contractIdx)} (#${contractIdx})`
    : null;

  const inputHex = fmt.b64ToHex(tx.inputData || '');
  const sigHex   = fmt.b64ToHex(tx.signature || '');

  $view().innerHTML = `
    <section class="tx-header">
      <div class="muted" style="font-size:0.7em;letter-spacing:0.25em;margin-bottom:0.5em">TX HASH</div>
      <div class="tx-hash id">${fmt.esc(tx.hash || hash)}</div>
    </section>

    <div class="tx-flow">
      <div class="addr">
        <div class="label">FROM</div>
        <div class="id" style="word-break:break-all">
          <a class="link" href="#/id/${fmt.esc(tx.source || '')}">${fmt.esc(tx.source || '—')}</a>
        </div>
      </div>
      <div class="arrow">═►</div>
      <div class="addr">
        <div class="label">TO ${dispLabel ? `· ${fmt.esc(dispLabel)}` : ''}</div>
        <div class="id" style="word-break:break-all">
          <a class="link" href="#/id/${fmt.esc(tx.destination || '')}">${fmt.esc(tx.destination || '—')}</a>
        </div>
      </div>
    </div>

    <div class="grid-3">
      <div class="tile">
        <div class="label">AMOUNT</div>
        <div class="value green">${fmt.n(tx.amount)} QU</div>
      </div>
      <div class="tile">
        <div class="label">TICK</div>
        <div class="value"><a class="link" href="#/tick/${tx.tickNumber}">${fmt.n(tx.tickNumber)}</a></div>
      </div>
      <div class="tile">
        <div class="label">TIMESTAMP</div>
        <div class="value cyan" style="font-size:1em">${fmt.esc(tx.timestamp || '—')}</div>
      </div>
      <div class="tile">
        <div class="label">INPUT TYPE</div>
        <div class="value cyan">${tx.inputType ?? '—'}</div>
      </div>
      <div class="tile">
        <div class="label">INPUT SIZE</div>
        <div class="value">${tx.inputSize ?? '—'} <span class="muted" style="font-size:0.55em">BYTES</span></div>
      </div>
      <div class="tile">
        <div class="label">MONEY FLEW</div>
        <div class="value ${tx.moneyFlew ? 'green' : ''}">${tx.moneyFlew ? '✓ YES' : '◌ NO'}</div>
      </div>
    </div>

    <h2 class="section">INPUT DATA (${tx.inputSize ?? 0} bytes · hex)</h2>
    ${inputHex ? `<pre class="hex">${fmt.esc(fmt.hexDump(inputHex))}</pre>` : '<div class="empty-state">empty payload</div>'}

    <h2 class="section">SIGNATURE (64 bytes · hex)</h2>
    <pre class="hex">${fmt.esc(fmt.hexDump(sigHex))}</pre>

    <h2 class="section">RAW JSON</h2>
    <pre class="code">${fmt.esc(JSON.stringify(tx, null, 2))}</pre>

    <p style="margin-top:2em"><a class="link" href="#/">‹ back to overview</a></p>
  `;
}

// ---------- identity detail ----------
async function renderIdentity(idRaw) {
  const id = (idRaw || '').toUpperCase().trim();
  if (!id || id.length !== 60) {
    $view().innerHTML = `
      <h2 class="section">IDENTITY LOOKUP</h2>
      ${id ? `<div class="empty-state">need 60-char identity · got ${id.length} char(s)</div>` : ''}
      <div class="toolbar" style="margin-top:1.5em">
        <input id="id-input"
               placeholder="paste 60-char identity (uppercase base26)…"
               onkeydown="if(event.key==='Enter')window.__lookupId()">
        <button onclick="window.__lookupId()">LOOK UP</button>
      </div>`;
    window.__lookupId = () => {
      const v = (document.getElementById('id-input').value || '').trim().toUpperCase();
      if (v.length === 60) location.hash = '#/id/' + v;
    };
    return;
  }
  $view().innerHTML = `<p class="muted">› resolving ${fmt.truncId(id)}…</p>`;

  let bal = null, tr = null;
  try { bal = await api.balance(id); } catch (e) {}
  try { tr  = await api.transfers(id, 50); } catch (e) {}

  const b = (bal && bal.balance) ? bal.balance : null;
  if (!b) {
    $view().innerHTML = `
      <h2 class="section">IDENTITY ${fmt.truncId(id)}</h2>
      <div class="empty-state">no spectrum entry · address never seen on chain</div>
      <p style="margin-top:1em"><a class="link" href="#/">‹ back</a></p>`;
    return;
  }

  const isContract = destLooksLikeContract(id);
  const contractIdx = isContract ? contractIdxFromId(id) : null;
  const dispLabel = isContract && contractIdx != null
    ? `${contractLabel(contractIdx)} (#${contractIdx})` : null;

  const balanceQu = BigInt(b.incomingAmount || '0') - BigInt(b.outgoingAmount || '0');
  const balanceStr = fmt.n(balanceQu.toString());
  const balanceClass = balanceQu > 0n ? 'green' : (balanceQu < 0n ? 'red' : 'cyan');

  const transfers = (tr && Array.isArray(tr.transactions)) ? tr.transactions : [];
  const trRows = transfers.map(tx => {
    const inDir = tx.direction === 'in';
    const peer = inDir ? tx.source : tx.destination;
    return `
      <tr>
        <td><a class="link" href="#/tick/${tx.tickNumber}">${fmt.n(tx.tickNumber)}</a></td>
        <td><span class="badge ${inDir ? 'ok' : 'alert'}">${inDir ? 'IN' : 'OUT'}</span></td>
        <td class="id"><a class="link" href="#/tx/${fmt.esc(tx.hash)}/${tx.tickNumber}">${fmt.truncId(tx.hash)}</a></td>
        <td class="id"><a class="link" href="#/id/${fmt.esc(peer)}" title="${fmt.esc(peer)}">${fmt.truncId(peer)}</a></td>
        <td class="num" style="color:${inDir ? 'var(--green)' : 'var(--red)'}">${inDir ? '+' : '−'}${fmt.n(tx.amount)}</td>
        <td class="num">${tx.inputType ?? ''}</td>
        <td class="muted">${fmt.esc(tx.timestamp || '')}</td>
      </tr>`;
  }).join('');

  $view().innerHTML = `
    <section class="tx-header">
      <div class="muted" style="font-size:0.7em;letter-spacing:0.25em;margin-bottom:0.5em">
        IDENTITY${dispLabel ? ` · ${fmt.esc(dispLabel)}` : ''}
      </div>
      <div class="tx-hash id">${fmt.esc(id)}</div>
    </section>

    <div class="hero" style="grid-template-columns:1.4fr 1fr;margin-top:1.5em">
      <div class="hero-tick">
        <div class="tick-number ${balanceClass}" style="font-size:clamp(2.5rem,6vw,4.5rem);color:var(--${balanceClass === 'green' ? 'green' : balanceClass === 'red' ? 'red' : 'cyan'});text-shadow:var(--glow-${balanceClass === 'green' ? 'green' : balanceClass === 'red' ? 'red' : 'cyan'})">
          ${balanceStr}
        </div>
        <div class="muted" style="letter-spacing:0.3em;font-size:0.7em;margin-top:0.5em">BALANCE · QU</div>
        <dl class="tick-meta">
          <div><dt>VALID FOR TICK</dt><dd>${fmt.n(b.validForTick)}</dd></div>
          <div><dt>LATEST IN TICK</dt><dd>${fmt.n(b.latestIncomingTransferTick) || '—'}</dd></div>
          <div><dt>LATEST OUT TICK</dt><dd>${fmt.n(b.latestOutgoingTransferTick) || '—'}</dd></div>
        </dl>
      </div>
      <div class="hero-panel">
        <div class="hero-stat">
          <div class="label">TOTAL INCOMING (QU)</div>
          <div class="value" style="color:var(--green);text-shadow:var(--glow-green)">+${fmt.n(b.incomingAmount)}</div>
        </div>
        <div class="hero-stat">
          <div class="label">TOTAL OUTGOING (QU)</div>
          <div class="value" style="color:var(--red);text-shadow:var(--glow-red)">−${fmt.n(b.outgoingAmount)}</div>
        </div>
        <div class="hero-stat">
          <div class="label"># INCOMING TX</div>
          <div class="value green">${fmt.n(b.numberOfIncomingTransfers)}</div>
        </div>
        <div class="hero-stat">
          <div class="label"># OUTGOING TX</div>
          <div class="value" style="color:var(--red)">${fmt.n(b.numberOfOutgoingTransfers)}</div>
        </div>
      </div>
    </div>

    <h2 class="section">RECENT TRANSFERS · ${transfers.length} of ${tr && tr.count != null ? tr.count : '?'} (latest first, max 50)</h2>
    ${trRows ? `<div class="scroll"><table>
      <thead><tr>
        <th class="num">TICK</th><th>DIR</th><th>HASH</th><th>PEER</th><th class="num">AMOUNT</th><th class="num">TYPE</th><th>TIME</th>
      </tr></thead>
      <tbody>${trRows}</tbody>
    </table></div>` : '<div class="empty-state">no transfers in recent scan</div>'}

    <p style="margin-top:2em"><a class="link" href="#/">‹ back to overview</a></p>
  `;
}

// ---------- contracts ----------
const CONTRACTS_SCAN_TICKS = 500;  // server-side scan range per call (capped at 1000 backend)
const CONTRACTS_PAGE_SIZE  = 50;   // rows per page

async function renderContracts(page = 0) {
  $view().innerHTML = `<p class="muted">› scanning recent ticks for contract calls…</p>`;
  let head;
  try { head = (await api.data()).header || {}; }
  catch (e) { $view().innerHTML = '<div class="empty-state">offline</div>'; return; }
  const tickN = head.tick || 0;
  const initT = head.initialTick || 0;
  const fromTick = Math.max(initT, tickN - CONTRACTS_SCAN_TICKS + 1);
  const toTick = tickN;

  let resp;
  try {
    resp = await api.contractCalls({
      fromTick, toTick,
      page, pageSize: CONTRACTS_PAGE_SIZE,
    });
  } catch (e) {
    $view().innerHTML = `<div class="empty-state">backend error · ${fmt.esc(e.message || e)}</div>`;
    return;
  }

  const total = resp.total || 0;
  const txs = resp.transactions || [];
  const totalPages = Math.max(1, Math.ceil(total / CONTRACTS_PAGE_SIZE));
  const startIdx = page * CONTRACTS_PAGE_SIZE;

  // count per contract for the summary chips
  const byIdx = {};
  for (const tx of txs) {
    const i = tx.contractIndex;
    byIdx[i] = (byIdx[i] || 0) + 1;
  }
  const chips = Object.entries(byIdx)
    .sort((a, b) => b[1] - a[1])
    .map(([i, n]) => `<span class="badge info">${fmt.esc(contractLabel(parseInt(i,10)))} · ${n}</span>`)
    .join('');

  const rows = txs.map(tx => {
    const label = contractLabel(tx.contractIndex);
    return `
      <tr>
        <td class="num"><a class="link" href="#/tick/${tx.tickNumber}">${fmt.n(tx.tickNumber)}</a></td>
        <td><a class="link" href="#/contract/${tx.contractIndex}"><span class="badge info">${fmt.esc(label)}</span></a></td>
        <td class="id"><a class="link" href="#/tx/${fmt.esc(tx.hash)}/${tx.tickNumber}">${fmt.truncId(tx.hash)}</a></td>
        <td>${fmt.idLink(tx.source)}</td>
        <td>${fmt.idLink(tx.destination)}</td>
        <td class="num">${fmt.n(tx.amount)}</td>
        <td class="num">${tx.inputType ?? ''}</td>
        <td class="num">${tx.inputSize ?? ''}</td>
      </tr>`;
  }).join('');

  const pgPrev = page > 0
    ? `<a class="link" href="#/contracts/${page - 1}">‹ prev page</a>`
    : '<span class="muted">‹ prev page</span>';
  const pgNext = page + 1 < totalPages
    ? `<a class="link" href="#/contracts/${page + 1}">next page ›</a>`
    : '<span class="muted">next page ›</span>';

  $view().innerHTML = `
    <h2 class="section">CONTRACT CALLS · SCAN TICKS ${fmt.n(fromTick)} → ${fmt.n(toTick)} (${fmt.n(toTick-fromTick+1)} ticks)</h2>

    <div style="margin-bottom:1.5em;font-size:1em">
      <div style="margin-bottom:1em">
        <span class="muted" style="letter-spacing:0.12em">TOTAL CALLS</span>
        <strong style="color:var(--orange);margin-left:0.5em;font-size:1.15em">${fmt.n(total)}</strong>
        ${total > 0 ? `<span class="muted" style="margin-left:0.5em">· this page ${startIdx + 1}–${Math.min(startIdx + CONTRACTS_PAGE_SIZE, total)}</span>` : ''}
      </div>
      ${chips ? `<div style="margin-top:0.75em;display:flex;flex-wrap:wrap;gap:0.5em">${chips}</div>` : ''}
    </div>

    ${rows ? `<div class="scroll"><table>
      <thead><tr>
        <th class="num">TICK</th>
        <th>CONTRACT</th>
        <th>HASH</th>
        <th>CALLER</th>
        <th>DEST</th>
        <th class="num">AMOUNT</th>
        <th class="num">INTYPE</th>
        <th class="num">INSIZE</th>
      </tr></thead>
      <tbody>${rows}</tbody>
    </table></div>` : '<div class="empty-state">no contract calls in this scan window</div>'}

    <div style="display:flex;justify-content:space-between;align-items:center;margin-top:1.25em;font-size:0.85em">
      <span>${pgPrev}</span>
      <span class="muted">page ${page + 1} / ${totalPages}</span>
      <span>${pgNext}</span>
    </div>

    <p style="margin-top:2em"><a class="link" href="#/">‹ overview</a></p>
  `;
}

async function renderContract(idx) {
  if (!Number.isFinite(idx)) { $view().innerHTML = '<div class="empty-state">invalid contract index</div>'; return; }
  const label = contractLabel(idx);
  const meta = KNOWN_CONTRACTS_META.find(c => c.index === idx);
  $view().innerHTML = `<p class="muted">› fetching ${fmt.esc(label)} calls…</p>`;

  let head;
  try { head = (await api.data()).header || {}; }
  catch (e) { $view().innerHTML = '<div class="empty-state">offline</div>'; return; }
  const tickN = head.tick || 0;
  const initT = head.initialTick || 0;
  const fromTick = Math.max(initT, tickN - CONTRACTS_SCAN_TICKS + 1);

  let resp;
  try {
    resp = await api.contractCalls({
      fromTick, toTick: tickN, contractIndex: idx,
      page: 0, pageSize: 100,
    });
  } catch (e) {
    $view().innerHTML = `<div class="empty-state">backend error · ${fmt.esc(e.message || e)}</div>`;
    return;
  }

  const txs = resp.transactions || [];
  const rows = txs.map(tx => `
    <tr>
      <td class="num"><a class="link" href="#/tick/${tx.tickNumber}">${fmt.n(tx.tickNumber)}</a></td>
      <td class="id"><a class="link" href="#/tx/${fmt.esc(tx.hash)}/${tx.tickNumber}">${fmt.truncId(tx.hash)}</a></td>
      <td>${fmt.idLink(tx.source)}</td>
      <td class="num">${fmt.n(tx.amount)}</td>
      <td class="num">${tx.inputType ?? ''}</td>
      <td class="num">${tx.inputSize ?? ''}</td>
      <td class="muted">${fmt.esc(tx.timestamp || '')}</td>
    </tr>`).join('');

  $view().innerHTML = `
    <section class="tx-header">
      <div class="muted" style="font-size:0.7em;letter-spacing:0.25em;margin-bottom:0.5em">CONTRACT · #${idx}</div>
      <div class="tx-hash">${fmt.esc(label)}</div>
      ${meta ? `<dl class="tick-meta" style="margin-top:1.25em">
        <div><dt>NAME</dt><dd>${fmt.esc(meta.name)}</dd></div>
        <div><dt>CONSTRUCTION EPOCH</dt><dd>${meta.constructionEpoch}</dd></div>
        <div><dt>DESTRUCTION EPOCH</dt><dd>${meta.destructionEpoch}</dd></div>
        <div><dt>STATE SIZE</dt><dd>${fmt.n(meta.stateSize)} bytes</dd></div>
      </dl>` : ''}
    </section>

    <h2 class="section">CALLS · ${fmt.n(resp.total || 0)} in ticks ${fmt.n(fromTick)} → ${fmt.n(tickN)}</h2>
    ${rows ? `<div class="scroll"><table>
      <thead><tr>
        <th class="num">TICK</th><th>HASH</th><th>CALLER</th>
        <th class="num">AMOUNT</th><th class="num">INTYPE</th><th class="num">INSIZE</th><th>TIME</th>
      </tr></thead>
      <tbody>${rows}</tbody>
    </table></div>` : '<div class="empty-state">no calls in scan window</div>'}

    <p style="margin-top:2em"><a class="link" href="#/contracts">‹ contracts</a></p>
  `;
}

// ---------- modal ----------
function openModal(title, html) {
  let m = document.getElementById('modal-root');
  if (!m) {
    m = document.createElement('div');
    m.id = 'modal-root';
    document.body.appendChild(m);
  }
  m.innerHTML = `
    <div class="modal-backdrop" onclick="window.__closeModal()"></div>
    <div class="modal-panel">
      <div class="modal-header">
        <h3>${fmt.esc(title)}</h3>
        <button class="modal-close" onclick="window.__closeModal()">✕</button>
      </div>
      <div class="modal-body">${html}</div>
    </div>`;
  m.style.display = 'block';
}
window.__closeModal = () => {
  const m = document.getElementById('modal-root');
  if (m) m.style.display = 'none';
};

window.__showVotes = async function(tick) {
  openModal(`Votes · Tick ${fmt.n(tick)}`, '<p class="muted">› loading…</p>');
  let r;
  try { r = await api.votes(tick); }
  catch (e) { document.querySelector('#modal-root .modal-body').innerHTML =
    `<div class="empty-state">error · ${fmt.esc(e.message || e)}</div>`; return; }

  if (!r || r.code) {
    document.querySelector('#modal-root .modal-body').innerHTML =
      `<div class="empty-state">${fmt.esc((r && r.message) || 'no data')}</div>`;
    return;
  }

  const votes = r.votes || [];
  const count = r.count || 0;

  // Salted digests are intentionally per-computor (each = K12(pubkey || etalon.salted...)),
  // so they MUST differ even for aligned votes. Real consensus check = group by the
  // unsalted prev-digest tuple + transactionDigest, which IS identical across honest votes.
  const groupKey = v =>
    (v.prevSpectrumDigest || '') + '|' +
    (v.prevUniverseDigest || '') + '|' +
    (v.prevComputerDigest || '') + '|' +
    (v.transactionDigest  || '');

  const buckets = {};
  for (const v of votes) {
    const k = groupKey(v);
    buckets[k] = buckets[k] || {count: 0, sample: v};
    buckets[k].count++;
  }
  const groups = Object.entries(buckets).sort((a, b) => b[1].count - a[1].count);
  const leaderKey = groups.length ? groups[0][0] : '';

  const summaryHtml = `
    <div style="display:flex;flex-wrap:wrap;gap:0.6em;margin-bottom:1.25em">
      <span class="badge ok">VOTES ${count} / 676</span>
      ${groups.map(([k, g], i) => `
        <span class="badge ${i === 0 ? 'info' : 'alert'}"
              title="prevSpectrum: ${fmt.esc(g.sample.prevSpectrumDigest || '')}\nprevUniverse: ${fmt.esc(g.sample.prevUniverseDigest || '')}\nprevComputer: ${fmt.esc(g.sample.prevComputerDigest || '')}\ntxDigest: ${fmt.esc(g.sample.transactionDigest || '')}">
          ${i === 0 ? 'QUORUM' : 'MISALIGN ' + i} · ${g.count}
        </span>`).join('')}
    </div>`;

  const rows = votes.map(v => {
    const aligned = groupKey(v) === leaderKey;
    return `
      <tr ${aligned ? '' : 'style="background:rgba(255,59,48,0.06)"'}>
        <td class="num">${v.computorIndex}</td>
        <td><span class="badge ${aligned ? 'ok' : 'bad'}">${aligned ? 'ALIGNED' : 'MISALIGNED'}</span></td>
        <td class="muted">${fmt.esc(v.timestamp || '')}</td>
        <td class="id" title="${fmt.esc(v.prevSpectrumDigest)}">${v.prevSpectrumDigest ? v.prevSpectrumDigest.slice(0, 10) + '…' : '—'}</td>
        <td class="id" title="${fmt.esc(v.prevUniverseDigest)}">${v.prevUniverseDigest ? v.prevUniverseDigest.slice(0, 10) + '…' : '—'}</td>
        <td class="id" title="${fmt.esc(v.prevComputerDigest)}">${v.prevComputerDigest ? v.prevComputerDigest.slice(0, 10) + '…' : '—'}</td>
        <td class="id" title="${fmt.esc(v.transactionDigest)}">${v.transactionDigest ? v.transactionDigest.slice(0, 10) + '…' : '—'}</td>
        <td class="num">${v.prevResourceTestingDigest >>> 0}</td>
      </tr>`;
  }).join('');

  document.querySelector('#modal-root .modal-body').innerHTML = `
    ${summaryHtml}
    <p class="muted" style="font-size:0.85em;margin-bottom:1em">
      Alignment is computed from the <strong>unsalted</strong> prev-state tuple
      (prevSpectrum / prevUniverse / prevComputer / transactionDigest).
      Salted-* fields are per-computor K12(pubkey ‖ etalon) and intentionally differ.
    </p>
    ${rows ? `<div class="scroll" style="max-height:60vh"><table>
      <thead><tr>
        <th class="num">COMP #</th><th>STATUS</th><th>TIMESTAMP</th>
        <th>PREV SPECTRUM</th><th>PREV UNIVERSE</th><th>PREV COMPUTER</th>
        <th>TX DIGEST</th><th class="num">PREV RESTEST</th>
      </tr></thead>
      <tbody>${rows}</tbody>
    </table></div>` : '<div class="empty-state">no votes received for this tick</div>'}
  `;
};

// ---------- logs ----------
// logType numeric tag → short label
const LOG_TYPE_LABEL = {
  0: 'QU TRANSFER',
  1: 'ASSET OWN CHG',
  2: 'ASSET POSS CHG',
  3: 'ASSET ISSUANCE',
  4: 'CONTRACT ERR',
  5: 'CONTRACT WARN',
  6: 'CONTRACT INFO',
  7: 'CONTRACT DBG',
  8: 'BURNING',
  9: 'DUST BURN',
  10: 'SPECTRUM STATS',
  11: 'CONTRACT RES DED',
  12: 'ORACLE Q STATUS',
  13: 'ORACLE SUB MSG',
  255: 'CUSTOM MSG',
};
const logTypeLabel = t => (LOG_TYPE_LABEL[t] || ('T' + t));

// Build a structured-payload preview from one event-log entry.
function logPayloadPreview(e) {
  if (e.quTransfer) {
    const q = e.quTransfer;
    return `${fmt.idLink(q.source)}
            <span style="color:var(--orange);margin:0 0.5em">═►</span>
            ${fmt.idLink(q.destination)}
            <span style="color:var(--green);margin-left:0.75em">+${fmt.n(q.amount)} QU</span>`;
  }
  if (e.customMessage) {
    // value is uint64 — may map to a short ascii tag (e.g. STA_EPOC). Decode best-effort.
    let val = e.customMessage.value || '';
    let tag = '';
    try {
      let n = BigInt(val);
      const bytes = [];
      for (let i = 0; i < 8 && n > 0n; i++) {
        const c = Number(n & 0xffn);
        if (c >= 32 && c <= 126) bytes.push(String.fromCharCode(c));
        n >>= 8n;
      }
      if (bytes.length) tag = ' · "' + bytes.join('') + '"';
    } catch (e) {}
    return `<span class="muted">value</span> <span style="color:var(--cyan)">${fmt.esc(val)}</span><span class="muted">${fmt.esc(tag)}</span>`;
  }
  if (e.assetTransfer) {
    const a = e.assetTransfer;
    return `<span class="muted">asset</span> ${fmt.esc(a.assetName || '')} ${fmt.idLink(a.source)} → ${fmt.idLink(a.destination)}`;
  }
  if (e.assetIssuance) {
    return `<span class="muted">issue</span> ${fmt.esc(e.assetIssuance.assetName || '')}`;
  }
  if (e.contractErrorMessage || e.contractWarningMessage || e.contractInfoMessage || e.contractDebugMessage) {
    const m = e.contractErrorMessage || e.contractWarningMessage || e.contractInfoMessage || e.contractDebugMessage;
    return `<span class="muted">contract #${m.contractIndex ?? '?'}</span> code <span style="color:var(--orange)">${m.messageType ?? '?'}</span>`;
  }
  if (e.burning) {
    return `<span style="color:var(--red)">burn</span> ${fmt.n(e.burning.amount)} from ${fmt.idLink(e.burning.sourcePublicKey)}`;
  }
  if (e.contractReserveDeduction) {
    const d = e.contractReserveDeduction;
    return `<span class="muted">reserve dedux</span> contract #${d.contractIndex} −${fmt.n(d.deductedAmount)} (rem ${fmt.n(d.remainingAmount)})`;
  }
  if (e.oracleQueryStatusChange) {
    const o = e.oracleQueryStatusChange;
    return `<span class="muted">oracle</span> q ${fmt.n(o.queryId)} status ${o.queryStatus} type ${o.queryType}`;
  }
  if (e.oracleSubscriberLogMessage) {
    const o = e.oracleSubscriberLogMessage;
    return `<span class="muted">oracle sub</span> id ${o.subscriptionId} contract #${o.contractIndex}`;
  }
  // fallback: short hex preview of rawPayload
  const hex = fmt.b64ToHex(e.rawPayload || '');
  return `<span class="muted mono" style="font-size:0.85em">${fmt.esc(hex.slice(0, 64))}${hex.length > 64 ? '…' : ''}</span>`;
}

// Popup: event logs for a single transaction (backend anchors by tx hash).
window.__showTxLogs = async function(hash, tick) {
  openModal(`Event Logs · ${fmt.truncId(hash)}`, '<p class="muted">› loading…</p>');
  const body = () => document.querySelector('#modal-root .modal-body');
  const filters = {transactionHash: hash};
  if (tick != null) filters.tickNumber = String(tick); // bound backend anchor scan to one tick
  let r;
  try { r = await api.logs(0, 1000, {filters}); }
  catch (e) { body().innerHTML = `<div class="empty-state">error · ${fmt.esc(e.message || e)}</div>`; return; }

  if (!r || (r.code && r.code !== 0)) {
    body().innerHTML = `<div class="empty-state">${fmt.esc((r && r.message) || 'no data')}</div>`;
    return;
  }

  const entries = Array.isArray(r.eventLogs) ? r.eventLogs : [];
  if (!entries.length) {
    body().innerHTML = '<div class="empty-state">no event logs for this transaction</div>';
    return;
  }

  const rows = entries.map(e => {
    const lt = Number(e.logType);
    const ltClass = (lt === 0) ? 'info'
                    : (lt >= 4 && lt <= 7) ? 'alert'
                    : (lt === 8 || lt === 9) ? 'bad' : 'ok';
    return `
      <tr>
        <td class="num muted">${e.logId}</td>
        <td><span class="badge ${ltClass}">${fmt.esc(logTypeLabel(lt))}</span></td>
        <td>${logPayloadPreview(e)}</td>
        <td class="muted" style="font-size:0.85em">${fmt.esc(e.timestamp || '')}</td>
      </tr>`;
  }).join('');

  body().innerHTML = `
    <p class="muted" style="margin-bottom:1em">${entries.length} event(s) · tick ${fmt.n(entries[0].tickNumber)}</p>
    <div class="scroll" style="max-height:60vh"><table>
      <thead><tr><th class="num">LOG ID</th><th>TYPE</th><th>EVENT</th><th>TIME</th></tr></thead>
      <tbody>${rows}</tbody>
    </table></div>`;
};

async function renderLogs(page = 0, tickFilter = null) {
  $view().innerHTML = '<p class="muted">› loading event logs…</p>';
  const PAGE_SIZE = 50;
  // order:desc → newest first; matches the page 0 = newest pagination labels below.
  const body = tickFilter ? {order: 'desc', filters: {tickNumber: String(tickFilter)}} : {order: 'desc'};
  let r;
  try { r = await api.logs(page * PAGE_SIZE, PAGE_SIZE, body); }
  catch (e) {
    $view().innerHTML = `<div class="empty-state">backend error · ${fmt.esc(e.message || e)}</div>`;
    return;
  }

  if (!r || (r.code && r.code !== 0)) {
    $view().innerHTML = `
      <h2 class="section">EVENT LOGS</h2>
      <div class="empty-state">backend rejected · ${fmt.esc((r && r.message) || 'unknown')}</div>
      <p style="margin-top:1em"><a class="link" href="#/">‹ back</a></p>`;
    return;
  }

  const entries = Array.isArray(r.eventLogs) ? r.eventLogs : [];
  const total = (r.hits && r.hits.total) || 0;
  const totalPages = Math.max(1, Math.ceil(total / PAGE_SIZE));
  const validForTick = r.validForTick;

  const rows = entries.map(e => {
    const lt = Number(e.logType);
    const label = logTypeLabel(lt);
    const ltClass = (lt === 0) ? 'info'
                    : (lt >= 4 && lt <= 7) ? 'alert'
                    : (lt === 8 || lt === 9) ? 'bad'
                    : 'ok';
    const txCell = e.transactionHash
      ? `<a class="link id" href="#/tx/${fmt.esc(e.transactionHash.toUpperCase())}/${e.tickNumber}" title="${fmt.esc(e.transactionHash)}">${fmt.truncId(e.transactionHash)}</a>`
      : '<span class="muted">—</span>';

    return `
      <tr>
        <td class="num"><a class="link" href="#/tick/${e.tickNumber}">${fmt.n(e.tickNumber)}</a></td>
        <td class="num muted">${e.logId}</td>
        <td><span class="badge ${ltClass}">${fmt.esc(label)}</span></td>
        <td>${txCell}</td>
        <td>${logPayloadPreview(e)}</td>
        <td class="muted" style="font-size:0.85em">${fmt.esc(e.timestamp || '')}</td>
      </tr>`;
  }).join('');

  // pagination URLs preserve current filter
  const filterSuffix = tickFilter ? `/${tickFilter}` : '';
  const pgPrev = page > 0
    ? `<a class="link" href="#/logs/${page - 1}${filterSuffix}">‹ newer</a>`
    : '<span class="muted">‹ newer</span>';
  const pgNext = page + 1 < totalPages
    ? `<a class="link" href="#/logs/${page + 1}${filterSuffix}">older ›</a>`
    : '<span class="muted">older ›</span>';

  // wire filter input + button + clear
  window.__applyLogsFilter = () => {
    const v = (document.getElementById('logs-tick-input').value || '').trim();
    if (!v) { location.hash = '#/logs'; return; }
    const t = parseInt(v, 10);
    if (Number.isFinite(t) && t > 0) location.hash = '#/logs/0/' + t;
  };
  window.__clearLogsFilter = () => { location.hash = '#/logs'; };

  $view().innerHTML = `
    <h2 class="section">EVENT LOGS · ${fmt.n(total)} ${tickFilter ? `(filtered)` : 'TOTAL'} · VALID FOR TICK ${fmt.n(validForTick)}</h2>

    <div class="toolbar" style="margin-bottom:1em">
      <input id="logs-tick-input"
             placeholder="filter by tick number…"
             value="${tickFilter || ''}"
             onkeydown="if(event.key==='Enter')window.__applyLogsFilter()">
      <button onclick="window.__applyLogsFilter()">APPLY</button>
      ${tickFilter ? `<button onclick="window.__clearLogsFilter()">CLEAR</button>` : ''}
      ${tickFilter ? `<span class="badge info">TICK ${fmt.n(tickFilter)}</span>` : ''}
    </div>

    <div style="margin-bottom:1em;font-size:0.95em">
      <span class="muted">page</span>
      <strong style="color:var(--orange);font-size:1.1em">${page + 1}</strong>
      <span class="muted">/ ${totalPages} · showing ${total ? (page * PAGE_SIZE + 1) : 0}–${Math.min((page + 1) * PAGE_SIZE, total)}</span>
    </div>
    ${rows ? `<div class="scroll"><table>
      <thead><tr>
        <th class="num">TICK</th>
        <th class="num">LOG ID</th>
        <th>TYPE</th>
        <th>TX HASH</th>
        <th>EVENT</th>
        <th>TIMESTAMP</th>
      </tr></thead>
      <tbody>${rows}</tbody>
    </table></div>` : '<div class="empty-state">no event logs returned</div>'}
    <div style="display:flex;justify-content:space-between;align-items:center;margin-top:1.25em;font-size:0.95em">
      <span>${pgPrev}</span>
      <span class="muted">page ${page + 1} / ${totalPages}</span>
      <span>${pgNext}</span>
    </div>
    <p style="margin-top:2em"><a class="link" href="#/">‹ back</a></p>
  `;
}

// ---------- computors ----------
async function renderComputors() {
  $view().innerHTML = '<p class="muted">› loading 676 computors…</p>';
  let r;
  try { r = await api.data(); } catch (e) { $view().innerHTML = '<div class="empty-state">offline</div>'; return; }
  const cs = r.computors || [];
  const tickN = (r.header && r.header.tick) || 0;
  const leaderIdx = tickN % 676;

  const rows = cs.map(c => `
    <tr ${c.index === leaderIdx ? 'style="background:rgba(247,147,26,0.08)"' : ''}>
      <td class="num">${String(c.index).padStart(3,'0')}</td>
      <td>${fmt.idLink(c.publicKey, false)}</td>
      <td>${c.index === leaderIdx ? '<span class="badge alert">LEADER NOW</span>' : ''}</td>
    </tr>`).join('');

  $view().innerHTML = `
    <h2 class="section">COMPUTOR QUORUM · ${cs.length} NODES</h2>
    <div class="grid-3" style="margin-bottom:1.5em">
      <div class="tile pulse-tile">
        <div class="pulse-bars"><span></span><span></span><span></span><span></span><span></span><span></span><span></span></div>
        <div class="label" style="margin-top:0.6em">NETWORK LIVE</div>
      </div>
      <div class="tile">
        <div class="label">EPOCH</div>
        <div class="value">${(r.header && r.header.epoch) ?? '—'}</div>
      </div>
      <div class="tile">
        <div class="label">LEADER · TICK ${fmt.n(tickN)}</div>
        <div class="value">#${leaderIdx}</div>
      </div>
    </div>
    <div class="scroll">
      <table>
        <thead><tr><th class="num">#</th><th>IDENTITY</th><th>STATUS</th></tr></thead>
        <tbody>${rows}</tbody>
      </table>
    </div>
    <p style="margin-top:2em"><a class="link" href="#/">‹ back</a></p>
  `;
}
