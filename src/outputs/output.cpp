#include "outputs/output.h"

#include <algorithm>

namespace weblinked {

std::string OutputSpec::optionString(const std::string& key,
                                     const std::string& fallback) const {
  return options[key].asString(fallback);
}

int OutputSpec::optionInt(const std::string& key, int fallback) const {
  return options[key].isNumber() ? options[key].asInt(fallback) : fallback;
}

bool OutputSpec::optionBool(const std::string& key, bool fallback) const {
  return options[key].isNull() ? fallback : options[key].asBool(fallback);
}

OutputSpec outputSpecFromConfig(const OutputConfig& config) {
  OutputSpec spec;
  spec.kind = config.kind;
  spec.name = config.name;
  spec.deviceIndex = config.deviceIndex;
  spec.options = config.options;
  spec.background = config.background;
  return spec;
}

OutputConfig outputConfigFromSpec(const OutputSpec& spec) {
  OutputConfig config;
  config.kind = spec.kind;
  config.name = spec.name;
  config.deviceIndex = spec.deviceIndex;
  config.options = spec.options;
  config.background = spec.background;
  return config;
}

std::vector<std::string> compiledOutputKinds() {
  std::vector<std::string> kinds{"preview"};
#if defined(WEBLINKED_WITH_NDI)
  kinds.push_back("ndi");
#endif
#if defined(WEBLINKED_WITH_OMT)
  kinds.push_back("omt");
#endif
#if defined(WEBLINKED_WITH_DECKLINK)
  kinds.push_back("decklink");
#endif
#if defined(WEBLINKED_WITH_AJA)
  kinds.push_back("aja");
#endif
#if defined(WEBLINKED_WITH_SCREEN)
  kinds.push_back("screen");
#endif
#if defined(WEBLINKED_WITH_SHARED)
  kinds.push_back("shared");
#endif
#if defined(WEBLINKED_WITH_STREAM)
  kinds.push_back("stream");
#endif
  return kinds;
}

std::unique_ptr<IOutput> createOutput(const OutputSpec& spec, std::string& error) {
  if (spec.kind == "preview") {
    return createPreviewOutput(spec);
  }
#if defined(WEBLINKED_WITH_NDI)
  if (spec.kind == "ndi") {
    return createNdiOutput(spec);
  }
#endif
#if defined(WEBLINKED_WITH_OMT)
  if (spec.kind == "omt") {
    return createOmtOutput(spec);
  }
#endif
#if defined(WEBLINKED_WITH_DECKLINK)
  if (spec.kind == "decklink") {
    return createDeckLinkOutput(spec);
  }
#endif
#if defined(WEBLINKED_WITH_AJA)
  if (spec.kind == "aja") {
    return createAjaOutput(spec);
  }
#endif
#if defined(WEBLINKED_WITH_SCREEN)
  if (spec.kind == "screen") {
    return createScreenOutput(spec);
  }
#endif
#if defined(WEBLINKED_WITH_SHARED)
  if (spec.kind == "shared") {
    return createSharedOutput(spec);
  }
#endif
#if defined(WEBLINKED_WITH_STREAM)
  if (spec.kind == "stream") {
    return createStreamOutput(spec);
  }
#endif

  // Distinguish "never heard of it" from "this build has no support for it",
  // because the second is a build problem and the first is a typo.
  const auto compiled = compiledOutputKinds();
  const bool known = spec.kind == "ndi" || spec.kind == "omt" ||
                     spec.kind == "decklink" || spec.kind == "aja" ||
                     spec.kind == "screen" || spec.kind == "shared" ||
                     spec.kind == "stream" || spec.kind == "preview";
  if (known) {
    error = "output kind '" + spec.kind + "' was not compiled into this build";
  } else {
    error = "unknown output kind '" + spec.kind + "'";
  }
  return nullptr;
}

}  // namespace weblinked
