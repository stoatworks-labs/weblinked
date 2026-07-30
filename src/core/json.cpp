#include "core/json.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace weblinked::json {
namespace {

const Value& nullValue() {
  static const Value kNull;
  return kNull;
}

void appendEscaped(std::string& out, const std::string& text) {
  out += '"';
  for (const char raw : text) {
    const auto c = static_cast<unsigned char>(raw);
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      case '\b': out += "\\b";  break;
      case '\f': out += "\\f";  break;
      default:
        if (c < 0x20) {
          char buffer[7];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
          out += buffer;
        } else {
          // UTF-8 bytes pass through unchanged; JSON is defined over Unicode
          // text and re-encoding them as \u escapes would only make the output
          // harder to read.
          out += raw;
        }
        break;
    }
  }
  out += '"';
}

void appendNumber(std::string& out, double value) {
  if (!std::isfinite(value)) {
    out += "null";  // JSON has no NaN or Infinity
    return;
  }
  if (value == std::floor(value) && std::fabs(value) < 1e15) {
    out += std::to_string(static_cast<int64_t>(value));
    return;
  }
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%.10g", value);
  out += buffer;
}

class Parser {
 public:
  explicit Parser(const std::string& text) : text_(text) {}

  std::optional<Value> run(std::string* error) {
    skipWhitespace();
    auto value = parseValue();
    if (!value) {
      if (error != nullptr) {
        *error = error_;
      }
      return std::nullopt;
    }
    skipWhitespace();
    if (pos_ != text_.size()) {
      fail("trailing characters after value");
      if (error != nullptr) {
        *error = error_;
      }
      return std::nullopt;
    }
    return value;
  }

 private:
  bool fail(const std::string& message) {
    if (error_.empty()) {
      error_ = message + " at offset " + std::to_string(pos_);
    }
    return false;
  }

