#pragma once

namespace weblinked::assets {

/// The control page, compiled in.
///
/// Embedded rather than served from disk so the application is one file to
/// deploy and cannot be broken by a missing directory. It is the *whole* UI —
/// WebLinked opens no window of its own, so this page in a browser is the app's
/// only front end, which is why there is no GUI toolkit anywhere in this project.
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
  /* The browser's own [hidden] rule is display:none at UA specificity, so any
     class here that sets a display — .check is flex — silently outranks it and
     the element stays visible. That showed up as an alpha checkbox on the
     preview output, which has no alpha to carry. */
  [hidden] { display: none !important; }
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
  /* The source strip. Each chip carries its own thumbnail, because the id and
     the raster are not enough to tell two scoreboards apart at a glance — the
     picture is. */
  #source-strip {
    display: flex; align-items: stretch; gap: 8px; overflow-x: auto;
    padding: 8px 16px; background: var(--panel);
    border-bottom: 1px solid var(--line);
  }
  #source-chips { display: flex; gap: 8px; }
  .schip {
    display: flex; align-items: center; gap: 8px; flex: 0 0 auto;
    padding: 6px 10px 6px 6px; border-radius: 8px; cursor: pointer;
    background: var(--panel-2); border: 1px solid var(--line); color: var(--dim);
    font-size: 11px; text-align: left;
  }
  .schip:hover { border-color: var(--accent); }
  .schip.on { border-color: var(--accent); color: var(--text); }
  .schip canvas {
    width: 64px; height: 36px; border-radius: 4px; background: #000;
    display: block; flex: 0 0 auto;
  }
  .schip .sname { font-weight: 600; color: var(--text); }
  .schip .smeta { display: block; color: var(--dim); }
  #add-source {
    flex: 0 0 auto; align-self: center; margin-left: 4px;
  }
  #new-source {
    display: flex; align-items: center; gap: 6px; flex: 0 0 auto;
    align-self: center; margin-left: 4px;
  }
  #new-source[hidden] { display: none; }
  #new-source input {
    padding: 5px 7px; border-radius: 6px; font-size: 11px;
    background: var(--panel-2); border: 1px solid var(--line); color: var(--text);
  }
  #new-source input:invalid { border-color: var(--bad); }
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

  /* --- tabs ---------------------------------------------------------------
     Three views on one page rather than three pages: switching must not cost
     the preview stream or the state poll to a navigation. */
  nav { display: flex; gap: 2px; }
  nav button {
    background: none; border: 1px solid transparent; border-radius: 5px;
    color: var(--dim); padding: 4px 10px; font-size: 11px;
    text-transform: uppercase; letter-spacing: .06em;
  }
  nav button.on { background: var(--panel-2); border-color: var(--line); color: var(--text); }
  main[hidden] { display: none; }

  /* --- settings ----------------------------------------------------------- */
  input[type=number] {
    width: 100%; padding: 7px 9px; background: var(--bg);
    border: 1px solid var(--line); border-radius: 5px; color: var(--text);
    font: inherit;
  }
  .field { margin-bottom: 10px; }
  .fields { display: grid; grid-template-columns: 1fr 1fr; gap: 0 10px; }
  .check {
    display: flex; align-items: center; gap: 6px; color: var(--text);
    font-size: 12px; margin-bottom: 10px;
  }
  .check input { accent-color: var(--accent); margin: 0; }
  .output-editor {
    border: 1px solid var(--line); border-radius: 6px; padding: 10px 12px;
    margin-bottom: 10px; background: var(--panel-2);
  }
  .output-editor header {
    background: none; border: none; padding: 0 0 8px; gap: 8px;
  }
  .output-editor header .name { font-size: 12px; font-weight: 600; }
  .buttons { display: flex; gap: 8px; flex-wrap: wrap; }
  button.danger { border-color: #6a2c2a; color: #ffb3ae; }
  button.danger:hover { border-color: var(--bad); }
  .note { font-size: 11px; color: var(--dim); margin: 8px 0 0; }
  .path {
    font-family: ui-monospace, SFMono-Regular, Menlo, monospace; font-size: 11px;
    color: var(--dim); word-break: break-all;
  }

  /* --- diagnostics -------------------------------------------------------- */
  #log {
    background: #0e1114; border: 1px solid var(--line); border-radius: 5px;
    padding: 8px 10px; height: 420px; overflow: auto;
    font-family: ui-monospace, SFMono-Regular, Menlo, monospace;
    font-size: 11px; line-height: 1.45; white-space: pre-wrap; word-break: break-word;
  }
  #log div.warn { color: #ffc76b; }
  #log div.error, #log div.fatal { color: #ff9b93; }
  #log div.debug, #log div.trace { color: var(--dim); }
