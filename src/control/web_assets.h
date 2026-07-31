#pragma once

namespace weblinked::assets {

/// The control page, compiled in.
///
/// Embedded rather than served from disk so the application is one file to
/// deploy and cannot be broken by a missing directory. It is also loaded into a
/// normal windowed CEF browser as the app's own UI, which is why there is no
/// separate GUI toolkit anywhere in this project: CEF is already a dependency,
/// so the operator window and the remote control surface are the same page.
///
/// Deliberately plain: no framework, no build step, no external requests. A
/// broadcast tool that cannot open its own control panel because a CDN is
/// unreachable is not a broadcast tool.
inline constexpr const char* kControlPage = R"WEBLINKED(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>WebLinked</title>
<style>
  :root {
    --bg: #14171a;
    --panel: #1d2126;
    --panel-2: #23282e;
    --line: #31383f;
    --text: #e6eaee;
    --dim: #8e9aa6;
    --accent: #4da3ff;
    --live: #35c46a;
    --off: #5a646e;
    --bad: #ff5f56;
  }
  * { box-sizing: border-box; }
  body {
    margin: 0; background: var(--bg); color: var(--text);
    font: 13px/1.5 -apple-system, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
  }
  header {
    display: flex; align-items: center; gap: 12px;
    padding: 10px 16px; background: var(--panel);
    border-bottom: 1px solid var(--line);
  }
  header h1 { font-size: 14px; font-weight: 600; margin: 0; letter-spacing: .04em; }
  header .version { color: var(--dim); font-size: 11px; }
  header .spacer { flex: 1; }
  .pill {
    display: inline-flex; align-items: center; gap: 6px;
    padding: 3px 9px; border-radius: 999px; font-size: 11px;
    background: var(--panel-2); border: 1px solid var(--line); color: var(--dim);
  }
  .dot { width: 7px; height: 7px; border-radius: 50%; background: var(--off); }
  .dot.on { background: var(--live); }
  .dot.bad { background: var(--bad); }
  main {
    display: grid; grid-template-columns: minmax(320px, 1fr) minmax(280px, 420px);
    gap: 14px; padding: 14px; align-items: start;
  }
  @media (max-width: 820px) { main { grid-template-columns: 1fr; } }
  section {
    background: var(--panel); border: 1px solid var(--line);
    border-radius: 8px; padding: 12px 14px;
  }
  section h2 {
    font-size: 11px; text-transform: uppercase; letter-spacing: .08em;
    color: var(--dim); margin: 0 0 10px; font-weight: 600;
  }
  label { display: block; font-size: 11px; color: var(--dim); margin-bottom: 4px; }
  input[type=text], select {
    width: 100%; padding: 7px 9px; background: var(--bg);
    border: 1px solid var(--line); border-radius: 5px; color: var(--text);
    font: inherit;
  }
  input[type=text]:focus, select:focus { outline: 1px solid var(--accent); border-color: var(--accent); }
  .row { display: flex; gap: 8px; align-items: flex-end; }
  .row > *:first-child { flex: 1; }
  button {
    padding: 7px 13px; background: var(--panel-2); color: var(--text);
    border: 1px solid var(--line); border-radius: 5px; font: inherit;
    cursor: pointer; white-space: nowrap;
  }
  button:hover { border-color: var(--accent); }
  button.primary { background: var(--accent); border-color: var(--accent); color: #08121c; font-weight: 600; }
  #preview {
    width: 100%; display: block; background: #000; border-radius: 5px;
    border: 1px solid var(--line); image-rendering: auto;
  }
  table { width: 100%; border-collapse: collapse; font-size: 12px; }
  th, td { text-align: left; padding: 6px 6px; border-bottom: 1px solid var(--line); }
  th { color: var(--dim); font-weight: 500; font-size: 11px; }
  tr:last-child td { border-bottom: none; }
  td.num { text-align: right; font-variant-numeric: tabular-nums; color: var(--dim); }
  .kind { color: var(--dim); font-size: 11px; }
  .err { color: var(--bad); font-size: 11px; }
  .meter { height: 4px; background: var(--panel-2); border-radius: 2px; overflow: hidden; margin-top: 8px; }
  .meter > div { height: 100%; background: var(--live); width: 0; transition: width .1s linear; }
  dl { display: grid; grid-template-columns: auto 1fr; gap: 4px 12px; margin: 0; font-size: 12px; }
  dt { color: var(--dim); }
  dd { margin: 0; font-variant-numeric: tabular-nums; }
  section h2 .toggle {
    float: right; text-transform: none; letter-spacing: 0; font-weight: 400;
    color: var(--dim); cursor: pointer; display: inline-flex; align-items: center;
    gap: 5px; font-size: 11px;
  }
  section h2 .toggle input { margin: 0; accent-color: var(--accent); }
  .hint { font-size: 11px; color: var(--dim); margin: 8px 0 0; }
  .hint strong { color: var(--bad); font-weight: 600; }
  #preview.live { outline: 2px solid var(--accent); outline-offset: 2px; cursor: crosshair; }
  #toast {
    position: fixed; bottom: 14px; left: 50%; transform: translateX(-50%);
    background: var(--panel-2); border: 1px solid var(--line); color: var(--text);
    padding: 8px 14px; border-radius: 6px; font-size: 12px; opacity: 0;
    transition: opacity .2s; pointer-events: none; max-width: 80vw;
  }
  #toast.show { opacity: 1; }
  #toast.bad { border-color: var(--bad); color: #ffd9d6; }
