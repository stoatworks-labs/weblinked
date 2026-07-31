// Launcher panel controller.
//
// Talks to the Rust backend via Tauri's global `invoke`. In a plain browser it
// falls back to mock data (with per-app themes) so the panel — and its theming
// — can be previewed and screenshotted without the native app.

const hasTauri = !!(window.__TAURI__ && window.__TAURI__.core);
const invoke = hasTauri ? window.__TAURI__.core.invoke : mockInvoke;

const el = (id) => document.getElementById(id);
const ui = {
  mark: el("mark"),
  name: el("app-name"),
  sub: el("app-sub"),
  card: el("server-card"),
  state: el("state"),
  url: el("url"),
  iface: el("iface"),
  port: el("port"),
  toggle: el("toggle"),
  launch: el("launch"),
  hide: el("hide"),
  quit: el("quit"),
  gear: el("gear"),
  msg: el("msg"),
};

let running = false;
let pollTimer = null;

function flash(text, isError = false) {
  ui.msg.textContent = text || "";
  ui.msg.classList.toggle("error", isError);
}

function applyTheme(theme) {
  if (!theme) return;
  for (const [k, v] of Object.entries(theme)) {
    document.documentElement.style.setProperty(`--${k}`, v);
  }
}

function renderStatus(status) {
  running = status.running;
  ui.state.textContent = status.running ? "Running" : "Stopped";
  ui.url.textContent = status.url || "not running";
  ui.url.href = status.url || "#";
  ui.card.classList.toggle("running", status.running);

  ui.toggle.textContent = status.running ? "Stop server" : "Start server";
  ui.toggle.classList.toggle("is-running", status.running);
  ui.iface.disabled = status.running;
  ui.port.disabled = status.running;
  ui.launch.disabled = !status.running;

  if (status.message && status.message !== "Running" && status.message !== "Stopped") {
    flash(status.message);
  } else {
    flash("");
  }
}

async function refreshStatus() {
  try {
    renderStatus(await invoke("get_status"));
  } catch (e) {
    flash(String(e), true);
  }
}

function startPolling() {
  stopPolling();
  pollTimer = setInterval(refreshStatus, 2000);
}
function stopPolling() {
  if (pollTimer) clearInterval(pollTimer);
  pollTimer = null;
}

async function persist() {
  const port = parseInt(ui.port.value, 10);
  if (!Number.isFinite(port) || port < 1 || port > 65535) {
    flash("Port must be 1–65535", true);
    return;
  }
  try {
    await invoke("save_settings", { port, interface: ui.iface.value });
    await refreshStatus();
  } catch (e) {
    flash(String(e), true);
  }
}

async function init() {
  try {
    const info = await invoke("get_app_info");
    applyTheme(info.theme);
    ui.name.textContent = info.name;
    ui.mark.textContent = (info.name.trim()[0] || "◆").toUpperCase();
    document.title = `${info.name} Launcher`;

    const ifaces = await invoke("list_interfaces");
    const settings = await invoke("get_settings");
    ui.iface.innerHTML = "";
    for (const i of ifaces) {
      const opt = document.createElement("option");
      opt.value = i.name;
      opt.textContent = i.label;
      if (i.name === settings.interface) opt.selected = true;
      ui.iface.appendChild(opt);
    }
    ui.port.value = settings.port;

    await refreshStatus();
    startPolling();
  } catch (e) {
    flash(String(e), true);
  }
}

// --- Wiring ---
ui.iface.addEventListener("change", persist);
ui.port.addEventListener("change", persist);

ui.toggle.addEventListener("click", async () => {
  ui.toggle.disabled = true;
  try {
    renderStatus(await invoke(running ? "stop_server" : "start_server"));
  } catch (e) {
    flash(String(e), true);
  } finally {
    ui.toggle.disabled = false;
  }
});

ui.launch.addEventListener("click", () => invoke("open_gui").catch((e) => flash(String(e), true)));
ui.hide.addEventListener("click", () => invoke("hide_window").catch(() => {}));
ui.quit.addEventListener("click", () => invoke("quit_app").catch(() => {}));
ui.gear.addEventListener("click", () =>
  flash("Config: ~/Library/Application Support/<launcher-id>")
);

window.addEventListener("DOMContentLoaded", init);

// ---------- Mock backend (browser preview + screenshots only) ----------
// ?app=flock&port=8080&state=running&host=10.147.17.93 picks the app/state.
const MOCK_THEMES = {
  // This repo's own app, first: the mock is what scripts/screenshot.sh renders,
  // and the palette is copied from src/control/web_assets.h so the panel and
  // the control page it opens read as one application.
  WebLinked: {
    bg: "#14171a", panel: "#1d2126", "panel-2": "#23282e", border: "#31383f",
    text: "#e6eaee", muted: "#8e9aa6", dim: "#5a646e",
    accent: "#4da3ff", "accent-soft": "#16283c", good: "#35c46a",
  },
  "SRT Router": {
    bg: "#14161a", panel: "#1a1d24", "panel-2": "#22262e", border: "#2a2d33",
    text: "#e6e6e6", muted: "#b7bfca", dim: "#6b7280",
    accent: "#9fb4ff", "accent-soft": "#1c2333", good: "#37835c",
  },
  flock: {
    bg: "#14161a", panel: "#1b1e24", "panel-2": "#21252c", border: "#2c313a",
    text: "#e6e8eb", muted: "#9aa1ac", dim: "#6b7280",
    accent: "#1fae63", "accent-soft": "#15271d", good: "#1fae63",
  },
  RFutils: {
    bg: "#12141a", panel: "#1a1d26", "panel-2": "#232733", border: "#2a2e3a",
    text: "#e8eaf0", muted: "#9aa1b2", dim: "#6b7080",
    accent: "#6ea8fe", "accent-soft": "#172138", good: "#3fae5a",
  },
};

function mockInvoke(cmd, args = {}) {
  const q = new URLSearchParams(location.search);
  const host = q.get("host") || "10.147.17.93";
  const app = q.get("app") || "SRT Router";
  const s =
    mockInvoke.state ||
    (mockInvoke.state = {
      running: q.get("state") === "running",
      port: Number(q.get("port")) || 8080,
      iface: q.get("iface") || "en0",
    });
  const url = () => `http://${s.iface === "lo0" ? "127.0.0.1" : host}:${s.port}/`;
  const status = () => ({
    running: s.running,
    url: url(),
    host,
    port: s.port,
    message: s.running ? "Running" : "Stopped",
  });
  switch (cmd) {
    case "get_app_info":
      return Promise.resolve({
        name: app,
        default_port: s.port,
        url_template: "http://{host}:{port}/",
        theme: MOCK_THEMES[app] || MOCK_THEMES.WebLinked,
      });
    case "list_interfaces":
      return Promise.resolve([
        { name: "all", ip: "0.0.0.0", label: "All interfaces (0.0.0.0)", loopback: false },
        { name: "en0", ip: "10.147.17.93", label: "en0: 10.147.17.93", loopback: false },
        { name: "lo0", ip: "127.0.0.1", label: "lo0: 127.0.0.1", loopback: true },
      ]);
    case "get_settings":
      return Promise.resolve({ port: s.port, interface: s.iface });
    case "save_settings":
      s.port = args.port; s.iface = args.interface; return Promise.resolve();
    case "get_status":
      return Promise.resolve(status());
    case "start_server":
      s.running = true; return Promise.resolve(status());
    case "stop_server":
      s.running = false; return Promise.resolve(status());
    default:
      return Promise.resolve();
  }
}
