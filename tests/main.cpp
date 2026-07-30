#include <cstdio>
#include <cstring>

#include "test_support.h"

int main(int argc, char** argv) {
  auto& registry = weblinked::test::Registry::instance();

  const char* filter = nullptr;
  if (argc > 1) {
    filter = argv[1];
  }

  int ran = 0;
  for (const auto& entry : registry.entries) {
    if (filter != nullptr && entry.name.find(filter) == std::string::npos) {
      continue;
    }
    registry.currentTest = entry.name;
    const int failuresBefore = registry.failures;
    std::printf("%s\n", entry.name.c_str());
    entry.run();
    ++ran;
    if (registry.failures == failuresBefore) {
      std::printf("  ok\n");
    }
  }

  std::printf("\n%d test%s, %d check%s, %d failure%s\n", ran, ran == 1 ? "" : "s",
              registry.checks, registry.checks == 1 ? "" : "s", registry.failures,
              registry.failures == 1 ? "" : "s");

  return registry.failures == 0 ? 0 : 1;
}
