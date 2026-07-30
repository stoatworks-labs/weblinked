#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace weblinked::json {

/// A small JSON value type.
///
/// Hand-rolled rather than vendored: the control API needs to emit a state
/// object and read a handful of scalars out of a request body, and that is the
/// whole requirement. Objects keep insertion order so /api/state renders the
/// same way every time, which matters when a human is diffing two responses to
/// work out what changed mid-show.
class Value {
 public:
  enum class Type { kNull, kBool, kNumber, kString, kArray, kObject };

  Value() = default;
  explicit Value(bool value);
  explicit Value(double value);
  explicit Value(int value);
  explicit Value(int64_t value);
  Value(const char* value);
  explicit Value(std::string value);

  static Value array();
  static Value object();

  Type type() const { return type_; }
  bool isNull() const { return type_ == Type::kNull; }
  bool isBool() const { return type_ == Type::kBool; }
  bool isNumber() const { return type_ == Type::kNumber; }
  bool isString() const { return type_ == Type::kString; }
  bool isArray() const { return type_ == Type::kArray; }
  bool isObject() const { return type_ == Type::kObject; }

  bool asBool(bool fallback = false) const;
  double asDouble(double fallback = 0.0) const;
  int asInt(int fallback = 0) const;
  int64_t asInt64(int64_t fallback = 0) const;
  std::string asString(const std::string& fallback = {}) const;

  /// Array access.
  void push(Value value);
  size_t size() const;
  const Value& at(size_t index) const;

  /// Object access. set() replaces an existing key in place, preserving its
  /// position.
  void set(const std::string& key, Value value);
  bool has(const std::string& key) const;
  /// Returns a null value for a missing key, so chained reads never throw.
  const Value& operator[](const std::string& key) const;

  const std::vector<std::pair<std::string, Value>>& members() const { return members_; }
  const std::vector<Value>& elements() const { return elements_; }

  std::string serialize(bool pretty = false) const;

 private:
  void serializeInto(std::string& out, bool pretty, int depth) const;

  Type type_ = Type::kNull;
  bool bool_ = false;
  double number_ = 0.0;
  std::string string_;
  std::vector<Value> elements_;
  std::vector<std::pair<std::string, Value>> members_;
};

/// Parses `text`. On failure returns nullopt and, if `error` is non-null, a
/// message naming the byte offset.
std::optional<Value> parse(const std::string& text, std::string* error = nullptr);

}  // namespace weblinked::json
