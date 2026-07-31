//! Reusable, app-agnostic launcher configuration.
//!
//! The launcher itself knows nothing about any particular server. Each app it
//! supervises is described by a small `launcher.toml`, which says how to start
//! the server binary and — crucially — *how to inject the chosen host:port*.
//! Three injection modes cover the whole fleet:
//!
//! * `configfile` — patch a key in the app's own TOML config (srt-router's
//!   `[web] bind`), then pass that rendered file via `--config`.
//! * `env` — set environment variables (RFutils' `RFUTILS_SERVER_PORT`).
//! * `args` — the `{host}`/`{port}` placeholders are already in `[app].args`.

use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

/// Top-level `launcher.toml` schema.
#[derive(Debug, Clone, Deserialize)]
pub struct LauncherConfig {
    pub app: AppSpec,
    pub inject: InjectSpec,
}

#[derive(Debug, Clone, Deserialize)]
pub struct AppSpec {
    /// Display name shown in the panel and tray.
    pub name: String,
    /// Absolute path to the server binary (or a command on PATH).
    pub command: String,
    /// Arguments; supports `{host}`, `{port}` and `{config}` placeholders.
    #[serde(default)]
    pub args: Vec<String>,
    /// URL template shown to the user, e.g. `http://{host}:{port}/`.
    #[serde(default = "default_url")]
    pub url: String,
    /// Default port pre-filled in the UI.
    #[serde(default = "default_port")]
    pub default_port: u16,
    /// Working directory for the child (so relative paths in its config
    /// resolve). Optional; defaults to the binary's directory.
    #[serde(default)]
    pub cwd: Option<String>,
    /// Optional palette (CSS custom-property name -> value) applied to the
    /// panel so each launcher matches its app's own web UI. Keys like
    /// `bg`, `panel`, `border`, `text`, `muted`, `accent`, `accent-soft`,
    /// `good`. Anything omitted falls back to the shell's built-in defaults.
    #[serde(default)]
    pub theme: BTreeMap<String, String>,
}

#[derive(Debug, Clone, Deserialize)]
pub struct InjectSpec {
    /// `configfile` | `env` | `args`.
    pub mode: String,
    #[serde(default)]
    pub configfile: Option<ConfigFileInject>,
    /// For `env` mode: variable name -> value template (`{host}`/`{port}`).
    #[serde(default)]
    pub env: BTreeMap<String, String>,
}

#[derive(Debug, Clone, Deserialize)]
pub struct ConfigFileInject {
    /// Path to the app's own config file, used as a template.
    pub template: String,
    /// Dotted key to overwrite, e.g. `web.bind`.
    pub set_key: String,
    /// Value template written at `set_key`. Defaults to `{host}:{port}`.
    #[serde(default = "default_bind_value")]
    pub value: String,
}

fn default_url() -> String {
    "http://{host}:{port}/".into()
}
fn default_port() -> u16 {
    8080
}
fn default_bind_value() -> String {
    "{host}:{port}".into()
}

/// A network interface offered in the "GUI Interface" picker.
#[derive(Debug, Clone, Serialize)]
pub struct Interface {
    /// Interface name, e.g. `en0`, or `all` for the 0.0.0.0 pseudo-entry.
    pub name: String,
    /// IPv4 address to bind to (`0.0.0.0` for the "all" entry).
    pub ip: String,
    /// Human label, e.g. `en0: 10.147.17.93`.
    pub label: String,
    pub loopback: bool,
}

/// The concrete command to spawn, after host:port injection.
#[derive(Debug, Clone)]
pub struct Launch {
    pub program: String,
    pub args: Vec<String>,
    pub envs: Vec<(String, String)>,
    pub cwd: Option<PathBuf>,
}

