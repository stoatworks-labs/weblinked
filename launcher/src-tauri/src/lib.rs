//! WebLinked's tray launcher — the av-launcher shell, which supervises a local
//! web-server app: pick a network interface + port, Start/Stop, open the GUI,
//! and live in the system tray.

mod config;
mod embedded;

use std::process::{Child, Command};
use std::sync::Mutex;

use serde::{Deserialize, Serialize};
use tauri::menu::{Menu, MenuItem};
use tauri::tray::{MouseButton, MouseButtonState, TrayIconBuilder, TrayIconEvent};
use tauri::{AppHandle, Manager, State, WindowEvent};

/// Runtime state: the supervised child process, if running.
#[derive(Default)]
struct AppState {
    child: Mutex<Option<Child>>,
}

/// Persisted user choices (port + interface), stored next to the launcher's
/// config in the OS app-config directory.
#[derive(Debug, Clone, Serialize, Deserialize)]
struct Settings {
    port: u16,
    /// Interface name (`en0`) or `all` for 0.0.0.0.
    interface: String,
}

/// Static info about the supervised app, for the UI header.
#[derive(Debug, Clone, Serialize)]
struct AppInfo {
    name: String,
    default_port: u16,
    url_template: String,
    theme: std::collections::BTreeMap<String, String>,
}

/// The launcher's current status, mirrored into the panel.
#[derive(Debug, Clone, Serialize)]
struct Status {
    running: bool,
    url: String,
    host: String,
    port: u16,
    message: String,
}

/// Where the operator's port/interface choice is remembered, in the OS
/// app-config directory.
fn settings_path(app: &AppHandle) -> Result<std::path::PathBuf, String> {
    let dir = app
        .path()
        .app_config_dir()
        .map_err(|e| format!("resolving app config dir: {e}"))?;
    std::fs::create_dir_all(&dir).map_err(|e| format!("creating app config dir: {e}"))?;
    Ok(dir.join("settings.json"))
}

/// Read persisted settings, falling back to defaults on EVERY failure path —
/// unresolvable config dir, missing file, unparseable JSON. Deliberate, so a
/// corrupt settings file can't brick the launcher, and it means a reset port is
/// silent: nothing distinguishes "never saved" from "file is damaged".
fn load_settings(app: &AppHandle, default_port: u16) -> Settings {
    let fallback = Settings {
        port: default_port,
        interface: "all".into(),
    };
    let Ok(path) = settings_path(app) else {
        return fallback;
    };
    match std::fs::read_to_string(&path) {
        Ok(raw) => serde_json::from_str(&raw).unwrap_or(fallback),
        Err(_) => fallback,
    }
}

/// Persist settings. Unlike loading, this reports a write failure — though
/// nothing re-reads to confirm the value survived.
fn store_settings(app: &AppHandle, s: &Settings) -> Result<(), String> {
    let path = settings_path(app)?;
    let raw = serde_json::to_string_pretty(s).map_err(|e| e.to_string())?;
    std::fs::write(&path, raw).map_err(|e| format!("writing settings: {e}"))
}

#[tauri::command]
/// Static description of the supervised app, for the panel header: name,
/// default port, URL template and theme. Read from the launcher config, so it
/// changes only when the config does.
fn get_app_info() -> Result<AppInfo, String> {
    let cfg = config::load()?;
    Ok(AppInfo {
        name: cfg.app.name,
        default_port: cfg.app.default_port,
        url_template: cfg.app.url,
        theme: cfg.app.theme,
    })
}

#[tauri::command]
/// Bindable IPv4 interfaces plus the `all` (0.0.0.0) pseudo-entry.
///
/// Infallible by design — the picker must always have something to show, so an
/// enumeration problem yields a short list rather than an error the panel would
/// have to handle.
fn list_interfaces() -> Vec<config::Interface> {
    config::list_interfaces()
}

#[tauri::command]
/// The operator's remembered port and interface, or the config's defaults.
fn get_settings(app: AppHandle) -> Result<Settings, String> {
    let cfg = config::load()?;
    Ok(load_settings(&app, cfg.app.default_port))
}

#[tauri::command]
/// Remember a port/interface choice. Takes effect on the next start_server();
/// a running server is not restarted.
fn save_settings(app: AppHandle, port: u16, interface: String) -> Result<(), String> {
    store_settings(&app, &Settings { port, interface })
}

/// Compute status from settings without touching the child (used by the UI to
/// preview the URL before starting).
fn status_from(app: &AppHandle, running: bool, message: String) -> Result<Status, String> {
    let cfg = config::load()?;
    let s = load_settings(app, cfg.app.default_port);
    let (_bind, display) = config::resolve_hosts(&s.interface);
    let url = cfg
        .app
        .url
        .replace("{host}", &display)
        .replace("{port}", &s.port.to_string());
    Ok(Status {
        running,
        url,
        host: display,
        port: s.port,
        message,
    })
}

