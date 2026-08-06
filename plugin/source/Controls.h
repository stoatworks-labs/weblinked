#pragma once

namespace weblinked {

/// Parameter indices, in the order Resolume shows them.
///
/// The URL comes first because it is the whole point of the plugin: everything
/// else is about *how* that page gets here.
enum ParamId : unsigned int {
  /// The page to render. FF_TYPE_TEXT.
  ///
  /// **Empty is load-bearing, not merely a default.** Resolume instantiates
  /// every plugin it finds when it scans, and a fresh instance has this empty.
  /// Nothing may spawn a process, open a port or touch the network until an
  /// operator has actually typed something here — otherwise a plugin scan
  /// starts a browser per installed plugin.
  PT_URL = 0,

  /// Where the picture comes from. Option parameter, two elements.
  ///
  /// The same shape as `cartridge`'s In Process / Helper choice, and for the
  /// same reason: an operator should be able to see which of two quite
  /// different arrangements they are in.
  ///
  ///   Run WebLinked — this plugin owns a WebLinked process, renders PT_URL
  ///                   in it, and shows the result. The reason the plugin
  ///                   exists: the URL is saved with the composition.
  ///   Attach        — show an existing Syphon source named PT_SOURCE_NAME,
  ///                   started by somebody else. What Resolume's own Syphon
  ///                   input already does, kept because it is the honest
  ///                   fallback when a WebLinked is already running.
  PT_MODE,

  /// The Syphon/Spout source name. FF_TYPE_TEXT.
  ///
  /// In Attach mode this is what to look for. In Run mode it is only a stem —
  /// see WebLinkedPlugin::instanceSourceName, which makes it unique per
  /// instance so two layers do not publish under one name and fight.
  PT_SOURCE_NAME,

  /// Whether the page should be running. FF_TYPE_BOOLEAN.
  ///
  /// Defaults to on, which is safe only because PT_URL defaults to empty —
  /// together they mean a scanned instance does nothing at all.
  PT_RUN,

  PT_COUNT
};

/// PT_MODE values.
enum class SourceMode { kRun = 0, kAttach = 1 };

inline SourceMode modeFromParam(float value) {
  return value >= 0.5f ? SourceMode::kAttach : SourceMode::kRun;
}

}  // namespace weblinked
