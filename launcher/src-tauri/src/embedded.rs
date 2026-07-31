//! Unpacking the WebLinked bundle that ships inside this launcher.
//!
//! # Why an archive and not a plain directory
//!
//! Tauri's resource collector walks the tree it is told to bundle. The Chromium
//! Embedded Framework is a macOS framework, so `Versions/Current` and
//! `Resources` are symlinks; the walk follows them and fails on a path that
//! does not exist (`.../Resources/Resources`). Shipping one `.zip` gives the
//! collector a single regular file and the problem disappears.
//!
//! # Why it is unpacked *outside* the bundle
//!
//! This is the part that matters, and it is not a detail. An ad-hoc signed
//! `.app` that nests further executables inside itself does not make them
//! trusted when the user approves the outer app: the nested helpers are
//! SIGKILLed with no dialog and nothing in any log, which reads exactly like a
//! CEF initialisation bug. WebLinked carries five name-matched helper `.app`s,
//! so nesting it is the worst case of that.
//!
//! Unpacking to Application Support sidesteps it: the result is not nested in
//! anyone's bundle, so nothing about the outer app's trust applies to it.
//!
//! # What was measured, and what was therefore removed
//!
//! An earlier version of this also re-signed the unpacked bundle
//! (`codesign --force --deep --sign -`) on the theory that the helper
//! signatures needed re-establishing at the new path. Measured on macOS 26,
//! that is simply not true:
//!
//!   * the ad-hoc signature round-trips the archive intact — the unpacked
//!     bundle still reports `flags=0x2(adhoc)` with its original identifier;
//!   * the extracted files carry no `com.apple.quarantine`;
//!   * it runs, and the renderer and GPU helpers stay alive, with no
//!     post-processing of any kind.
//!
//! So the re-sign was dropped. It was not merely redundant: `--deep` is
//! deprecated for signing and contradicts this project's own rule that macOS
//! bundles are signed inside-out (see `cmake/SignMacBundle.cmake`), and it was
//! the step most likely to break quietly on a future macOS.
//!
//! The quarantine clear below stays, because it defends the one case that
//! cannot be tested from a source checkout — see the note on it.
//!
//! # Why `ditto` rather than a zip crate on macOS
//!
//! `ditto` is the one that round-trips a framework: symlinks stay symlinks,
//! the execute bits survive, and resource forks are preserved. A generic zip
//! extractor flattens the symlinks into copies, which quadruples the size and
//! breaks the signature.

use std::path::{Path, PathBuf};

/// Name of the archive inside the bundle's resource dir. Written by CI —
/// see `.github/workflows/release.yml`.
#[cfg(target_os = "macos")]
pub const ARCHIVE_NAME: &str = "WebLinked.app.zip";
#[cfg(not(target_os = "macos"))]
pub const ARCHIVE_NAME: &str = "WebLinked.zip";

/// Where a dev build looks when no archive is bundled, so `tauri dev` and a
/// source checkout keep working exactly as they did before embedding.
#[cfg(target_os = "macos")]
pub const FALLBACK_INSTALL: &str = "/Applications/WebLinked.app";

/// What the launcher unpacked, and where.
#[derive(Debug, Clone)]
pub struct Runtime {
    /// The directory the archive was expanded into.
    pub root: PathBuf,
    /// True when nothing was bundled and the fallback is in play.
    pub fallback: bool,
}

fn stamp_path(data_dir: &Path) -> PathBuf {
    data_dir.join("runtime").join(".unpacked")
}

/// Identifies the archive currently bundled: length plus mtime.
///
/// Not a hash. Hashing 100-odd MB on every launch to answer "is this the same
/// file as last time" would cost more than it saves, and the archive only ever
/// changes when a new launcher is installed over the old one.
fn archive_stamp(archive: &Path) -> Result<String, String> {
    let meta = std::fs::metadata(archive)
        .map_err(|e| format!("reading {}: {e}", archive.display()))?;
    let modified = meta
        .modified()
        .ok()
        .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
        .map(|d| d.as_secs())
        .unwrap_or(0);
    Ok(format!("{}:{}", meta.len(), modified))
}