</style>
</head>
<body>
<header>
  <h1>WEBLINKED</h1>
  <span class="version" id="version"></span>
  <nav>
    <button data-view="control" class="on">Control</button>
    <button data-view="settings">Settings</button>
    <button data-view="diagnostics">Diagnostics</button>
  </nav>
  <span class="spacer"></span>
  <span class="pill"><span class="dot" id="engine-dot"></span><span id="engine-text">connecting</span></span>
  <span class="pill" id="format-pill">&mdash;</span>
</header>

<!-- The source strip. Hidden entirely when there is only one source, so a
     command-line launch looks exactly as it did before this existed and an
     operator with one feed is never asked to think about a collection. -->
<div id="source-strip">
  <div id="source-chips"></div>
  <button id="add-source" title="Start another pipeline">+ tab</button>
  <form id="new-source" hidden>
    <input type="text" id="ns-id" placeholder="id" spellcheck="false" size="8"
           pattern="[A-Za-z0-9._-]+" required
           title="Letters, digits, dot, dash and underscore">
    <input type="text" id="ns-url" placeholder="https://..." spellcheck="false">
    <input type="text" id="ns-format" placeholder="1080p50" spellcheck="false" size="9">
    <button type="submit" class="primary">Start</button>
    <button type="button" id="ns-cancel">Cancel</button>
  </form>
</div>

<main id="view-control">
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
        cookie banner or sign in. A link that asks for a new tab loads here
        instead of opening a window. <strong>This is the on-air output.</strong>
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

<main id="view-settings" hidden>
  <div style="display:flex;flex-direction:column;gap:14px">
    <section>
      <h2>Outputs</h2>
      <div id="output-editors"></div>
      <div class="buttons">
        <button id="add-output">Add an output</button>
      </div>
      <p class="note">
        Changes apply immediately. An output that cannot open keeps its previous
        settings and shows why &mdash; nothing is silently dropped.
      </p>
    </section>

    <section>
      <h2>Saved settings</h2>
      <p class="path" id="settings-path">&mdash;</p>
      <div class="buttons" style="margin-top:10px">
        <button class="primary" id="save-settings">Save</button>
        <button id="reload-settings">Reload from file</button>
      </div>
      <p class="note">
        Saving records what is actually running, so a card that failed to open
        is never written down as working. Anything given on the command line
        wins over the file at the next launch.
      </p>
    </section>
  </div>

  <div style="display:flex;flex-direction:column;gap:14px">
    <section>
      <h2>Source</h2>
      <div class="field">
        <label for="set-url">URL</label>
        <input type="text" id="set-url" spellcheck="false">
      </div>
      <div class="fields">
        <div class="field">
          <label for="set-format-2">Format</label>
          <input type="text" id="set-format-2" spellcheck="false" placeholder="1080p50">
        </div>
        <div class="field">
          <label for="set-matrix">Colour matrix</label>
          <select id="set-matrix">
            <option value="auto">auto (709 at 720+ lines)</option>
            <option value="709">BT.709</option>
            <option value="601">BT.601</option>
          </select>
        </div>
        <div class="field">
          <label for="set-pacing">Pacing</label>
          <select id="set-pacing">
            <option value="external">external &mdash; we drive frames</option>
            <option value="internal">internal &mdash; Chromium's timer</option>
          </select>
        </div>
        <div class="field">
          <label for="set-popups">New tabs and windows</label>
          <select id="set-popups">
            <option value="navigate">load here instead</option>
            <option value="block">block</option>
          </select>
        </div>
      </div>
      <label class="check">
        <input type="checkbox" id="set-interactive">
        Arm the preview for input when the page loads
      </label>
      <div class="buttons">
        <button class="primary" id="apply-settings">Apply</button>
        <!-- Only shown when there is more than one source: removing the only
             one would leave the process with nothing to control, which the
             server refuses anyway. -->
        <button id="remove-source" hidden>Remove this source</button>
      </div>
      <p class="note">
        Changing the format or the pacing restarts things: every output reopens
        at the new raster, and a pacing change rebuilds the browser at the same
        URL. Neither is a mid-show operation.
      </p>
    </section>
  </div>
