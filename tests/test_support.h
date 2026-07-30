#pragma once

// A test harness small enough to read in one sitting.
//
// No framework, because pulling one in would double the build time of a target
// whose entire job is to run in two seconds and tell you whether the pixel
// maths is still right.

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace weblinked::test {

struct Registry {
  struct Entry {
    std::string name;
    std::function<void()> run;
  };

  static Registry& instance() {
    static Registry registry;
    return registry;
  }

  std::vector<Entry> entries;
  int checks = 0;
  int failures = 0;
  std::string currentTest;
};

struct Registrar {
  Registrar(const char* name, std::function<void()> run) {
    Registry::instance().entries.push_back({name, std::move(run)});
  }
};

inline void reportFailure(const char* file, int line, const std::string& detail) {
  auto& registry = Registry::instance();
  ++registry.failures;
  std::printf("  FAIL %s:%d — %s\n", file, line, detail.c_str());
}

inline void check(bool condition, const char* expression, const char* file, int line) {
  auto& registry = Registry::instance();
  ++registry.checks;
  if (!condition) {
    reportFailure(file, line, expression);
  }
}

template <typename A, typename B>
void checkEqual(const A& actual, const B& expected, const char* expression,
                const char* file, int line) {
  auto& registry = Registry::instance();
  ++registry.checks;
  if (!(actual == expected)) {
    std::string detail = std::string(expression) + " — got '" +
                         std::to_string(actual) + "', expected '" +
                         std::to_string(expected) + "'";
    reportFailure(file, line, detail);
  }
}

inline void checkEqualString(const std::string& actual, const std::string& expected,
                             const char* expression, const char* file, int line) {
  auto& registry = Registry::instance();
  ++registry.checks;
  if (actual != expected) {
    reportFailure(file, line, std::string(expression) + " — got \"" + actual +
                                  "\", expected \"" + expected + "\"");
  }
}

inline void checkNear(double actual, double expected, double tolerance,
                      const char* expression, const char* file, int line) {
  auto& registry = Registry::instance();
  ++registry.checks;
  if (std::fabs(actual - expected) > tolerance) {
    reportFailure(file, line,
                  std::string(expression) + " — got " + std::to_string(actual) +
                      ", expected " + std::to_string(expected) + " ± " +
                      std::to_string(tolerance));
  }
}

}  // namespace weblinked::test

#define WEBLINKED_TEST(name)                                              \
  static void name();                                                     \
  static ::weblinked::test::Registrar registrar_##name(#name, name);      \
  static void name()

#define CHECK(condition) \
  ::weblinked::test::check((condition), #condition, __FILE__, __LINE__)

#define CHECK_EQ(actual, expected) \
  ::weblinked::test::checkEqual((actual), (expected), #actual " == " #expected, __FILE__, __LINE__)

#define CHECK_STR(actual, expected)                                     \
  ::weblinked::test::checkEqualString((actual), (expected),             \
                                      #actual " == " #expected, __FILE__, __LINE__)

#define CHECK_NEAR(actual, expected, tolerance)                             \
  ::weblinked::test::checkNear((actual), (expected), (tolerance),           \
                               #actual " ~= " #expected, __FILE__, __LINE__)
