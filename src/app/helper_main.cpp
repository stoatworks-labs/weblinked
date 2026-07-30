// The macOS helper process.
//
// One tiny executable, copied into five differently-named .app bundles inside
// WebLinked.app/Contents/Frameworks. CEF picks whichever it needs — renderer,
// GPU, utility, alerts — by bundle name, so all this has to do is hand control
// straight back to CEF.
//
// Nothing else belongs here. This process must not touch the engine, open a
// socket or install a crash handler: it runs sandboxed page content, and the
// browser process owns the diagnostics.

#include "include/cef_app.h"
#include "include/wrapper/cef_library_loader.h"

int main(int argc, char** argv) {
  // LoadInHelper, not LoadInMain: the framework sits one level further up from
  // a helper bundle than from the main app.
  CefScopedLibraryLoader libraryLoader;
  if (!libraryLoader.LoadInHelper()) {
    return 1;
  }

  CefMainArgs mainArgs(argc, argv);
  return CefExecuteProcess(mainArgs, nullptr, nullptr);
}