</main>

<main id="view-diagnostics" hidden>
  <div style="display:flex;flex-direction:column;gap:14px">
    <section>
      <h2>Log
        <label class="toggle">
          <input type="checkbox" id="log-follow" checked> follow
        </label>
      </h2>
      <div id="log"></div>
      <div class="buttons" style="margin-top:10px">
        <select id="log-level" style="width:auto">
          <option value="trace">trace</option>
          <option value="debug">debug</option>
          <option value="info">info</option>
          <option value="warn">warn</option>
          <option value="error">error</option>
        </select>
        <button id="log-refresh">Refresh</button>
      </div>
    </section>
  </div>

  <div style="display:flex;flex-direction:column;gap:14px">
    <section>
      <h2>Collect</h2>
      <div class="buttons">
        <button class="primary" id="download-bundle">Download a bundle</button>
        <button id="write-report">Write a report</button>
      </div>
      <p class="note">
        A bundle is one JSON file carrying the build identity, the platform, the
        redacted configuration, the recent log and any crash reports sitting
        beside it. It downloads here rather than only naming a path, because the
        machine with the fault is usually not the one you are sitting at.
      </p>
    </section>

    <section>
      <h2>Where things are</h2>
      <dl id="diag-paths"></dl>
    </section>

    <section>
      <h2>Browser</h2>
      <dl id="diag-browser"></dl>
    </section>
  </div>
</main>

<div id="toast"></div>

<script>
const qs = new URLSearchParams(location.search);
const token = qs.get('token');

// Which pipeline this page is driving. Empty means "whichever is primary",
// which is what the server assumes for a request that names none — so before
// the first /api/sources reply, and for a single-source launch, every call
// behaves exactly as it did when there was only ever one engine.
//
// Threading it through api() rather than through each call site is deliberate:
// every fetch on this page already goes through here for the token, so there is
// no way to add a request later that forgets to say which source it meant. The
// process-wide endpoints ignore the parameter.
let currentSource = qs.get('source') || '';

function api(path) {
  const params = [];
  if (token) params.push('token=' + encodeURIComponent(token));
  if (currentSource) params.push('source=' + encodeURIComponent(currentSource));
  if (!params.length) return path;
  return path + (path.includes('?') ? '&' : '?') + params.join('&');
}

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

async function get(path) {
  const response = await fetch(api(path));
  const data = await response.json().catch(() => ({}));
  if (!response.ok) throw new Error(data.error || ('HTTP ' + response.status));
  return data;
}

// --- views ------------------------------------------------------------------
//
// Switching hides a <main> rather than navigating, so the preview stream and
// the state poll survive a trip to the settings and back.

let view = 'control';
let lastState = {};

function showView(name) {
  view = name;
  for (const button of document.querySelectorAll('nav button')) {
    button.classList.toggle('on', button.dataset.view === name);
  }
  for (const id of ['control', 'settings', 'diagnostics']) {
    document.getElementById('view-' + id).hidden = id !== name;
  }
  // Populated on arrival, not on every poll: a form that rewrites itself while
  // somebody is typing in it is worse than a stale one.
  if (name === 'settings') renderSettings(lastState);
  if (name === 'diagnostics') pullLog();
}

for (const button of document.querySelectorAll('nav button')) {
  button.onclick = () => showView(button.dataset.view);
}

// ?view=settings — so a view is linkable, and so the screenshot script can
// capture each one without driving a window server. Applied after the first
// state arrives rather than now: the settings editors are built from state, and
// switching to them before there is any would render an empty page.
const requestedView = qs.get('view');
let pendingView = ['control', 'settings', 'diagnostics'].includes(requestedView)
  ? requestedView
  : null;

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

// Output errors are built in C++ and can embed an operator-supplied device or
// source name, so they are escaped before they ever reach innerHTML. Any http(s)
// URL left in the text becomes a link — that is how the "NDI runtime not found"
// message turns into a one-click route to the redistributable download.
function errorHtml(text) {
  const escaped = String(text).replace(/[&<>"']/g, c => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'
  })[c]);
  // Trailing punctuation is sentence structure, not part of the URL.
  return escaped.replace(/https?:\/\/[^\s<]*[^\s<.,;:)]/g,
    url => '<a href="' + url + '" target="_blank" rel="noopener noreferrer">' + url + '</a>');
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
      (output.error ? '<div class="err">' + errorHtml(output.error) + '</div>' : '');
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

