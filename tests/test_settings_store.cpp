// The settings store is what the control page's Save button writes and what the
// next launch reads back. Tested here rather than through the API because the
// failure that matters — a half-written file leaving an instance with no
// outputs — is a filesystem problem, not an HTTP one.

#include <cstdio>
#include <filesystem>
#include <fstream>

#include "core/settings_store.h"
#include "test_support.h"

using namespace weblinked;

namespace {

/// A scratch path that cleans up after itself, so a failing assertion cannot
/// leave a stale settings file for the next run to read.
struct TemporaryPath {
  std::filesystem::path path;

  explicit TemporaryPath(const char* name)
      : path(std::filesystem::temp_directory_path() / name) {
    std::filesystem::remove(path);
  }
  ~TemporaryPath() {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path.string() + ".tmp", ec);
  }
  std::string string() const { return path.string(); }
};

AppConfig sampleConfig() {
  AppConfig config;
  SourceConfig source;
  source.id = "main";
  source.url = "https://example.com/graphic.html";
  source.format = *VideoFormat::parse("720p59.94");
  source.matrix = ColourMatrix::kBt709;
  source.externalPacing = false;
  source.interactiveByDefault = false;
  source.popupPolicy = "block";

  OutputConfig preview;
  preview.kind = "preview";
  preview.name = "preview";
  preview.options.set("factor", json::Value(4));
  source.outputs.push_back(preview);

  OutputConfig ndi;
  ndi.kind = "ndi";
  ndi.name = "Programme";
  ndi.options.set("alpha", json::Value(true));
  source.outputs.push_back(ndi);

  config.sources.push_back(source);
  config.httpPort = 7999;
  return config;
}

}  // namespace

WEBLINKED_TEST(settings_default_path_is_under_a_config_directory) {
  const std::string directory = settings::directory();
  CHECK(!directory.empty());
  CHECK(directory.find("WebLinked") != std::string::npos);
  // The log directory is a different place on purpose: logs are disposable,
  // settings are the show.
  CHECK(settings::defaultPath().find("settings.json") != std::string::npos);
}

// The regression this guards is not a parse failure but a launch failure: two
// instances sharing one Chromium profile directory means the second gets a
// "profile could not be loaded correctly" dialog and renders nothing. The
// control port is what keeps them apart, so it has to reach the path.
WEBLINKED_TEST(settings_profile_directory_differs_per_control_port) {
  const std::string first = settings::profileDirectory(7654);
  const std::string second = settings::profileDirectory(7664);

  CHECK(first != second);
  CHECK(first.find("7654") != std::string::npos);
  CHECK(second.find("7664") != std::string::npos);

  // Under the app's own directory, not CEF's machine-wide default, which is
  // shared with every other CEF application installed.
  CHECK(first.find(settings::directory()) == 0);

  // Deterministic: the same port on the next launch reuses the same profile
  // rather than leaving a directory behind every time.
  CHECK(settings::profileDirectory(7654) == first);
}

WEBLINKED_TEST(settings_round_trip_through_a_file) {
  TemporaryPath file("weblinked-settings-round-trip.json");
  std::string error;
  CHECK(settings::save(sampleConfig(), file.string(), &error));
  CHECK_STR(error, "");

  const auto loaded = settings::load(file.string(), &error);
  CHECK(loaded.has_value());
  if (!loaded) return;

  CHECK_EQ(loaded->sources.size(), static_cast<size_t>(1));
  const SourceConfig& source = loaded->sources.front();
  CHECK_STR(source.url, "https://example.com/graphic.html");
  CHECK_STR(source.format.toString(), "1280x720p59.94");
  CHECK(source.matrix == ColourMatrix::kBt709);
  CHECK(!source.externalPacing);
  // The two fields the settings page added. A silent default here would mean an
  // operator's choice quietly reverting on the next launch.
  CHECK(!source.interactiveByDefault);
  CHECK_STR(source.popupPolicy, "block");

  CHECK_EQ(source.outputs.size(), static_cast<size_t>(2));
  CHECK_STR(source.outputs[1].name, "Programme");
  CHECK(source.outputs[1].options["alpha"].asBool(false));
  CHECK_EQ(loaded->httpPort, 7999);
}

WEBLINKED_TEST(settings_save_creates_missing_directories) {
  const auto root = std::filesystem::temp_directory_path() /
                    "weblinked-settings-nested";
  std::error_code ec;
  std::filesystem::remove_all(root, ec);

  const std::string path = (root / "deeper" / "settings.json").string();
  std::string error;
  CHECK(settings::save(sampleConfig(), path, &error));
  CHECK(std::filesystem::exists(path));

  std::filesystem::remove_all(root, ec);
}

WEBLINKED_TEST(settings_save_leaves_no_temporary_behind) {
  TemporaryPath file("weblinked-settings-atomic.json");
  CHECK(settings::save(sampleConfig(), file.string()));
  // The write goes to a sibling and is renamed over the target; a leftover .tmp
  // would mean the rename never happened and the next save would be writing
  // over a file somebody might already be reading.
  CHECK(!std::filesystem::exists(file.string() + ".tmp"));
  CHECK(std::filesystem::exists(file.string()));
}

WEBLINKED_TEST(settings_load_reports_why_it_could_not) {
  std::string error;
  CHECK(!settings::load("/definitely/not/here/settings.json", &error).has_value());
  CHECK(error.find("cannot open") != std::string::npos);

  TemporaryPath file("weblinked-settings-broken.json");
  {
    std::ofstream out(file.path);
    out << "{ this is not json";
  }
  CHECK(!settings::load(file.string(), &error).has_value());
  CHECK(error.find("not valid JSON") != std::string::npos);
}

WEBLINKED_TEST(settings_overwrite_replaces_rather_than_appends) {
  TemporaryPath file("weblinked-settings-overwrite.json");
  AppConfig first = sampleConfig();
  CHECK(settings::save(first, file.string()));

  AppConfig second = sampleConfig();
  second.sources.front().url = "https://example.org/";
  second.sources.front().outputs.resize(1);
  CHECK(settings::save(second, file.string()));

  const auto loaded = settings::load(file.string());
  CHECK(loaded.has_value());
  if (!loaded) return;
  CHECK_STR(loaded->sources.front().url, "https://example.org/");
  CHECK_EQ(loaded->sources.front().outputs.size(), static_cast<size_t>(1));
}

WEBLINKED_TEST(source_config_rejects_an_unknown_popup_policy) {
  SourceConfig config;
  config.id = "main";
  config.popupPolicy = "open-a-window";
  std::string error;
  CHECK(!config.validate(&error));
  // There is deliberately no third option: a windowless browser cannot own a
  // popup window, which is what used to take the process down.
  CHECK(error.find("popup policy") != std::string::npos);

  config.popupPolicy = "block";
  CHECK(config.validate(&error));
  config.popupPolicy = "navigate";
  CHECK(config.validate(&error));
}
