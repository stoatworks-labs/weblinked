// Show files: the two halves (tabs, outputs) and what recalling either one onto
// a running set of tabs leaves running. Tested on the merge rather than through
// the API, because the merge is where "load the pages, keep my rig" is decided.

#include "core/show_file.h"
#include "test_support.h"

using namespace weblinked;

namespace {

OutputConfig output(const char* kind, const char* name) {
  OutputConfig out;
  out.kind = kind;
  out.name = name;
  return out;
}

SourceConfig tab(const char* id, const char* url, const char* format) {
  SourceConfig source;
  source.id = id;
  source.url = url;
  source.format = *VideoFormat::parse(format);
  source.wantPreview = false;
  source.outputs.push_back(output("preview", "preview"));
  return source;
}

/// Two tabs on air: "main" with an NDI feed, "lower" with its own.
AppConfig running() {
  AppConfig config;
  SourceConfig main = tab("main", "https://example.com/a", "1080p50");
  main.outputs.push_back(output("ndi", "Programme"));
  SourceConfig lower = tab("lower", "https://example.com/l3", "1080p50");
  lower.outputs.push_back(output("ndi", "Lower third"));
  config.sources = {main, lower};
  return config;
}

json::Value parsed(const char* text) { return *json::parse(text); }

}  // namespace

WEBLINKED_TEST(show_capture_writes_only_the_chosen_halves) {
  const AppConfig config = running();

  const json::Value both = show::capture(config, {true, true});
  CHECK_EQ(both["weblinked_show"].asInt(), show::kFormatVersion);
  CHECK_EQ(both["tabs"].size(), size_t{2});
  CHECK_STR(both["tabs"].at(1)["url"].asString(), "https://example.com/l3");
  CHECK_STR(both["outputs"].at(0)["tab"].asString(), "main");
  CHECK_STR(both["outputs"].at(0)["format"].asString(), config.sources[0].format.toString());
  CHECK_STR(both["outputs"].at(0)["outputs"].at(1)["name"].asString(), "Programme");
  // A tab entry says nothing about where it goes.
  CHECK(!both["tabs"].at(0).has("outputs"));
  CHECK(!both["tabs"].at(0).has("format"));

  const json::Value tabsOnly = show::capture(config, {true, false});
  CHECK(tabsOnly.has("tabs"));
  CHECK(!tabsOnly.has("outputs"));

  const json::Value outputsOnly = show::capture(config, {false, true});
  CHECK(!outputsOnly.has("tabs"));
  CHECK(outputsOnly.has("outputs"));
}

WEBLINKED_TEST(show_round_trip_recalls_what_was_saved) {
  const AppConfig saved = running();
  const json::Value document =
      *json::parse(show::capture(saved, {true, true}).serialize(true));

  // Recalled onto a single blank tab: both saved tabs come back with their rigs.
  AppConfig blank;
  blank.sources = {tab("main", "about:blank", "720p25")};
  std::string error;
  const auto result = show::recall(blank, document, {true, true}, &error);
  CHECK(result.has_value());
  if (!result) return;
  CHECK_EQ(result->sources.size(), size_t{2});
  CHECK_STR(result->sources[0].url, "https://example.com/a");
  CHECK_STR(result->sources[0].format.toString(), saved.sources[0].format.toString());
  CHECK_EQ(result->sources[1].outputs.size(), size_t{2});
  CHECK_STR(result->sources[1].outputs[1].name, "Lower third");
}

WEBLINKED_TEST(show_tabs_only_keeps_the_rig) {
  const json::Value document = parsed(R"({
    "weblinked_show": 1,
    "tabs": [ { "id": "main", "url": "https://example.com/tonight" },
              { "id": "clock", "url": "https://example.com/clock" } ],
    "outputs": [ { "tab": "main", "format": "720p25",
                   "outputs": [ { "kind": "ndi", "name": "Other rig" } ] } ]
  })");
  std::vector<std::string> notes;
  std::string error;
  const auto result = show::recall(running(), document, {true, false}, &error, &notes);
  CHECK(result.has_value());
  if (!result) return;

  // "lower" is not in the file, so it closes; "clock" is new.
  CHECK_EQ(result->sources.size(), size_t{2});
  CHECK_STR(result->sources[0].id, "main");
  CHECK_STR(result->sources[0].url, "https://example.com/tonight");
  // The file's outputs half was not asked for, so main keeps its own rig.
  CHECK_STR(result->sources[0].format.toString(), running().sources[0].format.toString());
  CHECK_EQ(result->sources[0].outputs.size(), size_t{2});
  CHECK_STR(result->sources[0].outputs[1].name, "Programme");

  // A new tab gets the running raster and a preview, and nothing that could
  // claim a card or an NDI name.
  CHECK_STR(result->sources[1].id, "clock");
  CHECK_STR(result->sources[1].format.toString(), running().sources[0].format.toString());
  CHECK_EQ(result->sources[1].outputs.size(), size_t{1});
  CHECK_STR(result->sources[1].outputs[0].kind, "preview");
}