/// Substitute `{host}`/`{port}` (plus `{config}`/`{resource}` when provided).
/// `{resource}` is the bundle's resource dir — lets a shipped config point at
/// bundled files (e.g. an embedded Node runtime + server) by absolute path.
fn subst(
    s: &str,
    host: &str,
    port: u16,
    config: Option<&str>,
    resource: Option<&str>,
) -> String {
    let mut out = s
        .replace("{host}", host)
        .replace("{port}", &port.to_string());
    if let Some(r) = resource {
        out = out.replace("{resource}", r);
    }
    if let Some(c) = config {
        out = out.replace("{config}", c);
    }
    out
}

/// Locate `launcher.toml`: `$AV_LAUNCHER_CONFIG`, else `./launcher.toml`
/// (the working dir — `src-tauri` under `tauri dev`), else next to the exe.
pub fn find_config_path() -> Result<PathBuf, String> {
    if let Ok(p) = std::env::var("AV_LAUNCHER_CONFIG") {
        return Ok(PathBuf::from(p));
    }
    if let Ok(cwd) = std::env::current_dir() {
        let p = cwd.join("launcher.toml");
        if p.exists() {
            return Ok(p);
        }
    }
    if let Ok(exe) = std::env::current_exe() {
        if let Some(dir) = exe.parent() {
            let p = dir.join("launcher.toml");
            if p.exists() {
                return Ok(p);
            }
        }
    }
    Err("launcher.toml not found (set AV_LAUNCHER_CONFIG or place it in the working directory)".into())
}

/// Parse the launcher configuration.
pub fn load() -> Result<LauncherConfig, String> {
    let path = find_config_path()?;
    let raw = std::fs::read_to_string(&path)
        .map_err(|e| format!("reading {}: {e}", path.display()))?;
    toml::from_str::<LauncherConfig>(&raw)
        .map_err(|e| format!("parsing {}: {e}", path.display()))
}

/// Enumerate bindable IPv4 interfaces, with an "All interfaces" entry first.
pub fn list_interfaces() -> Vec<Interface> {
    let mut out = vec![Interface {
        name: "all".into(),
        ip: "0.0.0.0".into(),
        label: "All interfaces (0.0.0.0)".into(),
        loopback: false,
    }];
    if let Ok(addrs) = if_addrs::get_if_addrs() {
        for a in addrs {
            // IPv4 only for now — matches how these servers advertise a URL.
            if let std::net::IpAddr::V4(v4) = a.ip() {
                out.push(Interface {
                    name: a.name.clone(),
                    ip: v4.to_string(),
                    label: format!("{}: {}", a.name, v4),
                    loopback: a.is_loopback(),
                });
            }
        }
    }
    out
}

/// The first non-loopback IPv4, used as the display host for "All interfaces".
pub fn primary_ip() -> String {
    list_interfaces()
        .into_iter()
        .find(|i| i.name != "all" && !i.loopback)
        .map(|i| i.ip)
        .unwrap_or_else(|| "127.0.0.1".into())
}

/// Resolve a chosen interface name into (bind_host, display_host).
/// `bind_host` is what the server binds; `display_host` is what the URL shows.
pub fn resolve_hosts(interface: &str) -> (String, String) {
    let ifaces = list_interfaces();
    match ifaces.iter().find(|i| i.name == interface) {
        Some(i) if i.name == "all" => ("0.0.0.0".into(), primary_ip()),
        Some(i) => (i.ip.clone(), i.ip.clone()),
        // Interface vanished (cable unplugged); fall back to all-interfaces.
        None => ("0.0.0.0".into(), primary_ip()),
    }
}

/// Set a dotted key (`web.bind`) in a TOML document, creating tables as needed.
fn set_dotted(doc: &mut toml_edit::DocumentMut, dotted: &str, val: &str) {
    let parts: Vec<&str> = dotted.split('.').collect();
    let mut node = doc.as_item_mut();
    for p in &parts[..parts.len().saturating_sub(1)] {
        node = &mut node[*p];
    }
    if let Some(last) = parts.last() {
        node[*last] = toml_edit::value(val);
    }
}

