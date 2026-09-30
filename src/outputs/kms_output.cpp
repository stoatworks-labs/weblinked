#include "outputs/kms_output.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

#include <drm.h>
#include <drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "diag/diag.h"

namespace weblinked {

namespace {

drmModeModeInfo* asMode(void* p) { return static_cast<drmModeModeInfo*>(p); }
drmModeCrtc* asCrtc(void* p) { return static_cast<drmModeCrtc*>(p); }

/// "HDMI-A-1", the name the kernel itself uses under /sys/class/drm. A local
/// table rather than drmModeGetConnectorTypeName(), which only arrived in
/// libdrm 2.4.108 — older than some vendor userlands this is meant to run on.
std::string connectorName(const drmModeConnector* connector) {
  static const char* const kNames[] = {
      "Unknown", "VGA",   "DVI-I", "DVI-D",   "DVI-A",     "Composite", "SVIDEO",
      "LVDS",    "Component", "DIN", "DP",    "HDMI-A",    "HDMI-B",    "TV",
      "eDP",     "Virtual", "DSI",   "DPI",   "Writeback", "SPI",       "USB"};
  const uint32_t type = connector->connector_type;
  const char* name = type < sizeof(kNames) / sizeof(kNames[0]) ? kNames[type] : "Unknown";
  return std::string(name) + "-" + std::to_string(connector->connector_type_id);
}

double refreshOf(const drmModeModeInfo& mode) {
  if (mode.htotal == 0 || mode.vtotal == 0) {
    return mode.vrefresh;
  }
  double hz = mode.clock * 1000.0 / (static_cast<double>(mode.htotal) * mode.vtotal);
  if (mode.flags & DRM_MODE_FLAG_INTERLACE) hz *= 2.0;
  return hz;
}

/// Matches "1920x1080" or "1920x1080@60" against a mode. The rate, when given,
/// is compared to the nearest whole hertz so that 59.94 answers to "@60".
bool modeMatches(const drmModeModeInfo& mode, const std::string& wanted) {
  int w = 0, h = 0, hz = 0;
  const int n = std::sscanf(wanted.c_str(), "%dx%d@%d", &w, &h, &hz);
  if (n < 2 || mode.hdisplay != w || mode.vdisplay != h) {
    return false;
  }
  if (mode.flags & DRM_MODE_FLAG_INTERLACE) {
    return false;  // never pick an interlaced mode for a CPU-drawn picture
  }
  return n < 3 || static_cast<int>(refreshOf(mode) + 0.5) == hz;
}

}  // namespace

KmsOutput::KmsOutput(const OutputSpec& spec)
    : IOutput(spec.name.empty() ? "kms" : spec.name),
      card_(spec.optionString("card", "/dev/dri/card0")),
      wantedConnector_(spec.optionString("connector")),
      wantedMode_(spec.optionString("mode")),
      fit_(blitFitFromString(spec.optionString("scaling", "fit"))) {
  // A bare index is the card number, as --kms=1 would give.
  if (!card_.empty() && card_.find_first_not_of("0123456789") == std::string::npos) {
    card_ = "/dev/dri/card" + card_;
  }
  const std::string rotation = spec.optionString("rotate", "0");
  if (!blitRotationFromString(rotation, rotation_)) {
    // The CLI refuses a bad angle before we get here; a settings file or the
    // HTTP API may not have, and upright is the least surprising fallback.
    diag::warn("kms '%s': rotate '%s' is not 0, 90, 180 or 270 — using 0",
               name_.c_str(), rotation.c_str());
    rotation_ = BlitRotation::k0;
  }
}

KmsOutput::~KmsOutput() {
  if (running_) {
    KmsOutput::stop();
  }
  teardown();
}

bool KmsOutput::openDevice(std::string& error) {
  fd_ = ::open(card_.c_str(), O_RDWR | O_CLOEXEC);
  if (fd_ < 0) {
    error = "cannot open " + card_ + ": " + std::strerror(errno);
    return false;
  }
  uint64_t dumb = 0;
  if (drmGetCap(fd_, DRM_CAP_DUMB_BUFFER, &dumb) != 0 || dumb == 0) {
    error = card_ + " does not offer dumb buffers, so it cannot be drawn without a GPU";
    return false;
  }
  // Root may take master if nobody holds it. If somebody does, this fails and
  // the modeset below would fail too — so say who is likely to blame now.
  if (drmSetMaster(fd_) != 0 && !drmIsMaster(fd_)) {
    error = "another process holds the display on " + card_ +
            " (DRM master). Stop whatever is driving it first — on a BirdDog PLAY "
            "that is PPApp, under BirdDogRunner.service";
    return false;
  }

  drmModeRes* resources = drmModeGetResources(fd_);
  if (resources == nullptr) {
    error = card_ + " is not a KMS device: " + std::strerror(errno);
    return false;
  }

  drmModeConnector* chosen = nullptr;
  std::string available;
  for (int i = 0; i < resources->count_connectors && chosen == nullptr; ++i) {
    drmModeConnector* connector = drmModeGetConnector(fd_, resources->connectors[i]);
    if (connector == nullptr) continue;
    const std::string cname = connectorName(connector);
    const bool connected =
        connector->connection == DRM_MODE_CONNECTED && connector->count_modes > 0;
    available += (available.empty() ? "" : ", ") + cname +
                 (connected ? " (connected)" : " (nothing attached)");
    const bool named = wantedConnector_.empty() || wantedConnector_ == cname;
    if (named && connected) {
      chosen = connector;
    } else {
      drmModeFreeConnector(connector);
    }
  }
  if (chosen == nullptr) {
    error = wantedConnector_.empty()
                ? "no display is attached to " + card_ + " (" + available + ")"
                : "connector " + wantedConnector_ + " has no display attached (" +
                      available + ")";
    drmModeFreeResources(resources);
    return false;
  }

  // Mode: the one asked for, else the display's preferred, else its first.
  const drmModeModeInfo* mode = nullptr;
  for (int i = 0; i < chosen->count_modes && !wantedMode_.empty(); ++i) {
    if (modeMatches(chosen->modes[i], wantedMode_)) {
      mode = &chosen->modes[i];
      break;
    }
  }
  if (mode == nullptr && !wantedMode_.empty()) {
    diag::warn("kms '%s': %s offers no %s mode — using its preferred mode",
               name_.c_str(), connectorName(chosen).c_str(), wantedMode_.c_str());
  }
  for (int i = 0; i < chosen->count_modes && mode == nullptr; ++i) {
    if (chosen->modes[i].type & DRM_MODE_TYPE_PREFERRED) mode = &chosen->modes[i];
  }
  if (mode == nullptr) mode = &chosen->modes[0];
  mode_ = new drmModeModeInfo(*mode);

  // CRTC: the one already driving this connector if there is one — that keeps
  // the handover from whatever was there before flicker-free where the driver
  // allows — otherwise the first the connector's encoders can reach.
  crtcId_ = 0;
  if (chosen->encoder_id != 0) {
    if (drmModeEncoder* encoder = drmModeGetEncoder(fd_, chosen->encoder_id)) {
      crtcId_ = encoder->crtc_id;
      drmModeFreeEncoder(encoder);
    }
  }
  for (int e = 0; e < chosen->count_encoders && crtcId_ == 0; ++e) {
    drmModeEncoder* encoder = drmModeGetEncoder(fd_, chosen->encoders[e]);
    if (encoder == nullptr) continue;
    for (int c = 0; c < resources->count_crtcs; ++c) {
      if (encoder->possible_crtcs & (1u << c)) {
        crtcId_ = resources->crtcs[c];
        break;
      }
    }
    drmModeFreeEncoder(encoder);
  }
  connectorId_ = chosen->connector_id;
  const std::string cname = connectorName(chosen);
  drmModeFreeConnector(chosen);
  drmModeFreeResources(resources);
  if (crtcId_ == 0) {
    error = "no display controller (CRTC) can drive " + cname;
    return false;
  }

  std::lock_guard<std::mutex> lock(statusMutex_);
  connectorName_ = cname;
  modeName_ = asMode(mode_)->name;
  displayWidth_ = asMode(mode_)->hdisplay;
  displayHeight_ = asMode(mode_)->vdisplay;
  refreshHz_ = refreshOf(*asMode(mode_));
  return true;
}

bool KmsOutput::createBuffer(Buffer& buffer, std::string& error) {
  drm_mode_create_dumb create{};
  create.width = static_cast<uint32_t>(displayWidth_);
  create.height = static_cast<uint32_t>(displayHeight_);
  create.bpp = 32;
  if (drmIoctl(fd_, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0) {
    error = std::string("cannot allocate a display buffer: ") + std::strerror(errno);
    return false;
  }
  buffer.handle = create.handle;
  buffer.pitch = create.pitch;
  buffer.size = create.size;
  // depth 24 / bpp 32 is XRGB8888, which every KMS driver scans out.
  if (drmModeAddFB(fd_, create.width, create.height, 24, 32, buffer.pitch,
                   buffer.handle, &buffer.fb) != 0) {
    error = std::string("cannot register a display buffer: ") + std::strerror(errno);
    return false;
  }
  drm_mode_map_dumb map{};
  map.handle = buffer.handle;
  if (drmIoctl(fd_, DRM_IOCTL_MODE_MAP_DUMB, &map) != 0) {
    error = std::string("cannot map a display buffer: ") + std::strerror(errno);
    return false;
  }
  void* memory = mmap(nullptr, buffer.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_,
                      static_cast<off_t>(map.offset));
  if (memory == MAP_FAILED) {
    error = std::string("cannot map a display buffer: ") + std::strerror(errno);
    return false;
  }
  buffer.map = static_cast<uint8_t*>(memory);
  return true;
}

void KmsOutput::destroyBuffer(Buffer& buffer) {
  if (buffer.map != nullptr) {
    munmap(buffer.map, buffer.size);
  }
  if (buffer.fb != 0) {
    drmModeRmFB(fd_, buffer.fb);
  }
  if (buffer.handle != 0) {
    drm_mode_destroy_dumb destroy{};
    destroy.handle = buffer.handle;
    drmIoctl(fd_, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
  }
  buffer = Buffer{};
}

bool KmsOutput::start(const VideoFormat& format, std::string& error) {
  if (running_) {
    return true;
  }
  if (!openDevice(error)) {
    teardown();
    return false;
  }
  for (auto& buffer : buffers_) {
    if (!createBuffer(buffer, error)) {
      teardown();
      return false;
    }
  }

  sourceWidth_ = format.width;
  sourceHeight_ = format.height;
  blit_.configure(sourceWidth_, sourceHeight_, displayWidth_, displayHeight_, rotation_,
                  fit_);
  // Black, bars and picture area alike: the blit never writes the bars, and
  // the first thing on the glass should not be whatever the allocator left.
  for (auto& buffer : buffers_) {
    std::memset(buffer.map, 0, buffer.size);
  }

  savedCrtc_ = drmModeGetCrtc(fd_, crtcId_);
  front_ = 0;
  if (drmModeSetCrtc(fd_, crtcId_, buffers_[front_].fb, 0, 0, &connectorId_, 1,
                     asMode(mode_)) != 0) {
    error = "cannot set " + modeName_ + " on " + connectorName_ + ": " + std::strerror(errno);
    teardown();
    return false;
  }

  flipPending_ = false;
  flipsSupported_ = true;
  lastSequence_ = kNothingDrawn;
  submitted_ = presented_ = dropped_ = repeats_ = 0;
  running_ = true;
  diag::info("kms '%s': %dx%d onto %s %s (%.2f Hz) on %s, rotate %s, %s", name_.c_str(),
             format.width, format.height, connectorName_.c_str(), modeName_.c_str(),
             refreshHz_, card_.c_str(), blitRotationToString(rotation_),
             blitFitToString(fit_));
  return true;
}

void KmsOutput::onPageFlip(int, unsigned int, unsigned int, unsigned int, void* data) {
  auto* self = static_cast<KmsOutput*>(data);
  self->flipPending_ = false;
  self->front_ ^= 1;
  self->presented_.fetch_add(1, std::memory_order_relaxed);
}

void KmsOutput::collectFlips(int timeoutMs) {
  while (flipPending_) {
    pollfd pfd{fd_, POLLIN, 0};
    if (::poll(&pfd, 1, timeoutMs) <= 0) {
      return;
    }
    drmEventContext context{};
    context.version = 2;
    context.page_flip_handler = &KmsOutput::onPageFlip;
    drmHandleEvent(fd_, &context);
  }
}

void KmsOutput::submit(const VideoFrame& video, const AudioBlock& audio) {
  (void)audio;
  if (!running_) {
    return;
  }
  submitted_.fetch_add(1, std::memory_order_relaxed);
  collectFlips(0);

  if (video.sequence() == lastSequence_) {
    repeats_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (video.format().width != sourceWidth_ || video.format().height != sourceHeight_) {
    return;  // a resize is in flight; the engine restarts outputs after it
  }
  if (flipPending_) {
    // The display has not taken the last frame yet. Queueing would only add a
    // frame of latency; the next tick brings a newer one anyway.
    dropped_.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  Buffer& back = buffers_[front_ ^ 1];
  const auto begin = std::chrono::steady_clock::now();
  blit_.blit(video.data(), video.rowBytes(), back.map, static_cast<int>(back.pitch));
  copyMicros_.store(std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - begin)
                        .count(),
                    std::memory_order_relaxed);
  lastSequence_ = video.sequence();

  if (flipsSupported_ &&
      drmModePageFlip(fd_, crtcId_, back.fb, DRM_MODE_PAGE_FLIP_EVENT, this) == 0) {
    flipPending_ = true;
    return;
  }
  if (flipsSupported_) {
    const int flipError = errno;
    diag::warn("kms '%s': page flip refused (%s) — falling back to a modeset per frame",
               name_.c_str(), std::strerror(flipError));
    flipsSupported_ = false;
  }
  if (drmModeSetCrtc(fd_, crtcId_, back.fb, 0, 0, &connectorId_, 1, asMode(mode_)) == 0) {
    front_ ^= 1;
    presented_.fetch_add(1, std::memory_order_relaxed);
  }
}

void KmsOutput::stop() {
  if (!running_) {
    return;
  }
  running_ = false;
  // A buffer must not be freed while a flip to it is in flight.
  collectFlips(100);
  diag::info("kms '%s': stopped after %lld frames presented", name_.c_str(),
             static_cast<long long>(presented_.load()));
  teardown();
}

void KmsOutput::teardown() {
  if (fd_ >= 0) {
    // Put back what was on the display before, so a console or the vendor's
    // own picture returns rather than a frozen last frame.
    if (drmModeCrtc* saved = asCrtc(savedCrtc_)) {
      if (saved->buffer_id != 0 && saved->mode_valid) {
        drmModeSetCrtc(fd_, saved->crtc_id, saved->buffer_id, saved->x, saved->y,
                       &connectorId_, 1, &saved->mode);
      } else {
        drmModeSetCrtc(fd_, crtcId_, 0, 0, 0, nullptr, 0, nullptr);
      }
    }
    for (auto& buffer : buffers_) {
      destroyBuffer(buffer);
    }
    drmDropMaster(fd_);
    ::close(fd_);
    fd_ = -1;
  }
  if (savedCrtc_ != nullptr) {
    drmModeFreeCrtc(asCrtc(savedCrtc_));
    savedCrtc_ = nullptr;
  }
  delete asMode(mode_);
  mode_ = nullptr;
}

json::Value KmsOutput::status() const {
  json::Value value = json::Value::object();
  value.set("kind", json::Value("kms"));
  value.set("name", json::Value(name_));
  value.set("running", json::Value(running_));
  value.set("card", json::Value(card_));
  value.set("rotate", json::Value(std::atoi(blitRotationToString(rotation_))));
  value.set("scaling", json::Value(blitFitToString(fit_)));
  {
    std::lock_guard<std::mutex> lock(statusMutex_);
    value.set("connector", json::Value(connectorName_));
    value.set("mode", json::Value(modeName_));
    value.set("display_width", json::Value(displayWidth_));
    value.set("display_height", json::Value(displayHeight_));
    value.set("refresh_hz", json::Value(refreshHz_));
  }
  value.set("frames", json::Value(submitted_.load(std::memory_order_relaxed)));
  value.set("presented", json::Value(presented_.load(std::memory_order_relaxed)));
  // Unchanged frames that needed no copy. On a static page this is nearly all
  // of them, and it is the number that says the output is idling, not stuck.
  value.set("repeats_skipped", json::Value(repeats_.load(std::memory_order_relaxed)));
  value.set("dropped", json::Value(dropped_.load(std::memory_order_relaxed)));
  value.set("copy_us", json::Value(copyMicros_.load(std::memory_order_relaxed)));
  return value;
}

std::unique_ptr<IOutput> createKmsOutput(const OutputSpec& spec) {
  return std::make_unique<KmsOutput>(spec);
}

}  // namespace weblinked