  void skipWhitespace() {
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos_;
      } else {
        break;
      }
    }
  }

  bool consume(char expected) {
    if (pos_ < text_.size() && text_[pos_] == expected) {
      ++pos_;
      return true;
    }
    return false;
  }

  bool literal(const char* word) {
    const size_t length = std::strlen(word);
    if (text_.compare(pos_, length, word) == 0) {
      pos_ += length;
      return true;
    }
    return false;
  }

  std::optional<Value> parseValue() {
    if (++depth_ > kMaxDepth) {
      fail("nesting too deep");
      return std::nullopt;
    }
    auto result = parseValueInner();
    --depth_;
    return result;
  }

  std::optional<Value> parseValueInner() {
    skipWhitespace();
    if (pos_ >= text_.size()) {
      fail("unexpected end of input");
      return std::nullopt;
    }

    switch (text_[pos_]) {
      case '{': return parseObject();
      case '[': return parseArray();
      case '"': {
        std::string out;
        if (!parseString(out)) {
          return std::nullopt;
        }
        return Value(std::move(out));
      }
      case 't':
        if (literal("true")) return Value(true);
        fail("invalid literal");
        return std::nullopt;
      case 'f':
        if (literal("false")) return Value(false);
        fail("invalid literal");
        return std::nullopt;
      case 'n':
        if (literal("null")) return Value();
        fail("invalid literal");
        return std::nullopt;
      default:
        return parseNumber();
    }
  }

  std::optional<Value> parseObject() {
    if (!consume('{')) {
      fail("expected '{'");
      return std::nullopt;
    }
    Value object = Value::object();
    skipWhitespace();
    if (consume('}')) {
      return object;
    }
    while (true) {
      skipWhitespace();
      std::string key;
      if (!parseString(key)) {
        return std::nullopt;
      }
      skipWhitespace();
      if (!consume(':')) {
        fail("expected ':'");
        return std::nullopt;
      }
      auto value = parseValue();
      if (!value) {
        return std::nullopt;
      }
      object.set(key, std::move(*value));
      skipWhitespace();
      if (consume(',')) {
        continue;
      }
      if (consume('}')) {
        return object;
      }
      fail("expected ',' or '}'");
      return std::nullopt;
    }
  }

  std::optional<Value> parseArray() {
    if (!consume('[')) {
      fail("expected '['");
      return std::nullopt;
    }
    Value array = Value::array();
    skipWhitespace();
    if (consume(']')) {
      return array;
    }
    while (true) {
      auto value = parseValue();
      if (!value) {
        return std::nullopt;
      }
      array.push(std::move(*value));
      skipWhitespace();
      if (consume(',')) {
        continue;
      }
      if (consume(']')) {
        return array;
      }
      fail("expected ',' or ']'");
      return std::nullopt;
    }
  }

  bool parseString(std::string& out) {
    if (!consume('"')) {
      return fail("expected string");
    }
    out.clear();
    while (pos_ < text_.size()) {
      const char c = text_[pos_++];
      if (c == '"') {
        return true;
      }
      if (c != '\\') {
        out += c;
        continue;
      }
      if (pos_ >= text_.size()) {
        return fail("unterminated escape");
      }
      const char escape = text_[pos_++];
      switch (escape) {
        case '"':  out += '"';  break;
        case '\\': out += '\\'; break;
        case '/':  out += '/';  break;
        case 'b':  out += '\b'; break;
        case 'f':  out += '\f'; break;
        case 'n':  out += '\n'; break;
        case 'r':  out += '\r'; break;
        case 't':  out += '\t'; break;
        case 'u': {
          uint32_t code = 0;
          if (!parseHex4(code)) {
            return false;
          }
          // Surrogate pair, as a URL with an emoji in a query string will
          // produce the moment anyone tests with one.
          if (code >= 0xD800 && code <= 0xDBFF && pos_ + 1 < text_.size() &&
              text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
            const size_t save = pos_;
            pos_ += 2;
            uint32_t low = 0;
            if (parseHex4(low) && low >= 0xDC00 && low <= 0xDFFF) {
              code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
            } else {
              pos_ = save;
            }
          }
          appendUtf8(out, code);
          break;
        }
        default:
          return fail("invalid escape");
      }
    }
    return fail("unterminated string");
  }

  bool parseHex4(uint32_t& out) {
    if (pos_ + 4 > text_.size()) {
      return fail("truncated \\u escape");
    }
    out = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = text_[pos_++];
      out <<= 4;
      if (c >= '0' && c <= '9') {
        out |= static_cast<uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        out |= static_cast<uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        out |= static_cast<uint32_t>(c - 'A' + 10);
      } else {
        return fail("invalid hex digit");
      }
    }
    return true;
  }

  static void appendUtf8(std::string& out, uint32_t code) {
    if (code < 0x80) {
      out += static_cast<char>(code);
    } else if (code < 0x800) {
      out += static_cast<char>(0xC0 | (code >> 6));
      out += static_cast<char>(0x80 | (code & 0x3F));
    } else if (code < 0x10000) {
      out += static_cast<char>(0xE0 | (code >> 12));
      out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (code & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (code >> 18));
      out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (code & 0x3F));
    }
  }

  std::optional<Value> parseNumber() {
    const size_t start = pos_;
    if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) {
      ++pos_;
    }
    bool anyDigits = false;
    while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
      ++pos_;
      anyDigits = true;
    }
    if (pos_ < text_.size() && text_[pos_] == '.') {
      ++pos_;
      while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
        ++pos_;
        anyDigits = true;
      }
    }
    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      ++pos_;
      if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) {
        ++pos_;
      }
      while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
        ++pos_;
      }
    }
    if (!anyDigits) {
      fail("invalid number");
      return std::nullopt;
    }
    return Value(std::strtod(text_.substr(start, pos_ - start).c_str(), nullptr));
  }

  static constexpr int kMaxDepth = 64;

  const std::string& text_;
  size_t pos_ = 0;
  int depth_ = 0;
  std::string error_;
};

}  // namespace

Value::Value(bool value) : type_(Type::kBool), bool_(value) {}
Value::Value(double value) : type_(Type::kNumber), number_(value) {}
Value::Value(int value) : type_(Type::kNumber), number_(static_cast<double>(value)) {}
Value::Value(int64_t value) : type_(Type::kNumber), number_(static_cast<double>(value)) {}
Value::Value(const char* value)
    : type_(Type::kString), string_(value != nullptr ? value : "") {}
