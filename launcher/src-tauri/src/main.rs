// Prevents additional console window on Windows in release, DO NOT REMOVE!!
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

fn main() {
    // Before anything that can fail, so a failure during startup is logged and
    // lands in a crash report like any other. A tray app has no console for an
    // error to appear in, which makes this the only record there will be.
    let _diag = match diag::init(diag::Options::new(
        "WebLinked Launcher",
        "WEBLINKED_LAUNCHER",
        env!("CARGO_PKG_VERSION"),
    )) {
        Ok(guard) => Some(guard),
        // Logging failing is not a reason to refuse to launch anything.
        Err(e) => {
            eprintln!("weblinked-launcher: logging unavailable: {e}");
            None
        }
    };

    if std::env::args().any(|a| a == "--collect-diagnostics") {
        match diag::collect_diagnostics() {
            Ok(path) => println!("{}", path.display()),
            Err(e) => eprintln!("could not collect diagnostics: {e}"),
        }
        return;
    }

    weblinked_launcher_lib::run()
}