// --- settings ---------------------------------------------------------------
//
// The engine is the single source of truth: every editor is built from
// /api/state and every change is a request. Nothing is kept client-side, so two
// browsers open on the same instance cannot disagree about what is configured.

// Which extra fields each backend actually has. Showing an NDI alpha checkbox
// against a DeckLink would only invite an operator to set something that is
// silently ignored.
const OUTPUT_FIELDS = {
  ndi: ['alpha'],
  omt: ['alpha'],
  decklink: ['device', 'keying'],
  // No 'keying': aja_output.cpp implements none, so offering the control would
  // let an operator set a mode that is silently discarded.
  aja: ['device'],
  // The display list comes from state.displays, so this offers real monitors
  // rather than an index the operator has to count out for themselves.
  screen: ['display', 'scaling'],
  preview: ['factor'],
};

// A <option> per attached display. Falls back to a bare index when the build
// has no screen backend and state.displays is therefore absent, so the editor
// still renders rather than throwing halfway through building its HTML.
function displayOptions(selected) {
  const displays = lastState.displays || [];
  if (displays.length === 0) {
    return '<option value="' + selected + '">display ' + selected + '</option>';
  }
  return displays.map((d) => {
    const label = d.name + ' (' + d.width + 'x' + d.height +
                  (d.refresh_hz ? ', ' + Math.round(d.refresh_hz) + ' Hz' : '') +
                  (d.primary ? ', main' : '') + ')';
    return '<option value="' + d.index + '"' +
           (d.index === selected ? ' selected' : '') + '>' + label + '</option>';
  }).join('');
}

