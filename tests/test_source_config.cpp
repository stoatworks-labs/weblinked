// SourceConfig is the pure-data description of one pipeline: a page, a format
// and where it goes. It exists separately from the engine so a configuration
// file can be parsed, validated and reported on before anything expensive is
// created — and so all of that is testable in a build with no browser in it.
//
// Groundwork for running several sources in one process. Nothing constructs
// these yet; the tests are here so that when something does, the parsing and
// validation are already known-good rather than debugged live.

#include "core/source_config.h"
#include "test_support.h"

using namespace weblinked;

WEBLINKED_TEST(source_config_defaults_to_something_recognisable) {
  const SourceConfig config;
  CHECK_STR(config.format.toString(), "1920x1080p50");
  CHECK_STR(config.url, "about:blank");
  CHECK(config.audioEnabled);
  CHECK(config.externalPacing);
}

WEBLINKED_TEST(source_config_parses_a_minimal_source) {
  auto parsed = json::parse(R"({"id":"scoreboard","url":"https://example.com"})");
  CHECK(parsed.has_value());
  if (!parsed) return;

  std::string error;
  const auto config = SourceConfig::fromJson(*parsed, &error);
  CHECK(config.has_value());
  if (!config) return;

  CHECK_STR(config->id, "scoreboard");
  CHECK_STR(config->url, "https://example.com");
  // Unstated fields keep their defaults rather than becoming zero.
  CHECK_STR(config->format.toString(), "1920x1080p50");
  CHECK(config->audioEnabled);
}

WEBLINKED_TEST(source_config_parses_outputs_and_defaults_their_names) {
  auto parsed = json::parse(R"({
    "id": "lower-third",
    "format": "720p59.94",
    "matrix": "709",
    "pacing": "internal",
    "audio": false,
    "outputs": [
      {"kind": "ndi", "name": "LowerThird", "options": {"alpha": true}},
      {"kind": "decklink", "device_index": 2},
      {"kind": "omt"}
    ]
  })");
  CHECK(parsed.has_value());
  if (!parsed) return;

  std::string error;
  const auto config = SourceConfig::fromJson(*parsed, &error);
  CHECK(config.has_value());
  if (!config) return;

  CHECK_STR(config->format.toString(), "1280x720p59.94");
  CHECK(config->matrix == ColourMatrix::kBt709);
  CHECK(!config->externalPacing);
  CHECK(!config->audioEnabled);
  CHECK_EQ(config->outputs.size(), static_cast<size_t>(3));

  CHECK_STR(config->outputs[0].name, "LowerThird");
  CHECK(config->outputs[0].options["alpha"].asBool());
  CHECK_EQ(config->outputs[1].deviceIndex, 2);
  // An output written as just {"kind":"omt"} still has to be addressable, so
  // the name falls back to the kind.
  CHECK_STR(config->outputs[2].name, "omt");
}