</style>
</head>
<body>
<header>
  <h1>WEBLINKED</h1>
  <span class="version" id="version"></span>
  <span class="spacer"></span>
  <span class="pill"><span class="dot" id="engine-dot"></span><span id="engine-text">connecting</span></span>
  <span class="pill" id="format-pill">&mdash;</span>
</header>

<main>
  <div style="display:flex;flex-direction:column;gap:14px">
    <section>
      <h2>Source</h2>
      <label for="url">URL</label>
      <div class="row">
        <input type="text" id="url" spellcheck="false" placeholder="https://example.com/graphic.html">
        <button class="primary" id="load">Load</button>
      </div>
      <div class="row" style="margin-top:10px">
        <button id="reload">Reload</button>
        <button id="hard-reload">Reload (no cache)</button>
        <button id="mute">Mute</button>
      </div>
      <div class="meter"><div id="audio-meter"></div></div>
    </section>

    <section>
      <h2>Preview
        <label class="toggle" title="Forward clicks and keys to the page. Remember the page is on air.">
          <input type="checkbox" id="interactive"> interactive
        </label>
      </h2>
      <canvas id="preview" width="480" height="270" tabindex="0"></canvas>
      <p id="interactive-hint" class="hint" hidden>
        Clicks, scrolling and typing go to the live page — enough to dismiss a
        cookie banner or close a popup. <strong>This is the on-air output.</strong>
      </p>
    </section>

    <section>
      <h2>Format</h2>
      <div class="row">
        <input type="text" id="format" spellcheck="false" placeholder="1080p50">
        <button id="set-format">Apply</button>
      </div>
      <p style="color:var(--dim);font-size:11px;margin:8px 0 0">
        Shorthand such as <code>1080p50</code>, <code>720p59.94</code>,
        <code>1080i25</code>, or an explicit raster like <code>1920x1080p50</code>.
        Applying restarts every output.
      </p>
    </section>
  </div>

  <div style="display:flex;flex-direction:column;gap:14px">
    <section>
      <h2>Outputs</h2>
      <table id="outputs"><tbody></tbody></table>
    </section>

    <section>
      <h2>Pacing</h2>
      <dl id="pacing"></dl>
    </section>

    <section>
      <h2>Source detail</h2>
      <dl id="source"></dl>
    </section>
  </div>
</main>

<div id="toast"></div>

<script>
const qs = new URLSearchParams(location.search);
const token = qs.get('token');
const api = (path) => token ? path + (path.includes('?') ? '&' : '?') + 'token=' + encodeURIComponent(token) : path;

let toastTimer = null;
function toast(message, bad) {
  const el = document.getElementById('toast');
  el.textContent = message;
  el.className = 'show' + (bad ? ' bad' : '');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { el.className = ''; }, 3200);
}

async function post(path, payload) {
  try {
    const response = await fetch(api(path), {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload || {}),
    });
    const data = await response.json().catch(() => ({}));
    if (!response.ok) {
      toast(data.error || ('HTTP ' + response.status), true);
      return null;
    }
    return data;
  } catch (err) {
    toast(String(err), true);
    return null;
  }
}

// --- controls ---------------------------------------------------------------