Value::Value(std::string value) : type_(Type::kString), string_(std::move(value)) {}

Value Value::array() {
  Value value;
  value.type_ = Type::kArray;
  return value;
}

Value Value::object() {
  Value value;
  value.type_ = Type::kObject;
  return value;
}

bool Value::asBool(bool fallback) const {
  if (type_ == Type::kBool) return bool_;
  if (type_ == Type::kNumber) return number_ != 0.0;
  return fallback;
}

double Value::asDouble(double fallback) const {
  if (type_ == Type::kNumber) return number_;
  if (type_ == Type::kBool) return bool_ ? 1.0 : 0.0;
  return fallback;
}

int Value::asInt(int fallback) const {
  return static_cast<int>(asInt64(fallback));
}

int64_t Value::asInt64(int64_t fallback) const {
  if (type_ == Type::kNumber) return static_cast<int64_t>(std::llround(number_));
  if (type_ == Type::kBool) return bool_ ? 1 : 0;
  return fallback;
}

std::string Value::asString(const std::string& fallback) const {
  if (type_ == Type::kString) return string_;
  return fallback;
}

void Value::push(Value value) {
  if (type_ != Type::kArray) {
    type_ = Type::kArray;
    elements_.clear();
  }
  elements_.push_back(std::move(value));
}

size_t Value::size() const {
  if (type_ == Type::kArray) return elements_.size();
  if (type_ == Type::kObject) return members_.size();
  return 0;
}

const Value& Value::at(size_t index) const {
  if (type_ != Type::kArray || index >= elements_.size()) {
    return nullValue();
  }
  return elements_[index];
}

void Value::set(const std::string& key, Value value) {
  if (type_ != Type::kObject) {
    type_ = Type::kObject;
    members_.clear();
  }
  for (auto& member : members_) {
    if (member.first == key) {
      member.second = std::move(value);
      return;
    }
  }
  members_.emplace_back(key, std::move(value));
}

bool Value::has(const std::string& key) const {
  if (type_ != Type::kObject) return false;
  for (const auto& member : members_) {
    if (member.first == key) return true;
  }
  return false;
}

const Value& Value::operator[](const std::string& key) const {
  if (type_ == Type::kObject) {
    for (const auto& member : members_) {
      if (member.first == key) return member.second;
    }
  }
  return nullValue();
}

std::string Value::serialize(bool pretty) const {
  std::string out;
  serializeInto(out, pretty, 0);
  return out;
}

void Value::serializeInto(std::string& out, bool pretty, int depth) const {
  const std::string indent = pretty ? std::string(static_cast<size_t>(depth + 1) * 2, ' ') : "";
  const std::string closeIndent = pretty ? std::string(static_cast<size_t>(depth) * 2, ' ') : "";
  const char* separator = pretty ? ",\n" : ",";

  switch (type_) {
    case Type::kNull:
      out += "null";
      break;
    case Type::kBool:
      out += bool_ ? "true" : "false";
      break;
    case Type::kNumber:
      appendNumber(out, number_);
      break;
    case Type::kString:
      appendEscaped(out, string_);
      break;
    case Type::kArray: {
      if (elements_.empty()) {
        out += "[]";
        break;
      }
      out += pretty ? "[\n" : "[";
      bool first = true;
      for (const auto& element : elements_) {
        if (!first) out += separator;
        first = false;
        out += indent;
        element.serializeInto(out, pretty, depth + 1);
      }
      out += pretty ? "\n" + closeIndent + "]" : "]";
      break;
    }
    case Type::kObject: {
      if (members_.empty()) {
        out += "{}";
        break;
      }
      out += pretty ? "{\n" : "{";
      bool first = true;
      for (const auto& member : members_) {
        if (!first) out += separator;
        first = false;
        out += indent;
        appendEscaped(out, member.first);
        out += pretty ? ": " : ":";
        member.second.serializeInto(out, pretty, depth + 1);
      }
      out += pretty ? "\n" + closeIndent + "}" : "}";
      break;
    }
  }
}

std::optional<Value> parse(const std::string& text, std::string* error) {
  Parser parser(text);
  return parser.run(error);
}

}  // namespace weblinked::json