WEBLINKED_TEST(source_config_rejects_what_cannot_work) {
  const auto check = [](const char* json, const char* what) {
    auto parsed = json::parse(json);
    CHECK(parsed.has_value());
    if (!parsed) return;
    std::string error;
    const auto config = SourceConfig::fromJson(*parsed, &error);
    if (config) {
      // Parsed, so it must fail validation instead.
      CHECK(!config->validate(&error));
      CHECK(!error.empty());
    }
    (void)what;
  };

  check(R"({"url":"https://x"})", "no id");
  check(R"({"id":"has space","url":"https://x"})", "id with a space");
  check(R"({"id":"a","format":"nonsense"})", "unparseable format");
  check(R"({"id":"a","format":"1921x1080p50"})", "odd width");
  check(R"({"id":"a","outputs":[{"kind":"ndi","name":"x"},{"kind":"omt","name":"x"}]})",
        "duplicate output names");

  // An output with no kind cannot be constructed at all.
  auto noKind = json::parse(R"({"id":"a","outputs":[{"name":"x"}]})");
  CHECK(noKind.has_value());
  if (noKind) {
    std::string error;
    CHECK(!SourceConfig::fromJson(*noKind, &error).has_value());
    CHECK(!error.empty());
  }
}

WEBLINKED_TEST(source_config_adds_a_preview_when_asked) {
  SourceConfig config;
  config.id = "a";
  config.ensurePreview();
  CHECK_EQ(config.outputs.size(), static_cast<size_t>(1));
  CHECK_STR(config.outputs[0].kind, "preview");

  // Calling it twice must not stack up previews.
  config.ensurePreview();
  CHECK_EQ(config.outputs.size(), static_cast<size_t>(1));

  SourceConfig without;
  without.id = "b";
  without.wantPreview = false;
  without.ensurePreview();
  CHECK_EQ(without.outputs.size(), static_cast<size_t>(0));
}

WEBLINKED_TEST(app_config_parses_several_sources_and_names_the_unnamed) {
  const auto config = AppConfig::parse(R"({
    "control": { "http_port": 8000, "http_token": "secret", "osc_enabled": false },
    "sources": [
      { "id": "clock", "url": "file:///clock.html", "format": "1080p25" },
      { "url": "https://example.com/two" }
    ]
  })");
  CHECK(config.has_value());
  if (!config) return;

  CHECK_EQ(config->httpPort, 8000);
  CHECK_STR(config->httpToken, "secret");
  CHECK(!config->oscEnabled);
  CHECK_EQ(config->sources.size(), static_cast<size_t>(2));

  CHECK_STR(config->sources[0].id, "clock");
  // A hand-written config should not have to invent ids for every source.
  CHECK_STR(config->sources[1].id, "source2");
  // Every source gets a preview so the control page has something to show.
  CHECK_STR(config->sources[0].outputs[0].kind, "preview");
}

WEBLINKED_TEST(app_config_rejects_duplicate_ids) {
  std::string error;
  const auto config = AppConfig::parse(
      R"({"sources":[{"id":"a","url":"x"},{"id":"a","url":"y"}]})", &error);
  CHECK(!config.has_value());
  // The message has to name the collision, or a twenty-source file is a
  // guessing game.
  CHECK(error.find("'a'") != std::string::npos);
}

WEBLINKED_TEST(app_config_reports_where_the_problem_is) {
  std::string error;
  const auto config = AppConfig::parse(
      R"({"sources":[{"id":"ok","url":"x"},{"id":"bad","format":"nope"}]})", &error);
  CHECK(!config.has_value());
  // Which source, not just "invalid config".
  CHECK(error.find("sources[1]") != std::string::npos);
}

WEBLINKED_TEST(app_config_round_trips) {
  const auto original = AppConfig::parse(R"({
    "sources": [
      { "id": "one", "url": "https://example.com", "format": "1080i25",
        "outputs": [ {"kind":"ndi","name":"One"} ] }
    ]
  })");
  CHECK(original.has_value());
  if (!original) return;

  const auto again = AppConfig::parse(original->toJson().serialize());
  CHECK(again.has_value());
  if (!again) return;

  CHECK_EQ(again->sources.size(), original->sources.size());
  CHECK_STR(again->sources[0].id, "one");
  CHECK_STR(again->sources[0].format.toString(), "1920x1080i25");
  CHECK(again->sources[0].format == original->sources[0].format);
}

WEBLINKED_TEST(app_config_rejects_malformed_files_clearly) {
  std::string error;
  CHECK(!AppConfig::parse("not json at all", &error).has_value());
  CHECK(error.find("not valid JSON") != std::string::npos);

  CHECK(!AppConfig::parse(R"({"control":{}})", &error).has_value());
  CHECK(error.find("sources") != std::string::npos);

  CHECK(!AppConfig::parse(R"({"sources":{}})", &error).has_value());
}