/// Ensures the embedded WebLinked is unpacked and runnable, returning where.
///
/// Cheap on every launch after the first: it compares a stamp file and returns.
pub fn ensure_unpacked(
    resource_dir: Option<&Path>,
    data_dir: &Path,
) -> Result<Runtime, String> {
    let archive = match resource_dir.map(|r| r.join(ARCHIVE_NAME)) {
        Some(p) if p.exists() => p,
        _ => {
            // No archive bundled. A dev build, or a deliberately slim one.
            #[cfg(target_os = "macos")]
            {
                return Ok(Runtime {
                    root: PathBuf::from(FALLBACK_INSTALL)
                        .parent()
                        .unwrap_or(Path::new("/Applications"))
                        .to_path_buf(),
                    fallback: true,
                });
            }
            #[cfg(not(target_os = "macos"))]
            {
                return Ok(Runtime {
                    root: data_dir.join("runtime"),
                    fallback: true,
                });
            }
        }
    };

    let destination = data_dir.join("runtime");
    let stamp = stamp_path(data_dir);
    let wanted = archive_stamp(&archive)?;

    if let Ok(existing) = std::fs::read_to_string(&stamp) {
        if existing.trim() == wanted {
            return Ok(Runtime { root: destination, fallback: false });
        }
    }

    // Stale or first run. Clear the old tree first: unpacking over the top
    // would leave files from a previous version behind, and a stray helper
    // .app with a mismatched signature is worse than no helper at all.
    if destination.exists() {
        std::fs::remove_dir_all(&destination)
            .map_err(|e| format!("clearing {}: {e}", destination.display()))?;
    }
    std::fs::create_dir_all(&destination)
        .map_err(|e| format!("creating {}: {e}", destination.display()))?;

    unpack(&archive, &destination)?;

    #[cfg(target_os = "macos")]
    prepare_macos_bundle(&destination)?;

    // Written last, and only on success, so a run that dies halfway through
    // unpacking retries next time instead of trusting a partial tree.
    std::fs::write(&stamp, &wanted)
        .map_err(|e| format!("writing {}: {e}", stamp.display()))?;

    Ok(Runtime { root: destination, fallback: false })
}

#[cfg(target_os = "macos")]
fn unpack(archive: &Path, destination: &Path) -> Result<(), String> {
    let output = std::process::Command::new("/usr/bin/ditto")
        .arg("-x")
        .arg("-k")
        .arg(archive)
        .arg(destination)
        .output()
        .map_err(|e| format!("running ditto: {e}"))?;
    if !output.status.success() {
        return Err(format!(
            "unpacking {}: {}",
            archive.display(),
            String::from_utf8_lossy(&output.stderr).trim()
        ));
    }
    Ok(())
}

#[cfg(not(target_os = "macos"))]
fn unpack(archive: &Path, destination: &Path) -> Result<(), String> {
    let file = std::fs::File::open(archive)
        .map_err(|e| format!("opening {}: {e}", archive.display()))?;
    let mut zip = zip::ZipArchive::new(file)
        .map_err(|e| format!("reading {}: {e}", archive.display()))?;
    zip.extract(destination)
        .map_err(|e| format!("unpacking {}: {e}", archive.display()))?;

    // The zip crate restores unix modes, but only when the archive carries
    // them; a zip built on Windows does not. Re-arm the executable bit on the
    // launch target rather than trusting it survived.
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let binary = destination.join("WebLinked");
        if let Ok(meta) = std::fs::metadata(&binary) {
            let mut perms = meta.permissions();
            perms.set_mode(perms.mode() | 0o111);
            let _ = std::fs::set_permissions(&binary, perms);
        }
    }
    Ok(())
}