let urlDirty = false;
const urlField = document.getElementById('url');
urlField.addEventListener('input', () => { urlDirty = true; });

document.getElementById('load').onclick = async () => {
  const value = urlField.value.trim();
  if (!value) return;
  if (await post('/api/url', { url: value })) { urlDirty = false; toast('loading ' + value); }
};
urlField.addEventListener('keydown', (e) => { if (e.key === 'Enter') document.getElementById('load').click(); });

document.getElementById('reload').onclick = () => post('/api/reload', {});
document.getElementById('hard-reload').onclick = () => post('/api/reload', { ignore_cache: true });

let muted = false;
document.getElementById('mute').onclick = async () => {
  if (await post('/api/mute', { muted: !muted })) muted = !muted;
};

const formatField = document.getElementById('format');
let formatDirty = false;
formatField.addEventListener('input', () => { formatDirty = true; });
document.getElementById('set-format').onclick = async () => {
  const value = formatField.value.trim();
  if (!value) return;
  const result = await post('/api/format', { format: value });
  if (result) { formatDirty = false; toast('format now ' + value); }
};

// --- state polling ----------------------------------------------------------

function definitions(target, pairs) {
  target.innerHTML = pairs
    .map(([key, value]) => '<dt>' + key + '</dt><dd>' + value + '</dd>')
    .join('');
}

function renderOutputs(outputs) {
  const body = document.querySelector('#outputs tbody');
  body.innerHTML = '';
  for (const output of outputs) {
    const row = document.createElement('tr');

    const nameCell = document.createElement('td');
    const live = output.enabled && output.running;
    nameCell.innerHTML = '<span class="dot ' + (live ? 'on' : (output.error ? 'bad' : '')) +
      '" style="display:inline-block;margin-right:6px"></span>' +
      output.name + '<div class="kind">' + output.kind +
      (output.device ? ' &middot; ' + output.device : '') +
      (output.pixel_format ? ' &middot; ' + output.pixel_format : '') + '</div>' +
      (output.error ? '<div class="err">' + output.error + '</div>' : '');
    row.appendChild(nameCell);

    const statCell = document.createElement('td');
    statCell.className = 'num';
    const bits = [];
    if (output.frames !== undefined) bits.push(output.frames + ' f');
    if (output.receivers !== undefined) bits.push(output.receivers + ' rx');
    if (output.buffered_frames !== undefined) bits.push('buf ' + output.buffered_frames);
    if (output.frames_dropped) bits.push(output.frames_dropped + ' drop');
    statCell.innerHTML = bits.join('<br>');
    row.appendChild(statCell);

    const actionCell = document.createElement('td');
    actionCell.style.width = '1%';
    const button = document.createElement('button');
    button.textContent = live ? 'Stop' : 'Start';
    button.onclick = async () => {
      button.disabled = true;
      await post('/api/output', { name: output.name, enabled: !live });
      button.disabled = false;
      refresh();
    };
    actionCell.appendChild(button);
    row.appendChild(actionCell);

    body.appendChild(row);
  }
  if (!outputs.length) {
    body.innerHTML = '<tr><td class="kind">no outputs configured</td></tr>';
  }
}

async function refresh() {
  let state;
  try {
    const response = await fetch(api('/api/state'));
    state = await response.json();
  } catch (err) {
    document.getElementById('engine-dot').className = 'dot bad';
    document.getElementById('engine-text').textContent = 'no connection';
    return;
  }

  document.getElementById('version').textContent = 'v' + (state.version || '?');
  document.getElementById('engine-dot').className = 'dot' + (state.running ? ' on' : '');
  document.getElementById('engine-text').textContent = state.running ? 'running' : 'stopped';
  document.getElementById('format-pill').textContent = state.format || '—';

  if (!urlDirty && state.source) urlField.value = state.source.url || '';
  if (!formatDirty) formatField.value = state.format || '';
  if (state.source) {
    muted = !!state.source.audio_muted;
    document.getElementById('mute').textContent = muted ? 'Unmute' : 'Mute';
  }

  renderOutputs(state.outputs || []);

  const pacing = state.pacing || {};
  const audio = state.audio || {};
  definitions(document.getElementById('pacing'), [
    ['ticks', pacing.ticks ?? '—'],
    ['repeated frames', pacing.repeated_frames ?? '—'],
    ['dropped ticks', pacing.dropped_ticks ?? '—'],
    ['last lateness', (pacing.last_lateness_us ?? 0) + ' µs'],
    ['paints', (state.source && state.source.paints) ?? '—'],
    ['audio', (audio.channels || 0) + ' ch @ ' + (audio.sample_rate || 0) + ' Hz'],
    ['audio buffer', (audio.buffered_frames ?? 0) + ' samples'],
    ['under / over', (audio.underruns ?? 0) + ' / ' + (audio.overruns ?? 0)],
  ]);

  const source = state.source || {};
  definitions(document.getElementById('source'), [
    ['loaded', source.loaded_url ? source.loaded_url.slice(0, 60) : '—'],
    ['loading', source.loading ? 'yes' : 'no'],
    ['pacing', source.pacing || '—'],
    ['console errors', source.console_errors ?? 0],
    ['backends', (state.compiled_backends || []).join(', ')],
    ['last error', source.last_error || '—'],
  ]);

  const preview = (state.outputs || []).find((o) => o.kind === 'preview');
  if (preview && preview.audio_peak !== undefined) {
    document.getElementById('audio-meter').style.width =
      Math.min(100, preview.audio_peak * 100) + '%';
  }
}

