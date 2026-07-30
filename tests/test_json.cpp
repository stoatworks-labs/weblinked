#include "core/json.h"
#include "test_support.h"

using namespace weblinked;

WEBLINKED_TEST(json_parses_the_shapes_the_control_api_uses) {
  auto parsed = json::parse(R"({"url":"https://example.com","fps":50,"live":true,
                                "outputs":["ndi","omt"],"gain":-3.5,"nothing":null})");
  CHECK(parsed.has_value());
  if (!parsed) {
    return;
  }

  CHECK(parsed->isObject());
  CHECK_STR((*parsed)["url"].asString(), "https://example.com");
  CHECK_EQ((*parsed)["fps"].asInt(), 50);
  CHECK((*parsed)["live"].asBool());
  CHECK_NEAR((*parsed)["gain"].asDouble(), -3.5, 1e-9);
  CHECK((*parsed)["nothing"].isNull());
  CHECK_EQ((*parsed)["outputs"].size(), static_cast<size_t>(2));
  CHECK_STR((*parsed)["outputs"].at(1).asString(), "omt");
}

WEBLINKED_TEST(json_missing_keys_read_as_null_rather_than_throwing) {
  auto parsed = json::parse(R"({"a":1})");
  CHECK(parsed.has_value());
  if (!parsed) {
    return;
  }
  CHECK((*parsed)["absent"].isNull());
  CHECK_EQ((*parsed)["absent"].asInt(42), 42);
  CHECK_STR((*parsed)["absent"].asString("fallback"), "fallback");
  // Chained reads through a missing key must not explode either.
  CHECK((*parsed)["absent"]["deeper"].isNull());
  CHECK((*parsed)["absent"].at(3).isNull());
}

WEBLINKED_TEST(json_round_trips_escapes) {
  const std::string awkward = "quote\" backslash\\ newline\n tab\t control\x01";
  json::Value object = json::Value::object();
  object.set("text", json::Value(awkward));

  auto reparsed = json::parse(object.serialize());
  CHECK(reparsed.has_value());
  if (reparsed) {
    CHECK_STR((*reparsed)["text"].asString(), awkward);
  }
}

WEBLINKED_TEST(json_decodes_utf8_and_surrogate_pairs) {
  // A URL with an emoji in a query string is not hypothetical.
  auto parsed = json::parse(R"({"s":"café 🎥"})");
  CHECK(parsed.has_value());
  if (parsed) {
    CHECK_STR((*parsed)["s"].asString(), "café 🎥");
  }

  // Raw UTF-8 passes through unchanged in both directions.
  json::Value object = json::Value::object();
  object.set("s", json::Value(std::string("café 🎥")));
  auto again = json::parse(object.serialize());
  CHECK(again.has_value());
  if (again) {
    CHECK_STR((*again)["s"].asString(), "café 🎥");
  }
}

WEBLINKED_TEST(json_object_order_is_insertion_order) {
  // /api/state should render identically every time, so two responses can be
  // diffed by eye during a show.
  json::Value object = json::Value::object();
  object.set("zebra", json::Value(1));
  object.set("alpha", json::Value(2));
  object.set("middle", json::Value(3));
  CHECK_STR(object.serialize(), R"({"zebra":1,"alpha":2,"middle":3})");

  // Replacing a key keeps its position rather than moving it to the end.
  object.set("zebra", json::Value(9));
  CHECK_STR(object.serialize(), R"({"zebra":9,"alpha":2,"middle":3})");
}

WEBLINKED_TEST(json_serialises_numbers_without_spurious_decimals) {
  json::Value object = json::Value::object();
  object.set("int", json::Value(50));
  object.set("negative", json::Value(-1));
  object.set("real", json::Value(1.5));
  object.set("big", json::Value(static_cast<int64_t>(1'000'000'000)));
  CHECK_STR(object.serialize(),
            R"({"int":50,"negative":-1,"real":1.5,"big":1000000000})");
}

WEBLINKED_TEST(json_rejects_malformed_input) {
  std::string error;
  CHECK(!json::parse("{", &error).has_value());
  CHECK(!error.empty());
  CHECK(!json::parse(R"({"a":})").has_value());
  CHECK(!json::parse(R"({"a" 1})").has_value());
  CHECK(!json::parse(R"(["unterminated)").has_value());
  CHECK(!json::parse(R"({"a":1} trailing)").has_value());
  CHECK(!json::parse("tru").has_value());
  CHECK(!json::parse("").has_value());
  // Deep nesting must fail cleanly rather than blowing the stack: this is a
  // network-facing parser.
  std::string deep(2000, '[');
  CHECK(!json::parse(deep).has_value());
}

WEBLINKED_TEST(json_pretty_printing_is_still_valid_json) {
  json::Value inner = json::Value::object();
  inner.set("name", json::Value("ndi"));
  inner.set("enabled", json::Value(true));

  json::Value outputs = json::Value::array();
  outputs.push(inner);

  json::Value root = json::Value::object();
  root.set("outputs", outputs);
  root.set("empty_object", json::Value::object());
  root.set("empty_array", json::Value::array());

  const std::string pretty = root.serialize(true);
  CHECK(pretty.find('\n') != std::string::npos);

  auto reparsed = json::parse(pretty);
  CHECK(reparsed.has_value());
  if (reparsed) {
    CHECK_STR((*reparsed)["outputs"].at(0)["name"].asString(), "ndi");
    CHECK((*reparsed)["empty_array"].isArray());
    CHECK((*reparsed)["empty_object"].isObject());
  }
}
