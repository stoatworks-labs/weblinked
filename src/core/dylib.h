#pragma once

#include <string>
#include <vector>

namespace weblinked {

/// A shared library opened at run time.
///
/// Three of the four output SDKs are reached this way. It is not laziness:
/// libndi may not be redistributed with an application, libomt ships only for
/// some platforms, and an operator with no DeckLink card should still be able
/// to run the NDI path without installing anything. Resolving these at load
/// time instead would mean one missing dylib takes the whole application down.
class Dylib {
 public:
  Dylib() = default;
  ~Dylib();

  Dylib(const Dylib&) = delete;
  Dylib& operator=(const Dylib&) = delete;
  Dylib(Dylib&& other) noexcept;
  Dylib& operator=(Dylib&& other) noexcept;

  /// Tries each candidate in order and keeps the first that opens. Candidates
  /// may be bare library names (resolved by the platform loader) or absolute
  /// paths.
  bool open(const std::vector<std::string>& candidates);

  void close();

  bool isOpen() const { return handle_ != nullptr; }

  /// Path or name that actually opened, for logging and the control API.
  const std::string& loadedPath() const { return loadedPath_; }

  /// Why the last open() failed, joined across candidates.
  const std::string& lastError() const { return lastError_; }

  void* rawSymbol(const char* name) const;

  /// Resolves a symbol into a function pointer. Returns false and leaves `out`
  /// untouched if the symbol is missing, so a caller can treat a partially
  /// implemented library as unavailable rather than crashing on first call.
  template <typename Fn>
  bool symbol(const char* name, Fn& out) const {
    void* address = rawSymbol(name);
    if (address == nullptr) {
      return false;
    }
    out = reinterpret_cast<Fn>(address);
    return true;
  }

  /// Directories worth searching for a vendor library, ahead of the platform's
  /// own search path: next to the executable, and inside a macOS bundle's
  /// Frameworks directory.
  static std::vector<std::string> localSearchPaths();

 private:
  void* handle_ = nullptr;
  std::string loadedPath_;
  std::string lastError_;
};

}  // namespace weblinked