#[tauri::command]
/// Current launcher state, as the single object the panel re-renders from.
///
/// REAPS THE CHILD: it calls `try_wait()`, so a server that exited on its own is
/// noticed here rather than leaving the panel claiming it's running. That makes
/// this a side-effecting getter — the panel polls it, and removing the reap
/// would make the UI lie.
fn get_status(app: AppHandle, state: State<AppState>) -> Result<Status, String> {
    let mut guard = state.child.lock().map_err(|e| e.to_string())?;
    let running = match guard.as_mut() {
        Some(child) => match child.try_wait() {
            Ok(Some(_exited)) => {
                *guard = None; // process ended on its own
                false
            }
            Ok(None) => true,
            Err(_) => false,
        },
        None => false,
    };
    drop(guard);
    let msg = if running { "Running" } else { "Stopped" };
    status_from(&app, running, msg.into())
}

#[tauri::command]
/// Spawn the supervised server on the chosen interface and port.
///
/// Safe to call twice: an already-running child reports its current status
/// instead of being double-spawned. It does NOT restart, so a settings change
/// needs an explicit stop first.
///
/// The host:port reaches the server by whichever injection mode the launcher
/// config selects — patching a key in its TOML, environment variables, or
/// argv placeholders. See config.rs; the launcher itself knows nothing about
/// any particular server.
fn start_server(app: AppHandle, state: State<AppState>) -> Result<Status, String> {
    {
        // Already running? Report current status instead of double-spawning.
        let mut guard = state.child.lock().map_err(|e| e.to_string())?;
        if let Some(child) = guard.as_mut() {
            if matches!(child.try_wait(), Ok(None)) {
                drop(guard);
                return status_from(&app, true, "Running".into());
            }
        }
    }

    let cfg = config::load()?;
    let s = load_settings(&app, cfg.app.default_port);
    let (bind_host, _display) = config::resolve_hosts(&s.interface);

    let work_dir = app
        .path()
        .app_config_dir()
        .map_err(|e| format!("resolving app config dir: {e}"))?;
    let resource_dir = app.path().resource_dir().ok();
    let data_dir = app
        .path()
        .app_data_dir()
        .map_err(|e| format!("resolving app data dir: {e}"))?;

    // Unpacks the bundled WebLinked on first run, and after an upgrade. A no-op
    // on every launch after that, and a fallback to /Applications when nothing
    // is bundled — see embedded.rs for why it is unpacked outside the bundle.
    let runtime = embedded::ensure_unpacked(resource_dir.as_deref(), &data_dir)?;

    let launch = config::build_launch(
        &cfg,
        &bind_host,
        s.port,
        &work_dir,
        resource_dir.as_deref(),
        Some(&runtime.root),
    )?;

    // A binary bundled as a resource can lose its execute bit on some platforms;
    // restore it before spawning so a shipped bundle just works.
    #[cfg(unix)]
    ensure_executable(&launch.program);

    let mut cmd = Command::new(&launch.program);
    cmd.args(&launch.args);
    for (k, v) in &launch.envs {
        cmd.env(k, v);
    }
    if let Some(cwd) = &launch.cwd {
        std::fs::create_dir_all(cwd).ok();
        cmd.current_dir(cwd);
    }

    let child = cmd
        .spawn()
        .map_err(|e| format!("starting {}: {e}", launch.program))?;
    *state.child.lock().map_err(|e| e.to_string())? = Some(child);

    // Which WebLinked is running is worth saying. The two cases behave
    // identically until they don't: a separately installed copy can be a
    // different version from the launcher that started it, and that mismatch is
    // otherwise invisible right up to the point it explains a missing feature.
    let message = if runtime.fallback {
        "Running (installed copy)".to_string()
    } else {
        "Running".to_string()
    };
    status_from(&app, true, message)
}

#[tauri::command]
/// Kill the supervised server. There is no graceful-shutdown signal, so a
/// server that writes state on exit gets no chance to.
fn stop_server(app: AppHandle, state: State<AppState>) -> Result<Status, String> {
    let mut guard = state.child.lock().map_err(|e| e.to_string())?;
    if let Some(mut child) = guard.take() {
        let _ = child.kill();
        let _ = child.wait();
    }
    drop(guard);
    status_from(&app, false, "Stopped".into())
}