// --- interaction -------------------------------------------------------------
//
// The preview canvas can forward pointer and keyboard events to the live page,
// which is the only practical way to dismiss a cookie banner, close a modal or
// sign in on a machine whose browser you cannot otherwise reach.
//
// Off by default and visibly outlined when on, because this is the on-air
// output: a stray click lands on the programme feed.
//
// Positions are sent normalised. The canvas is a downscaled preview whose CSS
// size depends on the window, so pixel coordinates here mean nothing to the
// engine; it scales 0..1 to whatever the current raster is.

const interactiveToggle = document.getElementById('interactive');
const interactiveHint = document.getElementById('interactive-hint');
let interactive = false;

interactiveToggle.onchange = () => {
  interactive = interactiveToggle.checked;
  canvas.classList.toggle('live', interactive);
  interactiveHint.hidden = !interactive;
  sendInput({ type: 'focus', focused: interactive });
  if (interactive) canvas.focus();
};

function eventPosition(e) {
  const box = canvas.getBoundingClientRect();
  return {
    nx: Math.min(1, Math.max(0, (e.clientX - box.left) / box.width)),
    ny: Math.min(1, Math.max(0, (e.clientY - box.top) / box.height)),
  };
}

// CEF event flags, from cef_types.h.
function modifiersOf(e) {
  let m = 0;
  if (e.shiftKey) m |= 1 << 1;
  if (e.ctrlKey)  m |= 1 << 2;
  if (e.altKey)   m |= 1 << 3;
  if (e.metaKey)  m |= 1 << 7;
  if (e.buttons & 1) m |= 1 << 4;
  if (e.buttons & 4) m |= 1 << 5;
  if (e.buttons & 2) m |= 1 << 6;
  return m;
}

// Pointer moves are coalesced into one request per animation frame. Sending one
// HTTP request per mousemove would swamp the control server during a drag.
let pendingMove = null;
let moveScheduled = false;

function flushMove() {
  moveScheduled = false;
  if (pendingMove) { sendInput(pendingMove); pendingMove = null; }
}

async function sendInput(event) {
  try {
    await fetch(api('/api/input'), {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(event),
    });
  } catch (err) {
    /* a dropped input event is not worth a dialog */
  }
}

canvas.addEventListener('mousemove', (e) => {
  if (!interactive) return;
  pendingMove = { type: 'move', ...eventPosition(e), modifiers: modifiersOf(e) };
  if (!moveScheduled) { moveScheduled = true; requestAnimationFrame(flushMove); }
});

canvas.addEventListener('mouseleave', (e) => {
  if (!interactive) return;
  sendInput({ type: 'move', ...eventPosition(e), leaving: true, modifiers: 0 });
});

canvas.addEventListener('mousedown', (e) => {
  if (!interactive) return;
  e.preventDefault();
  canvas.focus();
  sendInput({ type: 'down', ...eventPosition(e), button: e.button,
              clicks: e.detail || 1, modifiers: modifiersOf(e) });
});

canvas.addEventListener('mouseup', (e) => {
  if (!interactive) return;
  e.preventDefault();
  sendInput({ type: 'up', ...eventPosition(e), button: e.button,
              clicks: e.detail || 1, modifiers: modifiersOf(e) });
});