/// Clears any quarantine flag on the unpacked bundle.
///
/// Deliberately best-effort, and deliberately still here even though a source
/// checkout shows it doing nothing.
///
/// What cannot be tested from a checkout is the shipped path: a launcher `.dmg`
/// downloaded from GitHub *is* quarantined, the archive inside it inherits
/// that, and whether `ditto` then propagates it to the extracted tree depends
/// on macOS version and on how the download was made. If it ever does, the
/// symptom is the silent-SIGKILL-with-no-log described at the top of this file
/// — which is a genuinely horrible thing to debug and a one-line thing to
/// prevent.
///
/// Errors are ignored on purpose. `xattr -r` walks into the CEF framework's
/// symlinks and reports "No such file" for `Versions/A/Resources/Resources`
/// and a symlink loop for `Versions/A/A`. Both are noise from following links
/// that exist only as targets; the attribute removal on the real files still
/// happens.
#[cfg(target_os = "macos")]
fn prepare_macos_bundle(destination: &Path) -> Result<(), String> {
    let app = destination.join("WebLinked.app");
    if !app.exists() {
        return Err(format!(
            "the archive did not contain WebLinked.app (looked in {})",
            destination.display()
        ));
    }

    let _ = std::process::Command::new("/usr/bin/xattr")
        .arg("-dr")
        .arg("com.apple.quarantine")
        .arg(&app)
        .output();

    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    /// With nothing bundled, the launcher must fall back rather than error —
    /// this is every `tauri dev` run and every source checkout.
    fn a_missing_archive_falls_back_instead_of_failing() {
        let tmp = std::env::temp_dir().join(format!("wl-embed-{}", std::process::id()));
        std::fs::create_dir_all(&tmp).unwrap();
        let runtime = ensure_unpacked(None, &tmp).expect("fallback must not error");
        assert!(runtime.fallback);
        let _ = std::fs::remove_dir_all(&tmp);
    }

    #[test]
    /// A resource dir that exists but holds no archive is the same case.
    fn an_empty_resource_dir_also_falls_back() {
        let tmp = std::env::temp_dir().join(format!("wl-embed-res-{}", std::process::id()));
        let res = tmp.join("resources");
        std::fs::create_dir_all(&res).unwrap();
        let runtime = ensure_unpacked(Some(&res), &tmp).expect("fallback must not error");
        assert!(runtime.fallback);
        let _ = std::fs::remove_dir_all(&tmp);
    }

    #[cfg(target_os = "macos")]
    /// Builds a stand-in for the shipped archive: a WebLinked.app-shaped
    /// directory packed exactly the way CI packs the real one.
    fn make_archive(dir: &Path, marker: &str) -> PathBuf {
        let staged = dir.join("stage");
        let app = staged.join("WebLinked.app").join("Contents").join("MacOS");
        std::fs::create_dir_all(&app).unwrap();
        std::fs::write(app.join("WebLinked"), marker).unwrap();
        let archive = dir.join(ARCHIVE_NAME);
        let _ = std::fs::remove_file(&archive);
        let status = std::process::Command::new("/usr/bin/ditto")
            .args(["-c", "-k", "--sequesterRsrc", "--keepParent"])
            .arg(staged.join("WebLinked.app"))
            .arg(&archive)
            .status()
            .unwrap();
        assert!(status.success(), "ditto failed to build the test archive");
        std::fs::remove_dir_all(&staged).unwrap();
        archive
    }

    #[test]
    #[cfg(target_os = "macos")]
    /// The whole path: unpack, short-circuit on the stamp, then re-unpack when
    /// the archive changes. This is what the Start button runs, minus Tauri's
    /// own resource_dir() lookup.
    fn it_unpacks_then_short_circuits_then_re_unpacks_on_upgrade() {
        let tmp = std::env::temp_dir().join(format!("wl-embed-full-{}", std::process::id()));
        let res = tmp.join("resources");
        std::fs::create_dir_all(&res).unwrap();

        make_archive(&res, "version-one");
        let first = ensure_unpacked(Some(&res), &tmp).expect("first unpack");
        assert!(!first.fallback);
        let binary = first.root.join("WebLinked.app/Contents/MacOS/WebLinked");
        assert_eq!(std::fs::read_to_string(&binary).unwrap(), "version-one");
        assert!(stamp_path(&tmp).exists(), "the stamp must be written on success");

        // Second call must not redo the work. Detected by scribbling on the
        // unpacked tree: if it were re-extracted, the edit would be gone.
        std::fs::write(&binary, "touched-by-test").unwrap();
        ensure_unpacked(Some(&res), &tmp).expect("second call");
        assert_eq!(
            std::fs::read_to_string(&binary).unwrap(),
            "touched-by-test",
            "an unchanged archive must not be unpacked again"
        );

        // A new archive is an upgrade, and must replace the tree wholesale.
        std::thread::sleep(std::time::Duration::from_millis(1100)); // mtime is whole seconds
        make_archive(&res, "version-two-is-longer");
        ensure_unpacked(Some(&res), &tmp).expect("upgrade unpack");
        assert_eq!(
            std::fs::read_to_string(&binary).unwrap(),
            "version-two-is-longer",
            "a changed archive must be unpacked over the old one"
        );

        let _ = std::fs::remove_dir_all(&tmp);
    }

    #[test]
    #[cfg(target_os = "macos")]
    /// A partial unpack must not be trusted. The stamp is written last, so an
    /// archive that expands to the wrong shape leaves no stamp and is retried.
    fn a_bad_archive_leaves_no_stamp() {
        let tmp = std::env::temp_dir().join(format!("wl-embed-bad-{}", std::process::id()));
        let res = tmp.join("resources");
        std::fs::create_dir_all(&res).unwrap();

        // An archive that unpacks fine but contains no WebLinked.app.
        let staged = res.join("stage");
        std::fs::create_dir_all(&staged).unwrap();
        std::fs::write(staged.join("something-else"), "x").unwrap();
        std::process::Command::new("/usr/bin/ditto")
            .args(["-c", "-k", "--sequesterRsrc", "--keepParent"])
            .arg(&staged)
            .arg(res.join(ARCHIVE_NAME))
            .status()
            .unwrap();

        assert!(ensure_unpacked(Some(&res), &tmp).is_err());
        assert!(
            !stamp_path(&tmp).exists(),
            "a failed unpack must not leave a stamp behind to be trusted later"
        );
        let _ = std::fs::remove_dir_all(&tmp);
    }

    #[test]
    /// The stamp must change when the archive does, or an upgrade would keep
    /// running the previous version's unpacked tree forever.
    fn the_stamp_tracks_the_archive() {
        let tmp = std::env::temp_dir().join(format!("wl-embed-stamp-{}", std::process::id()));
        std::fs::create_dir_all(&tmp).unwrap();
        let archive = tmp.join("a.zip");
        std::fs::write(&archive, b"one").unwrap();
        let first = archive_stamp(&archive).unwrap();
        std::fs::write(&archive, b"a longer payload").unwrap();
        let second = archive_stamp(&archive).unwrap();
        assert_ne!(first, second);
        let _ = std::fs::remove_dir_all(&tmp);
    }
}