#[tauri::command]
/// Open the server's web UI in the default browser, resolving the URL fresh
/// rather than reusing whatever the panel last rendered.
fn open_gui(app: AppHandle) -> Result<(), String> {
    let status = status_from(&app, false, String::new())?;
    tauri_plugin_opener::open_url(status.url, None::<&str>)
        .map_err(|e| format!("opening browser: {e}"))
}

#[tauri::command]
/// Kill the server and exit. Distinct from hide_window(), which leaves it
/// running in the tray — the difference an operator most often gets wrong.
fn quit_app(app: AppHandle, state: State<AppState>) {
    if let Ok(mut guard) = state.child.lock() {
        if let Some(mut child) = guard.take() {
            let _ = child.kill();
        }
    }
    app.exit(0);
}

#[tauri::command]
/// Hide the panel back to the tray. THE SERVER KEEPS RUNNING.
fn hide_window(app: AppHandle) {
    if let Some(w) = app.get_webview_window("main") {
        let _ = w.hide();
    }
}

fn show_main(app: &AppHandle) {
    if let Some(w) = app.get_webview_window("main") {
        let _ = w.show();
        let _ = w.set_focus();
    }
}

#[cfg(unix)]
/// Restore the execute bit on a bundled binary if packaging stripped it.
///
/// A binary shipped as a Tauri resource can lose its execute bit on some
/// platforms, and the symptom is indistinguishable from the app simply not
/// working. Best-effort: every failure is swallowed, because a server that
/// already has its bit set is the normal case.
///
/// Note this does NOT address the macOS Gatekeeper trap, which is a different
/// failure with the same shape: for an unsigned .app bundling helper binaries,
/// approving the app does not unquarantine its payload and the helpers are
/// SIGKILLed silently. No permission change fixes that — see the README.
fn ensure_executable(path: &str) {
    use std::os::unix::fs::PermissionsExt;
    if let Ok(meta) = std::fs::metadata(path) {
        let mode = meta.permissions().mode();
        if mode & 0o111 == 0 {
            let _ = std::fs::set_permissions(path, std::fs::Permissions::from_mode(mode | 0o755));
        }
    }
}

/// Pin the config path so `config::load()` (which has no app handle) finds it.
/// Precedence: existing `$AV_LAUNCHER_CONFIG` > `./launcher.toml` (dev) >
/// the bundled `launcher.toml` in the resource dir.
fn pin_config_path(app: &AppHandle) {
    if std::env::var_os("AV_LAUNCHER_CONFIG").is_some() {
        return;
    }
    if std::env::current_dir()
        .map(|d| d.join("launcher.toml").exists())
        .unwrap_or(false)
    {
        return; // find_config_path will pick up ./launcher.toml
    }
    if let Ok(res) = app.path().resource_dir() {
        let bundled = res.join("launcher.toml");
        if bundled.exists() {
            std::env::set_var("AV_LAUNCHER_CONFIG", bundled);
        }
    }
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .manage(AppState::default())
        .invoke_handler(tauri::generate_handler![
            get_app_info,
            list_interfaces,
            get_settings,
            save_settings,
            get_status,
            start_server,
            stop_server,
            open_gui,
            hide_window,
            quit_app,
        ])
        .setup(|app| {
            pin_config_path(&app.handle().clone());

            // Name the tray after the app being launched, when we can read it.
            let app_name = config::load().map(|c| c.app.name).unwrap_or_else(|_| "Launcher".into());

            // Tray menu: Show / Quit.
            let show = MenuItem::with_id(app, "show", "Show", true, None::<&str>)?;
            let quit = MenuItem::with_id(app, "quit", "Quit", true, None::<&str>)?;
            let menu = Menu::with_items(app, &[&show, &quit])?;

            TrayIconBuilder::new()
                .icon(app.default_window_icon().unwrap().clone())
                .tooltip(&app_name)
                .menu(&menu)
                .show_menu_on_left_click(false)
                .on_menu_event(|app, event| match event.id.as_ref() {
                    "show" => show_main(app),
                    "quit" => {
                        if let Some(state) = app.try_state::<AppState>() {
                            if let Ok(mut guard) = state.child.lock() {
                                if let Some(mut child) = guard.take() {
                                    let _ = child.kill();
                                }
                            }
                        }
                        app.exit(0);
                    }
                    _ => {}
                })
                .on_tray_icon_event(|tray, event| {
                    if let TrayIconEvent::Click {
                        button: MouseButton::Left,
                        button_state: MouseButtonState::Up,
                        ..
                    } = event
                    {
                        show_main(tray.app_handle());
                    }
                })
                .build(app)?;

            Ok(())
        })
        // Closing the window hides it to the tray instead of quitting.
        .on_window_event(|window, event| {
            if let WindowEvent::CloseRequested { api, .. } = event {
                let _ = window.hide();
                api.prevent_close();
            }
        })
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