canvas.addEventListener('contextmenu', (e) => { if (interactive) e.preventDefault(); });

canvas.addEventListener('wheel', (e) => {
  if (!interactive) return;
  e.preventDefault();
  // Chromium expects pixel deltas; a wheel line is conventionally about 40.
  const scale = e.deltaMode === 1 ? 40 : 1;
  sendInput({ type: 'wheel', ...eventPosition(e),
              dx: Math.round(-e.deltaX * scale), dy: Math.round(-e.deltaY * scale),
              modifiers: modifiersOf(e) });
}, { passive: false });

canvas.addEventListener('keydown', (e) => {
  if (!interactive) return;
  e.preventDefault();
  const modifiers = modifiersOf(e);
  const printable = e.key.length === 1 && !e.ctrlKey && !e.metaKey;
  // The character has to go on the *keydown* too, not just the CHAR event.
  // Without it Chromium cannot work out which key was pressed from a bare
  // virtual-key code and the page sees e.key as "Unidentified" — so a graphic
  // listening for a specific key never fires. Verified: with it, the page reads
  // the right e.key and text lands in input fields.
  const character = printable ? e.key.charCodeAt(0) : 0;
  sendInput({ type: 'key', action: 'down', key_code: e.keyCode, character, modifiers });

  // A key that produces text needs a separate CHAR event, or the keystroke
  // arrives but nothing is typed.
  if (printable) {
    sendInput({ type: 'key', action: 'char', key_code: e.keyCode,
                character: e.key.charCodeAt(0), modifiers });
  } else if (e.key === 'Enter') {
    sendInput({ type: 'key', action: 'char', key_code: 13, character: 13, modifiers });
  }
});

canvas.addEventListener('keyup', (e) => {
  if (!interactive) return;
  e.preventDefault();
  const printable = e.key.length === 1 && !e.ctrlKey && !e.metaKey;
  sendInput({ type: 'key', action: 'up', key_code: e.keyCode,
              character: printable ? e.key.charCodeAt(0) : 0,
              modifiers: modifiersOf(e) });
});

// --- preview ----------------------------------------------------------------

const canvas = document.getElementById('preview');
const context = canvas.getContext('2d');
let previewBusy = false;
let lastPull = 0;

async function pullPreview() {
  if (previewBusy) return;
  // A hidden document still gets a picture, just a slow one. Skipping it
  // entirely is tempting for CPU, but "hidden" covers more than a backgrounded
  // tab — a kiosk shell, an embedded webview or a screenshot tool can all report
  // hidden while somebody is looking straight at it, and a confidence monitor
  // that shows black is worse than useless.
  const minimumInterval = document.hidden ? 2000 : 0;
  const now = Date.now();
  if (now - lastPull < minimumInterval) return;
  lastPull = now;

  previewBusy = true;
  try {
    const response = await fetch(api('/api/preview'));
    if (!response.ok) return;
    const width = parseInt(response.headers.get('X-Frame-Width') || '0', 10);
    const height = parseInt(response.headers.get('X-Frame-Height') || '0', 10);
    if (!width || !height) return;
    const bytes = new Uint8Array(await response.arrayBuffer());
    if (bytes.length < width * height * 4) return;

    if (canvas.width !== width || canvas.height !== height) {
      canvas.width = width;
      canvas.height = height;
    }
    const image = context.createImageData(width, height);
    // The wire format is BGRA, straight from the frame pipeline. Swapping here
    // costs nothing and saves a conversion pass in the engine.
    for (let i = 0; i < width * height * 4; i += 4) {
      image.data[i] = bytes[i + 2];
      image.data[i + 1] = bytes[i + 1];
      image.data[i + 2] = bytes[i];
      image.data[i + 3] = 255;
    }
    context.putImageData(image, 0, 0);
  } catch (err) {
    /* a dropped poll is not worth reporting */
  } finally {
    previewBusy = false;
  }
}

// Coming back to the page should show a live picture straight away rather than
// whatever was on screen when it was hidden.
document.addEventListener('visibilitychange', () => {
  if (!document.hidden) { lastPull = 0; pullPreview(); refresh(); }
});

refresh();
pullPreview();
setInterval(refresh, 1000);
setInterval(pullPreview, 125);
</script>
</body>
</html>
)WEBLINKED";

}  // namespace weblinked::assets