WEBLINKED_TEST(show_outputs_only_keeps_the_pages) {
  const json::Value document = parsed(R"({
    "weblinked_show": 1,
    "tabs": [ { "id": "main", "url": "https://example.com/other" } ],
    "outputs": [
      { "tab": "main", "format": "720p59.94", "matrix": "709",
        "outputs": [ { "kind": "ndi", "name": "Restored", "background": "#ff0000" } ] },
      { "tab": "gone", "outputs": [] }
    ]
  })");
  std::vector<std::string> notes;
  std::string error;
  const auto result = show::recall(running(), document, {false, true}, &error, &notes);
  CHECK(result.has_value());
  if (!result) return;

  // The tab set and the pages are exactly what was running.
  CHECK_EQ(result->sources.size(), size_t{2});
  CHECK_STR(result->sources[0].url, "https://example.com/a");
  CHECK_STR(result->sources[1].url, "https://example.com/l3");

  // main's rig is replaced whole — Programme is gone, not merged — and the
  // preview comes back even though the file omitted it.
  const SourceConfig& main = result->sources[0];
  CHECK_STR(main.format.toString(), VideoFormat::parse("720p59.94")->toString());
  CHECK(main.matrix == ColourMatrix::kBt709);
  CHECK_EQ(main.outputs.size(), size_t{2});
  CHECK_STR(main.outputs[0].kind, "preview");
  CHECK_STR(main.outputs[1].name, "Restored");
  CHECK(main.outputs[1].background.opaque);

  // A tab the file does not mention is left alone.
  CHECK_STR(result->sources[1].outputs[1].name, "Lower third");

  // A rig for a tab that is not open is reported, not silently dropped.
  CHECK_EQ(notes.size(), size_t{1});
  CHECK(notes[0].find("'gone'") != std::string::npos);
}

WEBLINKED_TEST(show_both_gives_new_tabs_their_saved_rig) {
  const json::Value document = parsed(R"({
    "weblinked_show": 1,
    "tabs": [ { "id": "clock", "url": "https://example.com/clock", "popups": "block" } ],
    "outputs": [ { "tab": "clock", "format": "720p50",
                   "outputs": [ { "kind": "ndi", "name": "Clock" } ] } ]
  })");
  std::string error;
  const auto result = show::recall(running(), document, {true, true}, &error);
  CHECK(result.has_value());
  if (!result) return;
  CHECK_EQ(result->sources.size(), size_t{1});
  CHECK_STR(result->sources[0].popupPolicy, "block");
  CHECK_STR(result->sources[0].format.toString(), VideoFormat::parse("720p50")->toString());
  CHECK_EQ(result->sources[0].outputs.size(), size_t{2});
  CHECK_STR(result->sources[0].outputs[1].name, "Clock");
}

WEBLINKED_TEST(show_recall_refuses_what_it_cannot_apply_whole) {
  std::string error;

  // Not a show at all — a settings.json picked by mistake.
  CHECK(!show::recall(running(), parsed(R"({"sources": []})"), {true, true}, &error));
  CHECK(error.find("not a WebLinked show") != std::string::npos);

  // Asking for a half the file does not have.
  const json::Value tabsOnly = parsed(R"({"weblinked_show": 1, "tabs": [{"id": "a"}]})");
  CHECK(!show::recall(running(), tabsOnly, {false, true}, &error));
  CHECK(error.find("no outputs") != std::string::npos);
  CHECK(show::present(tabsOnly).tabs);
  CHECK(!show::present(tabsOnly).outputs);

  // Nothing to render.
  CHECK(!show::recall(running(), parsed(R"({"weblinked_show": 1, "tabs": []})"),
                      {true, false}, &error));

  // Duplicate and unusable ids.
  CHECK(!show::recall(running(),
                      parsed(R"({"weblinked_show": 1, "tabs": [{"id": "a"}, {"id": "a"}]})"),
                      {true, false}, &error));
  CHECK(!show::recall(running(),
                      parsed(R"({"weblinked_show": 1, "tabs": [{"id": "a b"}]})"),
                      {true, false}, &error));

  // A bad format names the tab rather than half-applying.
  CHECK(!show::recall(running(),
                      parsed(R"({"weblinked_show": 1, "outputs": [{"tab": "main", "format": "banana"}]})"),
                      {false, true}, &error));
  CHECK(error.find("'main'") != std::string::npos);

  // From the future.
  CHECK(!show::recall(running(), parsed(R"({"weblinked_show": 99, "tabs": [{"id": "a"}]})"),
                      {true, false}, &error));
}