function outputEditor(output, isNew) {
  const kinds = lastState.compiled_backends || ['preview'];
  const options = output.options || {};
  const node = document.createElement('div');
  node.className = 'output-editor';

  const kind = isNew ? (kinds.includes('ndi') ? 'ndi' : kinds[0]) : output.kind;
  node.innerHTML =
    '<header><span class="name">' + (isNew ? 'New output' : output.name) + '</span></header>' +
    '<div class="fields">' +
      '<div class="field"><label>Kind</label><select data-f="kind">' +
        kinds.map((k) => '<option value="' + k + '"' + (k === kind ? ' selected' : '') +
                         '>' + k + '</option>').join('') +
      '</select></div>' +
      '<div class="field"><label>Name</label>' +
        '<input type="text" data-f="name" spellcheck="false" value="' +
        (output.name || '').replace(/"/g, '&quot;') + '"></div>' +
      '<div class="field" data-when="device"><label>Device index</label>' +
        '<input type="number" min="0" data-f="device_index" value="' +
        (output.device_index || 0) + '"></div>' +
      '<div class="field" data-when="keying"><label>Mode</label><select data-f="keying">' +
        '<option value="">Fill only</option>' +
        '<option value="external">Key + fill</option>' +
        '<option value="internal">Overlay</option></select></div>' +
      '<div class="field" data-when="keying"><label>Key level</label>' +
        '<input type="number" min="0" max="255" data-f="key_level" value="' +
        (options.key_level ?? 255) + '"></div>' +
      '<div class="field" data-when="factor"><label>Preview scale (1/n)</label>' +
        '<input type="number" min="1" max="16" data-f="factor" value="' +
        (options.factor ?? 4) + '"></div>' +
      '<div class="field" data-when="display"><label>Display</label>' +
        '<select data-f="display">' + displayOptions(options.display ?? 0) +
        '</select></div>' +
      '<div class="field" data-when="scaling"><label>Scaling</label>' +
        '<select data-f="scaling">' +
          '<option value="fit">Fit (bars)</option>' +
          '<option value="fill">Fill (crop)</option>' +
          '<option value="stretch">Stretch</option>' +
        '</select></div>' +
    '</div>' +
    '<p class="hint" data-when="display">' +
      'Paced by the display, not by the video format &mdash; a 50 Hz page on a ' +
      '60 Hz monitor repeats frames rather than tearing, so presented and ' +
      'frames are expected to differ.</p>' +
    '<p class="hint" data-when="keying">' +
      'Fill only sends the picture with no key. Key + fill puts fill and key on ' +
      'separate SDI connectors. Overlay composites over the input the card is ' +
      'already receiving, using its internal keyer. Never run against hardware ' +
      '&mdash; see docs/04-verification.md.</p>' +
    '<label class="check" data-when="alpha">' +
      '<input type="checkbox" data-f="alpha"' + (options.alpha ? ' checked' : '') + '>' +
      'Carry alpha (BGRA, straight)</label>' +
    '<div class="buttons">' +
      '<button class="primary" data-a="save">' + (isNew ? 'Add' : 'Apply') + '</button>' +
      (isNew ? '<button data-a="cancel">Cancel</button>'
             : '<button class="danger" data-a="remove">Remove</button>') +
    '</div>' +
    (output.error ? '<p class="err">' + errorHtml(output.error) + '</p>' : '');

  const field = (name) => node.querySelector('[data-f="' + name + '"]');
  field('keying').value = options.keying || '';
  field('scaling').value = options.scaling || 'fit';

  const applyVisibility = () => {
    const shown = OUTPUT_FIELDS[field('kind').value] || [];
    for (const element of node.querySelectorAll('[data-when]')) {
      element.hidden = !shown.includes(element.dataset.when);
    }
  };
  field('kind').onchange = applyVisibility;
  applyVisibility();

  const collect = () => {
    const kindValue = field('kind').value;
    const shown = OUTPUT_FIELDS[kindValue] || [];
    const body = {
      kind: kindValue,
      name: field('name').value.trim() || kindValue,
      device_index: shown.includes('device') ? Number(field('device_index').value) : 0,
      options: {},
    };
    if (shown.includes('alpha') && field('alpha').checked) body.options.alpha = true;
    if (shown.includes('keying') && field('keying').value) {
      body.options.keying = field('keying').value;
      body.options.key_level = Number(field('key_level').value);
    }
    if (shown.includes('factor')) body.options.factor = Number(field('factor').value);
    if (shown.includes('display')) body.options.display = Number(field('display').value);
    if (shown.includes('scaling')) body.options.scaling = field('scaling').value;
    return body;
  };

  node.querySelector('[data-a="save"]').onclick = async () => {
    const body = collect();
    const result = isNew
      ? await post('/api/output/add', body)
      : await post('/api/output/update', { name: output.name, output: body });
    if (result) {
      toast(isNew ? 'added ' + body.name : 'updated ' + body.name);
      await refresh();
      renderSettings(lastState);
    }
  };
  if (isNew) {
    node.querySelector('[data-a="cancel"]').onclick = () => renderSettings(lastState);
  } else {
    node.querySelector('[data-a="remove"]').onclick = async () => {
      if (await post('/api/output/remove', { name: output.name })) {
        toast('removed ' + output.name);
        await refresh();
        renderSettings(lastState);
      }
    };
  }
  return node;
}

function renderSettings(state) {
  const host = document.getElementById('output-editors');
  host.innerHTML = '';
  for (const output of state.outputs || []) {
    host.appendChild(outputEditor(output, false));
  }
  if (!(state.outputs || []).length) {
    host.innerHTML = '<p class="note">No outputs. The control page needs a preview to show anything.</p>';
  }

  const settings = state.settings || {};
  const source = state.source || {};
  document.getElementById('set-url').value = source.url || '';
  document.getElementById('set-format-2').value = state.format || '';
  document.getElementById('set-matrix').value = settings.matrix || 'auto';
  document.getElementById('set-pacing').value = source.pacing || 'external';
  document.getElementById('set-popups').value = source.popup_policy || 'navigate';
  document.getElementById('set-interactive').checked = !!settings.interactive_by_default;
}

document.getElementById('add-output').onclick = () => {
  document.getElementById('output-editors').appendChild(outputEditor({}, true));
};

document.getElementById('apply-settings').onclick = async () => {
  // One request rather than five, so the engine reconciles the whole thing and
  // leaves alone anything that has not actually changed.
  const source = {
    id: 'main',
    url: document.getElementById('set-url').value.trim() || 'about:blank',
    format: document.getElementById('set-format-2').value.trim(),
    matrix: document.getElementById('set-matrix').value,
    pacing: document.getElementById('set-pacing').value,
    popups: document.getElementById('set-popups').value,
    interactive: document.getElementById('set-interactive').checked,
    outputs: (lastState.outputs || []).map((output) => ({
      kind: output.kind,
      name: output.name,
      device_index: output.device_index || 0,
      options: output.options || {},
    })),
  };
  if (await post('/api/settings/apply', { source })) {
    toast('settings applied');
    await refresh();
    renderSettings(lastState);
  }
};

document.getElementById('save-settings').onclick = async () => {
  const result = await post('/api/settings/save', {});
  if (result) toast('saved to ' + result.path);
};

document.getElementById('reload-settings').onclick = async () => {
  if (await post('/api/settings/reload', {})) {
    toast('reloaded from file');
    await refresh();
    renderSettings(lastState);
  }
};

// --- diagnostics -------------------------------------------------------------

const logView = document.getElementById('log');
let logLevelKnown = false;
let settingsFilePath = '';

async function pullLog() {
  if (view !== 'diagnostics') return;
  try {
    const data = await get('/api/log?lines=400');
    if (!logLevelKnown) {
      document.getElementById('log-level').value = data.level || 'info';
      logLevelKnown = true;
    }
    const follow = document.getElementById('log-follow').checked;
    const atBottom = logView.scrollTop + logView.clientHeight >= logView.scrollHeight - 8;
    logView.innerHTML = (data.lines || [])
      .map((line) => {
        // "2026-07-31T09:14:02.113Z WARN message" — the level is the second
        // field, and colouring by it is the whole point of reading a log.
        const level = (line.split(' ')[1] || '').toLowerCase();
        return '<div class="' + level + '">' +
          line.replace(/&/g, '&amp;').replace(/</g, '&lt;') + '</div>';
      })
      .join('');
    if (follow && atBottom) logView.scrollTop = logView.scrollHeight;

    definitions(document.getElementById('diag-paths'), [
      ['log file', '<span class="path">' + (data.path || '—') + '</span>'],
      ['directory', '<span class="path">' + (data.directory || '—') + '</span>'],
      ['settings', '<span class="path">' + (settingsFilePath || '—') + '</span>'],
    ]);
  } catch (err) {
    toast(String(err), true);
  }
}

document.getElementById('log-refresh').onclick = pullLog;
document.getElementById('log-level').onchange = async (e) => {
  const result = await post('/api/log/level', { level: e.target.value });
  if (result) { toast('log level ' + result.level); pullLog(); }
};

document.getElementById('write-report').onclick = async () => {
  const result = await post('/api/diagnostics/report', { reason: 'requested from the control page' });
  if (result) toast('written to ' + result.report);
};

document.getElementById('download-bundle').onclick = () => {
  // A plain navigation, so the browser's own download machinery handles it and
  // the Content-Disposition filename is honoured.
  window.location.href = api('/api/diagnostics/bundle');
};

async function pullSettingsPath() {
  try {
    const data = await get('/api/settings');
    settingsFilePath = data.path || '';
    document.getElementById('settings-path').textContent =
      settingsFilePath + (data.saved ? '' : '  (not saved yet)');
  } catch (err) {
    /* the path is a nicety; the page works without it */
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

  definitions(document.getElementById('diag-browser'), [
    ['popups intercepted', source.popups ?? 0],
    ['popup policy', source.popup_policy || '—'],
    ['last popup', source.last_popup_url ? source.last_popup_url.slice(0, 60) : '—'],
    ['console errors', source.console_errors ?? 0],
    ['last console error', source.last_console_error || '—'],
    ['paints', source.paints ?? '—'],
    ['version', state.version || '—'],
  ]);

  const preview = (state.outputs || []).find((o) => o.kind === 'preview');
  if (preview && preview.audio_peak !== undefined) {
    document.getElementById('audio-meter').style.width =
      Math.min(100, preview.audio_peak * 100) + '%';
  }

  lastState = state;

  if (pendingView) {
    const wanted = pendingView;
    pendingView = null;
    showView(wanted);
  }

  // The preview arms itself on the first state that says it should. Done here
  // rather than in the markup because the answer belongs to the engine — one
  // instance can be launched with --no-interactive and another without, and
  // every browser opened on the same instance should agree.
  if (!interactiveResolved) {
    interactiveResolved = true;
    interactiveToggle.checked =
      !!(state.settings && state.settings.interactive_by_default);
    // Unconditionally, even when the checkbox already agrees: onchange is what
    // sends the focus event, and without it the page would look armed and
    // swallow every keystroke.
    interactiveToggle.onchange();
  }
}

// The canvas is declared here, above both the interaction and preview sections,
// because both use it. It used to live further down with the preview code, which
// put the interaction listeners in its temporal dead zone: the script threw on
// load, refresh() was never called, and the whole page sat on "connecting" with
// an empty preview. Nothing in the UI worked and there was no console error to
// see unless you were already attached.
const canvas = document.getElementById('preview');
const context = canvas.getContext('2d');

// --- interaction -------------------------------------------------------------
//
// The preview canvas can forward pointer and keyboard events to the live page,
// which is the only practical way to dismiss a cookie banner, close a modal or
// sign in on a machine whose browser you cannot otherwise reach.
//
// On by default — the engine decides, and says so in /api/state — because the
// preview is the only way to reach a page that wants a click before it shows
// anything, and an operator who has to find a toggle first usually concludes
// the preview is broken. Still visibly outlined whenever it is armed, because
// this is the on-air output: a stray click lands on the programme feed.
// `--no-interactive` restores the old behaviour.
//
// Positions are sent normalised. The canvas is a downscaled preview whose CSS
// size depends on the window, so pixel coordinates here mean nothing to the
// engine; it scales 0..1 to whatever the current raster is.

const interactiveToggle = document.getElementById('interactive');
const interactiveHint = document.getElementById('interactive-hint');
let interactive = false;
/// Set once the engine has told us whether the preview should start armed.
let interactiveResolved = false;

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

// --- the source strip -------------------------------------------------------
//
// One chip per pipeline, each with its own small live picture. The strip only
// appears once there is more than one source: a single-source launch — which is
// every command-line launch — should look exactly as it did before any of this
// existed.

let knownSources = [];
let lastChipPull = 0;
const chipCanvases = new Map();   // id -> canvas, so a redraw does not lose them

function selectSource(id) {
  if (currentSource === id) return;
  currentSource = id;
  // Every field on the page belongs to the source that was showing, so let the
  // next poll refill them rather than leaving one source's URL over another's.
  urlDirty = false;
  formatDirty = false;
  renderSourceChips();
  refresh();
  pullPreview();
  pullSettingsPath();
}

function renderSourceChips() {
  const host = document.getElementById('source-chips');
  const wanted = knownSources.map((s) => s.id).join(' ');
  if (host.dataset.signature !== wanted) {
    host.dataset.signature = wanted;
    host.textContent = '';
    chipCanvases.clear();
    for (const source of knownSources) {
      const chip = document.createElement('button');
      chip.className = 'schip';
      chip.dataset.id = source.id;

      const thumb = document.createElement('canvas');
      thumb.width = 64;
      thumb.height = 36;
      chipCanvases.set(source.id, thumb);
      chip.appendChild(thumb);

      const label = document.createElement('span');
      const name = document.createElement('span');
      name.className = 'sname';
      name.textContent = source.id;
      const meta = document.createElement('span');
      meta.className = 'smeta';
      meta.textContent = source.format || '';
      label.appendChild(name);
      label.appendChild(meta);
      chip.appendChild(label);

      chip.addEventListener('click', () => selectSource(source.id));
      host.appendChild(chip);
    }
  }
  for (const chip of host.children) {
    chip.classList.toggle('on', chip.dataset.id === currentSource);
  }
}

/// Paints one chip's thumbnail from that source's own preview output.
async function pullChip(id) {
  const thumb = chipCanvases.get(id);
  if (!thumb) return;
  try {
    // Built by hand rather than through api(), because api() deliberately
    // rewrites the source to the selected one — and the whole point of a chip
    // is to show a source that is *not* selected.
    let path = '/api/preview?source=' + encodeURIComponent(id);
    if (token) path += '&token=' + encodeURIComponent(token);
    const response = await fetch(path);
    if (!response.ok) return;
    const width = parseInt(response.headers.get('X-Frame-Width') || '0', 10);
    const height = parseInt(response.headers.get('X-Frame-Height') || '0', 10);
    if (!width || !height) return;
    const bytes = new Uint8Array(await response.arrayBuffer());
    if (bytes.length < width * height * 4) return;

    // Decoded at full size into an offscreen buffer, then drawn down to the
    // chip: putImageData ignores any transform, so it cannot scale by itself.
    const buffer = document.createElement('canvas');
    buffer.width = width;
    buffer.height = height;
    const image = buffer.getContext('2d').createImageData(width, height);
    for (let i = 0; i < width * height * 4; i += 4) {
      image.data[i] = bytes[i + 2];
      image.data[i + 1] = bytes[i + 1];
      image.data[i + 2] = bytes[i];
      image.data[i + 3] = 255;
    }
    buffer.getContext('2d').putImageData(image, 0, 0);
    thumb.getContext('2d').drawImage(buffer, 0, 0, thumb.width, thumb.height);
  } catch (err) {
    /* a dropped thumbnail is not worth reporting */
  }
}

async function pullSources() {
  let data;
  try {
    data = await get('/api/sources');
  } catch (err) {
    return;
  }
  knownSources = (data.sources || []).map((s) => ({
    id: s.id,
    format: s.format,
    running: s.running,
  }));

  // Fall back to the primary if the source this page was driving has gone —
  // otherwise every request 404s and the page looks broken rather than saying
  // that somebody removed the feed.
  if (currentSource && !knownSources.some((s) => s.id === currentSource)) {
    toast("source '" + currentSource + "' is gone", true);
    currentSource = '';
  }
  if (!currentSource && data.primary) currentSource = data.primary;

  // Removing the last tab would leave the process with nothing to render, so
  // that one stays hidden; the strip itself never does.
  document.getElementById('remove-source').hidden = knownSources.length < 2;
  renderSourceChips();

  // Throttled when hidden rather than skipped, for the same reason pullPreview
  // is: "hidden" covers a kiosk shell, an embedded webview and a screenshot
  // tool, all of which may have somebody looking straight at them. Skipping
  // outright is how every chip came out black the first time this was tried.
  const minimumInterval = document.hidden ? 2000 : 0;
  const now = Date.now();
  if (knownSources.length > 1 && now - lastChipPull >= minimumInterval) {
    lastChipPull = now;
    // Every source, the selected one included: its chip sits beside the others
    // and a black square there reads as a fault, not as "you are already
    // looking at this one".
    for (const source of knownSources) {
      pullChip(source.id);
    }
  }
}

document.getElementById('remove-source').addEventListener('click', async () => {
  const id = currentSource;
  if (!id) return;
  // A confirm, because this stops a feed that may be on air and the button sits
  // next to Apply.
  if (!confirm("Stop and remove '" + id + "'? Its outputs go off air.")) return;
  const removed = await post('/api/sources/remove', { id: id });
  if (removed) {
    toast("source '" + id + "' removed");
    currentSource = '';
    await pullSources();
    refresh();
  }
});

const newSourceForm = document.getElementById('new-source');

function showNewSource(show) {
  newSourceForm.hidden = !show;
  document.getElementById('add-source').hidden = show;
  if (show) {
    // Pre-filled from what is already running: a second tab is nearly always
    // the same raster as the first, and typing it again is a chance to get it
    // wrong.
    document.getElementById('ns-format').value =
      document.getElementById('format').value || '1080p50';
    document.getElementById('ns-url').value = '';
    document.getElementById('ns-id').value = '';
    document.getElementById('ns-id').focus();
  }
}

document.getElementById('add-source').addEventListener('click', () => showNewSource(true));
document.getElementById('ns-cancel').addEventListener('click', () => showNewSource(false));
newSourceForm.addEventListener('keydown', (event) => {
  if (event.key === 'Escape') showNewSource(false);
});

newSourceForm.addEventListener('submit', async (event) => {
  event.preventDefault();
  const id = document.getElementById('ns-id').value.trim();
  if (!id) return;
  if (knownSources.some((s) => s.id === id)) {
    toast("a tab called '" + id + "' already exists", true);
    return;
  }
  const source = { id: id, format: document.getElementById('ns-format').value.trim() };
  // Omitted rather than sent empty, so the server applies its own default.
  const url = document.getElementById('ns-url').value.trim();
  if (url) source.url = url;

  const created = await post('/api/sources/add', { source: source });
  if (created) {
    toast("tab '" + id + "' started");
    showNewSource(false);
    await pullSources();
    selectSource(id);
  }
  // On failure the form stays open with the values still in it, so a rejected
  // raster can be corrected rather than retyped.
});

// Coming back to the page should show a live picture straight away rather than
// whatever was on screen when it was hidden.
document.addEventListener('visibilitychange', () => {
  if (!document.hidden) { lastPull = 0; pullPreview(); refresh(); }
});

pullSources();
refresh();
pullPreview();
pullSettingsPath();
setInterval(refresh, 1000);
setInterval(pullPreview, 125);
// Slower than the main preview on purpose: a chip is for recognising a feed,
// not for judging it, and N sources means N of these requests every tick.
setInterval(pullSources, 1000);
// pullLog returns immediately unless the diagnostics view is showing, so this
// costs nothing while somebody is watching the preview.
setInterval(pullLog, 2000);
</script>
</body>
</html>
)WEBLINKED";

}  // namespace weblinked::assets