/// Resolve a possibly-relative path against `base` (the bundle's resource dir).
/// Absolute paths are returned unchanged, so dev configs with absolute paths and
/// shipped bundles with relative (bundled-resource) paths both work.
fn resolve_against(path: &str, base: Option<&Path>) -> String {
    let p = Path::new(path);
    if p.is_absolute() {
        return path.to_string();
    }
    match base {
        Some(dir) => dir.join(p).to_string_lossy().into_owned(),
        None => path.to_string(),
    }
}

/// On Windows a bundled command ships with a `.exe` extension (e.g. `node.exe`),
/// but `launcher.toml` names it without one (`node`, `bin/flock`). If the
/// extension-less program isn't present and a `.exe` sibling is, prefer that so
/// the spawn finds the executable. No-op on other platforms.
#[cfg(windows)]
fn with_windows_exe(program: String) -> String {
    let p = Path::new(&program);
    if p.extension().is_none() && !p.exists() {
        let exe = format!("{program}.exe");
        if Path::new(&exe).exists() {
            return exe;
        }
    }
    program
}

#[cfg(not(windows))]
fn with_windows_exe(program: String) -> String {
    program
}

/// Build the concrete [`Launch`] for the given host/port, performing whatever
/// injection the app's `launcher.toml` calls for.
///
/// * `work_dir` — writable dir for the rendered config and the default cwd
///   (the launcher's app-config directory).
/// * `resource_dir` — the bundle's resource dir; relative `command`/`template`
///   paths resolve against it, so a shipped `.app` can carry its server binary
///   and config template as bundled resources. `None` in dev (absolute paths).
pub fn build_launch(
    cfg: &LauncherConfig,
    bind_host: &str,
    port: u16,
    work_dir: &Path,
    resource_dir: Option<&Path>,
) -> Result<Launch, String> {
    let mut envs: Vec<(String, String)> = Vec::new();
    let mut rendered_config: Option<String> = None;
    let res_str = resource_dir.map(|p| p.to_string_lossy().into_owned());
    let res = res_str.as_deref();

    match cfg.inject.mode.as_str() {
        "configfile" => {
            let ci = cfg.inject.configfile.as_ref().ok_or(
                "inject.mode = \"configfile\" but [inject.configfile] is missing",
            )?;
            let template = resolve_against(&ci.template, resource_dir);
            let raw = std::fs::read_to_string(&template)
                .map_err(|e| format!("reading template {template}: {e}"))?;
            let mut doc = raw
                .parse::<toml_edit::DocumentMut>()
                .map_err(|e| format!("parsing template {template}: {e}"))?;
            let value = subst(&ci.value, bind_host, port, None, res);
            set_dotted(&mut doc, &ci.set_key, &value);

            std::fs::create_dir_all(work_dir)
                .map_err(|e| format!("creating work dir: {e}"))?;
            let out = work_dir.join("rendered-config.toml");
            std::fs::write(&out, doc.to_string())
                .map_err(|e| format!("writing rendered config: {e}"))?;
            rendered_config = Some(out.to_string_lossy().into_owned());
        }
        "env" => {
            for (k, v) in &cfg.inject.env {
                envs.push((k.clone(), subst(v, bind_host, port, None, res)));
            }
        }
        "args" => { /* host/port already substituted into args below */ }
        other => return Err(format!("unknown inject.mode: {other}")),
    }

    let args = cfg
        .app
        .args
        .iter()
        .map(|a| subst(a, bind_host, port, rendered_config.as_deref(), res))
        .collect();

    // Substitute {resource} in the command, then resolve any remaining relative
    // path against the resource dir (covers both `{resource}/node` and `bin/x`).
    let program = with_windows_exe(resolve_against(
        &subst(&cfg.app.command, bind_host, port, None, res),
        resource_dir,
    ));

    // Prefer an explicit cwd; otherwise run from the writable work dir so a
    // bundled server can persist state (it can't write inside a read-only .app).
    let cwd = cfg
        .app
        .cwd
        .as_ref()
        .map(PathBuf::from)
        .unwrap_or_else(|| work_dir.to_path_buf());

    Ok(Launch {
        program,
        args,
        envs,
        cwd: Some(cwd),
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn parse(toml: &str) -> LauncherConfig {
        toml::from_str(toml).expect("valid launcher config")
    }

    /// flock: config-file injection into a TOP-LEVEL `bind` key, config passed
    /// as a positional arg. Regression guard for flock's shape vs srt-router's.
    #[test]
    fn flock_configfile_toplevel_bind() {
        let tmp = std::env::temp_dir().join("av-launcher-test-flock");
        std::fs::create_dir_all(&tmp).unwrap();
        let template = tmp.join("flock.example.toml");
        std::fs::write(
            &template,
            "bind = \"0.0.0.0:8080\"\nregistry_path = \"data/registry.json\"\n",
        )
        .unwrap();

        let cfg = parse(&format!(
            r#"
            [app]
            name = "flock"
            command = "/bin/flock"
            args = ["{{config}}"]
            [inject]
            mode = "configfile"
            [inject.configfile]
            template = "{}"
            set_key = "bind"
            value = "{{host}}:{{port}}"
            "#,
            template.display()
        ));

        let launch = build_launch(&cfg, "10.0.0.5", 9000, &tmp, None).unwrap();
        // The single positional arg is the rendered config path.
        assert_eq!(launch.args.len(), 1);
        let rendered = std::fs::read_to_string(&launch.args[0]).unwrap();
        assert!(
            rendered.contains("bind = \"10.0.0.5:9000\""),
            "top-level bind not patched: {rendered}"
        );
        // Untouched keys are preserved.
        assert!(rendered.contains("registry_path = \"data/registry.json\""));
    }

    /// srt-router: config-file injection into a NESTED `web.bind` key.
    #[test]
    fn srt_router_configfile_nested_bind() {
        let tmp = std::env::temp_dir().join("av-launcher-test-srt");
        std::fs::create_dir_all(&tmp).unwrap();
        let template = tmp.join("srt.example.toml");
        std::fs::write(&template, "[web]\nbind = \"0.0.0.0:8080\"\n").unwrap();

        let cfg = parse(&format!(
            r#"
            [app]
            name = "SRT Router"
            command = "/bin/srtrouter"
            args = ["--config", "{{config}}"]
            [inject]
            mode = "configfile"
            [inject.configfile]
            template = "{}"
            set_key = "web.bind"
            value = "{{host}}:{{port}}"
            "#,
            template.display()
        ));

        let launch = build_launch(&cfg, "0.0.0.0", 8080, &tmp, None).unwrap();
        assert_eq!(launch.args[0], "--config");
        let rendered = std::fs::read_to_string(&launch.args[1]).unwrap();
        assert!(
            rendered.contains("bind = \"0.0.0.0:8080\""),
            "nested web.bind not patched: {rendered}"
        );
    }

    /// RFutils: env injection, no config file touched.
    #[test]
    fn rfutils_env_injection() {
        let tmp = std::env::temp_dir().join("av-launcher-test-env");
        let cfg = parse(
            r#"
            [app]
            name = "RFutils"
            command = "node"
            args = ["server.js"]
            [inject]
            mode = "env"
            [inject.env]
            RFUTILS_SERVER_PORT = "{port}"
            RFUTILS_HOST = "{host}"
            "#,
        );

        let launch = build_launch(&cfg, "192.168.1.20", 8420, &tmp, None).unwrap();
        assert!(launch
            .envs
            .contains(&("RFUTILS_SERVER_PORT".into(), "8420".into())));
        assert!(launch
            .envs
            .contains(&("RFUTILS_HOST".into(), "192.168.1.20".into())));
    }

    /// {resource} in command + args resolves to the bundle resource dir
    /// (how the RFutils bundle points at its embedded Node + server).
    #[test]
    fn resource_placeholder_in_command_and_args() {
        let res = std::env::temp_dir().join("av-launcher-test-resph");
        let work = std::env::temp_dir().join("av-launcher-test-resph-work");
        let cfg = parse(
            r#"
            [app]
            name = "RFutils"
            command = "{resource}/node"
            args = ["{resource}/app/server.mjs"]
            [inject]
            mode = "env"
            [inject.env]
            PORT = "{port}"
            "#,
        );
        let launch = build_launch(&cfg, "0.0.0.0", 8420, &work, Some(&res)).unwrap();
        assert_eq!(launch.program, format!("{}/node", res.to_string_lossy()));
        assert_eq!(launch.args, vec![format!("{}/app/server.mjs", res.to_string_lossy())]);
        assert!(launch.envs.contains(&("PORT".into(), "8420".into())));
    }

    /// The config this repo actually ships, parsed from the file on disk.
    ///
    /// A typo in launcher.toml is not a compile error and not a test failure
    /// anywhere else — it is a tray app that silently launches nothing. This
    /// asserts the real file produces the real argv.
    #[test]
    fn shipped_weblinked_config_produces_the_right_argv() {
        let text = include_str!("../launcher.toml");
        let cfg = parse(text);
        assert_eq!(cfg.app.name, "WebLinked");
        assert_eq!(cfg.inject.mode, "args");
        assert_eq!(cfg.app.default_port, 7654);

        let tmp = std::env::temp_dir().join("weblinked-launcher-test-shipped");
        let launch = build_launch(&cfg, "10.0.0.7", 7654, &tmp, None).unwrap();

        // --headless matters: without it WebLinked opens its own operator
        // window and the tray is supervising a process that has already put a
        // second, unmanaged copy of the UI on screen.
        assert!(launch.args.contains(&"--headless".to_string()));
        assert_eq!(
            launch.args,
            vec!["--headless", "--bind", "10.0.0.7", "--port", "7654"]
        );
        // The saved settings file must keep working, so --no-settings must not
        // creep in here.
        assert!(!launch.args.iter().any(|a| a == "--no-settings"));
    }

    /// args mode: {host}/{port} substituted directly into argv.
    #[test]
    fn args_injection() {
        let tmp = std::env::temp_dir().join("av-launcher-test-args");
        let cfg = parse(
            r#"
            [app]
            name = "Plain"
            command = "server"
            args = ["--host", "{host}", "--port", "{port}"]
            [inject]
            mode = "args"
            "#,
        );
        let launch = build_launch(&cfg, "127.0.0.1", 7000, &tmp, None).unwrap();
        assert_eq!(launch.args, vec!["--host", "127.0.0.1", "--port", "7000"]);
    }

    /// Shipped bundle: a relative `command` + `template` resolve against the
    /// resource dir, and cwd defaults to the writable work dir.
    #[test]
    fn bundled_relative_paths_resolve_against_resource_dir() {
        let res = std::env::temp_dir().join("av-launcher-test-res");
        let work = std::env::temp_dir().join("av-launcher-test-res-work");
        std::fs::create_dir_all(&res).unwrap();
        std::fs::write(res.join("server-config.toml"), "bind = \"0.0.0.0:8080\"\n").unwrap();

        let cfg = parse(
            r#"
            [app]
            name = "flock"
            command = "flock"
            args = ["{config}"]
            [inject]
            mode = "configfile"
            [inject.configfile]
            template = "server-config.toml"
            set_key = "bind"
            value = "{host}:{port}"
            "#,
        );
        let launch = build_launch(&cfg, "0.0.0.0", 8080, &work, Some(&res)).unwrap();
        assert_eq!(launch.program, res.join("flock").to_string_lossy());
        assert_eq!(launch.cwd, Some(work.clone()));
        let rendered = std::fs::read_to_string(&launch.args[0]).unwrap();
        assert!(rendered.contains("bind = \"0.0.0.0:8080\""));
    }
}
