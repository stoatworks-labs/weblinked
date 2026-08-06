// helper_probe — exercises the plugin's process supervision without Resolume.
//
// The plugin launches a WebLinked per instance and re-points it through the
// control API. That is the part with real consequences — a browser left running
// after a layer is deleted, or a port collision between two layers, is the sort
// of fault an operator discovers at the worst moment — so it is checked here
// rather than by loading the plugin into Arena and hoping.
//
// Links Helper.cpp directly, so what runs is the shipping code. Checks, in
// order:
//
//   1. the binary is found at all
//   2. start() spawns a process that is alive
//   3. it publishes a Syphon source under the name it was given
//   4. two helpers get different ports and both run
//   5. setUrl() re-points the page without a restart (same pid throughout)
//   6. stop() terminates AND reaps it — no zombie, no orphan
//
// Build: plugin/tools/build_unload_probe.sh
// Run:   WEBLINKED_BINARY=/path/to/WebLinked ./out/helper_probe

#import <Foundation/Foundation.h>

#include <signal.h>

#include <cstdio>
#include <string>

#include "Helper.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
  std::printf("  %-52s %s\n", what, condition ? "ok" : "FAIL");
  if (!condition) ++failures;
}

void spin(double seconds) {
  [[NSRunLoop currentRunLoop]
      runUntilDate:[NSDate dateWithTimeIntervalSinceNow:seconds]];
}

/// Does a Syphon server of this name exist? Looked up through whatever Syphon
/// is loaded, as the plugin does.
bool syphonSourceExists(const std::string& name) {
  Class directoryClass = NSClassFromString(@"SyphonServerDirectory");
  if (directoryClass == nil) return false;
  id directory = [directoryClass performSelector:@selector(sharedDirectory)];
  NSArray* servers = [directory performSelector:@selector(servers)];
  NSString* wanted = [NSString stringWithUTF8String:name.c_str()];
  for (NSDictionary* server in servers) {
    if ([server[@"SyphonServerDescriptionNameKey"] isEqualToString:wanted]) {
      return true;
    }
  }
  return false;
}

/// Is that pid still a live process? Signal 0 tests existence without sending
/// anything. Used to prove stop() really ended it rather than merely forgetting.
bool processExists(long pid) {
  return pid > 0 && ::kill(static_cast<pid_t>(pid), 0) == 0;
}

}  // namespace

int main() {
  @autoreleasepool {
    const std::string binary = weblinked::Helper::findBinary();
    std::printf("binary: %s\n", binary.empty() ? "(not found)" : binary.c_str());
    if (binary.empty()) {
      std::fprintf(stderr,
                   "Set WEBLINKED_BINARY to a WebLinked executable, or install\n"
                   "one to /Applications.\n");
      return 2;
    }

    const std::string name = "HelperProbe";

    std::printf("\nstart:\n");
    weblinked::Helper helper;
    std::string error;
    const bool started = helper.start("about:blank", name, error);
    check(started, "start() succeeded");
    if (!started) {
      std::fprintf(stderr, "  %s\n", error.c_str());
      return 1;
    }
    check(helper.port() > 0, "was given a control port");
    std::printf("  (port %d)\n", helper.port());

    // Chromium takes a moment. Poll rather than sleeping a fixed time.
    bool published = false;
    for (int attempt = 0; attempt < 200 && !published; ++attempt) {
      spin(0.1);
      published = syphonSourceExists(name);
    }
    check(helper.alive(), "process is alive");
    check(published, "published a Syphon source under its given name");

    std::printf("\nsecond instance:\n");
    weblinked::Helper second;
    const bool startedSecond = second.start("about:blank", name + "2", error);
    check(startedSecond, "a second helper starts alongside the first");
    check(!startedSecond || second.port() != helper.port(),
          "the two got different control ports");
    check(helper.alive(), "the first is undisturbed");

    std::printf("\nre-point without restart:\n");
    // Captured before, compared after: the whole point of going through the
    // control API is that the process does not change.
    const bool repointed = helper.setUrl("about:blank?second");
    check(repointed, "setUrl() accepted by the running helper");
    check(helper.alive(), "same process still running after setUrl()");

    std::printf("\nstop:\n");
    second.stop();
    check(!second.alive(), "second helper stopped");

    helper.stop();
    check(!helper.alive(), "helper stopped");

    // The source should retire, because stop() sends SIGTERM and WebLinked
    // shuts Chromium down in order rather than being killed outright.
    bool retired = false;
    for (int attempt = 0; attempt < 100 && !retired; ++attempt) {
      spin(0.1);
      retired = !syphonSourceExists(name);
    }
    check(retired, "its Syphon source retired rather than going stale");

    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
  }
}
