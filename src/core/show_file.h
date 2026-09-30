#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/json.h"
#include "core/source_config.h"

namespace weblinked {

/// A show: a file an operator saves to their own machine and loads back later,
/// in two halves that can be recalled together or on their own.
///
///   "tabs"     which pages are open — id, URL, and how each page behaves
///              (whether its preview arms for input, what a new tab does).
///   "outputs"  where each tab goes — its format, colour matrix and pacing,
///              and its output list, keyed by tab id.
///
/// The split follows what changes between two shows at one venue. The rig is
/// usually fixed — the same cards, the same NDI names, the same keyer colour —
/// while the pages change per session; so "tabs only" swaps the running pages
/// under an unchanged rig, and "outputs only" restores the rig without touching
/// which pages are on air. Format sits with the outputs because it is what the
/// destinations have to agree with: an SDI card set up for 1080i25 does not care
/// which page is playing, and cares very much about the raster.
///
/// Separate from settings.json on purpose. That file is the instance's own
/// start-up configuration and lives on the WebLinked machine; a show is a
/// document that belongs to whoever is running it, is downloaded by the control
/// page to the operator's computer (usually not the one rendering), and says
/// nothing about control ports or tokens.
///
/// Pure data, no CEF, so every rule below is unit-tested.
namespace show {

/// Written into every file, and checked on load so a settings.json or a
/// diagnostics bundle picked by mistake is refused rather than half-applied.
inline constexpr int kFormatVersion = 1;

/// Which halves a save writes or a load recalls.
struct Parts {
  bool tabs = true;
  bool outputs = true;
};

/// The chosen halves of `running` as a show document.
json::Value capture(const AppConfig& running, Parts parts);

/// Which halves a document actually carries, so the page can offer only those.
Parts present(const json::Value& document);

/// What recalling `parts` of `document` onto `running` should leave running.
/// The result is handed to SourceManager::applyConfiguration, which reconciles
/// it — so anything that ends up identical to what is running is left alone.
///
/// - **tabs**: the open tabs become the file's tabs, in its order. A tab that
///   is already open keeps its outputs and just changes page; a tab that is not
///   gets the first running tab's format and a preview only, so recalling pages
///   can never put a second copy of somebody's NDI name on the network. Tabs
///   not in the file are closed.
/// - **outputs**: each tab the file names has its format, matrix, pacing and
///   output list replaced. Tabs it does not name are left alone, and entries
///   for tabs that are not open are skipped and reported in `notes`.
/// - **both**: the file's tabs, each with the file's outputs where it has them.
///
/// Returns nothing — with `error` set — for a document that is not a show, asks
/// for a half it does not contain, or would leave an invalid configuration.
std::optional<AppConfig> recall(const AppConfig& running,
                                const json::Value& document, Parts parts,
                                std::string* error = nullptr,
                                std::vector<std::string>* notes = nullptr);

}  // namespace show
}  // namespace weblinked
